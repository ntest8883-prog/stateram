#define _WIN32_WINNT 0x0602
#define PSAPI_VERSION 1
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "Psapi.lib")

static constexpr size_t PAGE_BYTES = 4096;

static double now_ms() {
    LARGE_INTEGER f{}, q{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return 1000.0 * static_cast<double>(q.QuadPart) / static_cast<double>(f.QuadPart);
}

struct SharedState {
    volatile LONG command;      // 1 = touch, 2 = exit
    volatile LONG child_ok;
    volatile LONG round;
    double last_ms;
    uint64_t checksum;
    uint64_t allocation_base;
    uint64_t allocation_bytes;
};

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

static std::string narrow(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

static uint8_t page_value(uint64_t page_index) {
    uint64_t x = page_index * 0x9E3779B97F4A7C15ull + 0x535441544552414Dull;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return static_cast<uint8_t>(x & 0xffu);
}

struct Names {
    std::wstring ready;
    std::wstring go;
    std::wstring done;
    std::wstring mapping;
};

static Names make_names(const std::wstring& prefix) {
    return {
        L"Local\\" + prefix + L"_ready",
        L"Local\\" + prefix + L"_go",
        L"Local\\" + prefix + L"_done",
        L"Local\\" + prefix + L"_map"
    };
}

static int child_main(int mb, const std::wstring& prefix) {
    Names n = make_names(prefix);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, n.ready.c_str());
    HANDLE go = OpenEventW(SYNCHRONIZE, FALSE, n.go.c_str());
    HANDLE done = OpenEventW(EVENT_MODIFY_STATE, FALSE, n.done.c_str());
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, n.mapping.c_str());

    if (!ready || !go || !done || !map) {
        std::cerr << "child IPC open failed: " << GetLastError() << "\n";
        return 20;
    }

    auto* shared = static_cast<SharedState*>(
        MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState))
    );
    if (!shared) {
        std::cerr << "child MapViewOfFile failed: " << GetLastError() << "\n";
        return 21;
    }

    const size_t bytes = static_cast<size_t>(mb) * 1024ull * 1024ull;
    auto* mem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
    );
    if (!mem) {
        std::cerr << "child VirtualAlloc failed: " << GetLastError() << "\n";
        return 22;
    }

    const size_t pages = bytes / PAGE_BYTES;
    for (size_t i = 0; i < pages; ++i) {
        mem[i * PAGE_BYTES] = page_value(i);
    }

    shared->allocation_base = reinterpret_cast<uint64_t>(mem);
    shared->allocation_bytes = bytes;
    shared->child_ok = 1;
    SetEvent(ready);

    for (;;) {
        DWORD wr = WaitForSingleObject(go, INFINITE);
        if (wr != WAIT_OBJECT_0) return 23;

        LONG cmd = InterlockedCompareExchange(&shared->command, 0, 0);
        if (cmd == 2) break;

        double t0 = now_ms();
        uint64_t sum = 0;
        for (size_t i = 0; i < pages; ++i) {
            sum += mem[i * PAGE_BYTES];
        }
        double dt = now_ms() - t0;

        shared->checksum = sum;
        shared->last_ms = dt;
        InterlockedIncrement(&shared->round);
        SetEvent(done);
    }

    VirtualFree(mem, 0, MEM_RELEASE);
    UnmapViewOfFile(shared);
    CloseHandle(map);
    CloseHandle(ready);
    CloseHandle(go);
    CloseHandle(done);
    return 0;
}

static bool query_hot_pages(HANDLE process,
                            uint64_t region_base,
                            uint64_t region_bytes,
                            std::vector<uint64_t>& pages_out) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(process,
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                              sizeof(pmc))) {
        return false;
    }

    size_t estimated_pages = pmc.WorkingSetSize / PAGE_BYTES + 32768;
    size_t buf_bytes = sizeof(PSAPI_WORKING_SET_INFORMATION) +
        estimated_pages * sizeof(PSAPI_WORKING_SET_BLOCK);

    std::vector<unsigned char> buf(buf_bytes);
    bool ok = false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        if (QueryWorkingSet(process, buf.data(), static_cast<DWORD>(buf.size()))) {
            ok = true;
            break;
        }
        DWORD err = GetLastError();
        if (err != ERROR_BAD_LENGTH && err != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }
        buf.resize(buf.size() * 2);
    }
    if (!ok) return false;

    auto* ws = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buf.data());
    pages_out.clear();
    pages_out.reserve(static_cast<size_t>(ws->NumberOfEntries));

    const uint64_t end = region_base + region_bytes;
    for (ULONG_PTR i = 0; i < ws->NumberOfEntries; ++i) {
        uint64_t addr =
            static_cast<uint64_t>(ws->WorkingSetInfo[i].VirtualPage) * PAGE_BYTES;
        if (addr >= region_base && addr < end) {
            pages_out.push_back(addr);
        }
    }

    std::sort(pages_out.begin(), pages_out.end());
    pages_out.erase(std::unique(pages_out.begin(), pages_out.end()), pages_out.end());
    return true;
}

static std::vector<WIN32_MEMORY_RANGE_ENTRY>
make_ranges(const std::vector<uint64_t>& pages, size_t cap_bytes, size_t& selected_bytes) {
    std::vector<WIN32_MEMORY_RANGE_ENTRY> ranges;
    selected_bytes = 0;
    if (pages.empty() || cap_bytes < PAGE_BYTES) return ranges;

    uint64_t start = pages[0];
    uint64_t prev = pages[0];

    auto emit = [&](uint64_t s, uint64_t e) {
        if (selected_bytes >= cap_bytes) return;
        size_t bytes = static_cast<size_t>(e - s + PAGE_BYTES);
        size_t remaining = cap_bytes - selected_bytes;
        if (bytes > remaining) bytes = (remaining / PAGE_BYTES) * PAGE_BYTES;
        if (bytes == 0) return;
        WIN32_MEMORY_RANGE_ENTRY x{};
        x.VirtualAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(s));
        x.NumberOfBytes = bytes;
        ranges.push_back(x);
        selected_bytes += bytes;
    };

    for (size_t i = 1; i < pages.size(); ++i) {
        if (pages[i] == prev + PAGE_BYTES) {
            prev = pages[i];
            continue;
        }
        emit(start, prev);
        if (selected_bytes >= cap_bytes) return ranges;
        start = prev = pages[i];
    }
    emit(start, prev);
    return ranges;
}

static bool run_child_touch(HANDLE go, HANDLE done, SharedState* shared,
                            double& ms, uint64_t& checksum) {
    InterlockedExchange(&shared->command, 1);
    ResetEvent(done);
    if (!SetEvent(go)) return false;
    if (WaitForSingleObject(done, 30000) != WAIT_OBJECT_0) return false;
    ms = shared->last_ms;
    checksum = shared->checksum;
    return true;
}

static int parent_main(int mb, int prefetch_mb, const std::string& json_path) {
    std::wstring prefix =
        L"StateRAM3_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64());
    Names n = make_names(prefix);

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, n.ready.c_str());
    HANDLE go = CreateEventW(nullptr, FALSE, FALSE, n.go.c_str());
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, n.done.c_str());
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, sizeof(SharedState), n.mapping.c_str());

    if (!ready || !go || !done || !map) {
        throw std::runtime_error("parent IPC creation failed");
    }

    auto* shared = static_cast<SharedState*>(
        MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState))
    );
    if (!shared) throw std::runtime_error("parent MapViewOfFile failed");
    ZeroMemory(shared, sizeof(*shared));

    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        throw std::runtime_error("GetModuleFileName failed");
    }

    std::wstringstream cmd;
    cmd << L"\"" << exe << L"\" --child --mb " << mb
        << L" --prefix \"" << prefix << L"\"";
    std::wstring cmdline = cmd.str();
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("CreateProcess failed: " +
                                 std::to_string(GetLastError()));
    }
    CloseHandle(pi.hThread);

    if (WaitForSingleObject(ready, 30000) != WAIT_OBJECT_0 || !shared->child_ok) {
        TerminateProcess(pi.hProcess, 99);
        throw std::runtime_error("child did not become ready");
    }

    std::vector<uint64_t> hot_pages;
    if (!query_hot_pages(pi.hProcess,
                         shared->allocation_base,
                         shared->allocation_bytes,
                         hot_pages)) {
        TerminateProcess(pi.hProcess, 98);
        throw std::runtime_error("QueryWorkingSet hotset capture failed");
    }

    size_t selected_bytes = 0;
    auto ranges = make_ranges(
        hot_pages,
        static_cast<size_t>(prefetch_mb) * 1024ull * 1024ull,
        selected_bytes
    );

    // Probe the native low-memory notification mechanism that the eventual
    // real-PC governor will use. No memory setting is changed.
    HANDLE low_mem = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    BOOL low_state = FALSE;
    bool low_notification_ok =
        low_mem && QueryMemoryResourceNotification(low_mem, &low_state);

    // CONTROL: remove only this synthetic child workload's working set.
    // This is deliberately confined to the disposable GitHub runner.
    bool trim_control_ok = EmptyWorkingSet(pi.hProcess) != FALSE;
    Sleep(100);

    double control_ms = 0.0;
    uint64_t control_checksum = 0;
    if (!run_child_touch(go, done, shared, control_ms, control_checksum)) {
        TerminateProcess(pi.hProcess, 97);
        throw std::runtime_error("control child touch failed");
    }

    // Capture the hot pages again after the control reactivation.
    if (!query_hot_pages(pi.hProcess,
                         shared->allocation_base,
                         shared->allocation_bytes,
                         hot_pages)) {
        TerminateProcess(pi.hProcess, 96);
        throw std::runtime_error("second QueryWorkingSet failed");
    }
    ranges = make_ranges(
        hot_pages,
        static_cast<size_t>(prefetch_mb) * 1024ull * 1024ull,
        selected_bytes
    );

    bool trim_prefetch_ok = EmptyWorkingSet(pi.hProcess) != FALSE;
    Sleep(100);

    BOOL prefetch_ok = FALSE;
    DWORD prefetch_error = ERROR_SUCCESS;
    double prefetch_call_ms = 0.0;

    if (!ranges.empty()) {
        double t0 = now_ms();
        prefetch_ok = PrefetchVirtualMemory(
            pi.hProcess,
            static_cast<ULONG_PTR>(ranges.size()),
            ranges.data(),
            0
        );
        prefetch_call_ms = now_ms() - t0;
        if (!prefetch_ok) prefetch_error = GetLastError();
    }

    // Give the hint a small head start before the synthetic app becomes active.
    Sleep(100);

    double prefetched_ms = 0.0;
    uint64_t prefetched_checksum = 0;
    if (!run_child_touch(go, done, shared, prefetched_ms, prefetched_checksum)) {
        TerminateProcess(pi.hProcess, 95);
        throw std::runtime_error("prefetched child touch failed");
    }

    InterlockedExchange(&shared->command, 2);
    SetEvent(go);
    WaitForSingleObject(pi.hProcess, 10000);

    DWORD child_exit = 999;
    GetExitCodeProcess(pi.hProcess, &child_exit);

    const double captured_mb =
        static_cast<double>(selected_bytes) / (1024.0 * 1024.0);
    const double improvement_pct =
        control_ms > 0.0 ? 100.0 * (control_ms - prefetched_ms) / control_ms : 0.0;

    bool pass =
        child_exit == 0 &&
        trim_control_ok &&
        trim_prefetch_ok &&
        low_notification_ok &&
        !ranges.empty() &&
        captured_mb >= std::min(64, prefetch_mb) &&
        prefetch_ok &&
        control_checksum != 0 &&
        prefetched_checksum == control_checksum;

    std::ofstream out(json_path, std::ios::binary);
    out << "{\n";
    out << "  \"phase\": \"Windows Phase 3 app-aware prewarm\",\n";
    out << "  \"child_mb\": " << mb << ",\n";
    out << "  \"captured_hotset_mb\": " << std::fixed << std::setprecision(3)
        << captured_mb << ",\n";
    out << "  \"hot_pages_seen\": " << hot_pages.size() << ",\n";
    out << "  \"prefetch_ranges\": " << ranges.size() << ",\n";
    out << "  \"low_memory_notification_api_ok\": "
        << (low_notification_ok ? "true" : "false") << ",\n";
    out << "  \"low_memory_signal_now\": "
        << (low_state ? "true" : "false") << ",\n";
    out << "  \"control_trim_ok\": "
        << (trim_control_ok ? "true" : "false") << ",\n";
    out << "  \"prefetch_trim_ok\": "
        << (trim_prefetch_ok ? "true" : "false") << ",\n";
    out << "  \"prefetch_api_ok\": "
        << (prefetch_ok ? "true" : "false") << ",\n";
    out << "  \"prefetch_error\": " << prefetch_error << ",\n";
    out << "  \"prefetch_call_ms\": " << prefetch_call_ms << ",\n";
    out << "  \"control_reactivation_ms\": " << control_ms << ",\n";
    out << "  \"prefetched_reactivation_ms\": " << prefetched_ms << ",\n";
    out << "  \"relative_improvement_pct_informational\": "
        << improvement_pct << ",\n";
    out << "  \"control_checksum\": " << control_checksum << ",\n";
    out << "  \"prefetched_checksum\": " << prefetched_checksum << ",\n";
    out << "  \"child_exit_code\": " << child_exit << ",\n";
    out << "  \"pass\": " << (pass ? "true" : "false") << "\n";
    out << "}\n";
    out.close();

    std::cout << "StateRAM Windows Phase 3 - app-aware prewarm\n";
    std::cout << " captured hotset: " << captured_mb << " MiB in "
              << ranges.size() << " ranges\n";
    std::cout << " control reactivation: " << control_ms << " ms\n";
    std::cout << " prefetch API call: " << prefetch_call_ms << " ms\n";
    std::cout << " prefetched reactivation: " << prefetched_ms << " ms\n";
    std::cout << " improvement (informational only): "
              << improvement_pct << "%\n";
    std::cout << " prefetch API: " << (prefetch_ok ? "OK" : "FAIL")
              << " error=" << prefetch_error << "\n";
    std::cout << " integrity: "
              << ((control_checksum == prefetched_checksum && control_checksum != 0)
                  ? "OK" : "FAIL") << "\n";
    std::cout << " WINDOWS_STATERAM_PHASE3=" << (pass ? "PASS" : "FAIL") << "\n";

    if (low_mem) CloseHandle(low_mem);
    CloseHandle(pi.hProcess);
    UnmapViewOfFile(shared);
    CloseHandle(map);
    CloseHandle(ready);
    CloseHandle(go);
    CloseHandle(done);

    return pass ? 0 : 10;
}

int main(int argc, char** argv) {
    try {
        bool child = false;
        int mb = 256;
        int prefetch_mb = 192;
        std::wstring prefix;
        std::string json_path = "windows_phase3.json";

        for (int i = 1; i < argc; ++i) {
            std::string k = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + k);
                return argv[++i];
            };
            if (k == "--child") child = true;
            else if (k == "--parent") child = false;
            else if (k == "--mb") mb = std::stoi(next());
            else if (k == "--prefetch-mb") prefetch_mb = std::stoi(next());
            else if (k == "--prefix") prefix = widen(next());
            else if (k == "--json") json_path = next();
            else throw std::runtime_error("unknown argument: " + k);
        }

        if (child) {
            if (prefix.empty()) throw std::runtime_error("child missing --prefix");
            return child_main(mb, prefix);
        }
        return parent_main(mb, prefetch_mb, json_path);

    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        return 2;
    }
}
