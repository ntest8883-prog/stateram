#define _WIN32_WINNT 0x0602
#define PSAPI_VERSION 1
#define NOMINMAX

#include <windows.h>
#include <psapi.h>
#include <compressapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Cabinet.lib")

static constexpr size_t PAGE_BYTES = 4096;
static constexpr size_t UNIT_BYTES = 256 * 1024;

static double now_ms() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return 1000.0 * static_cast<double>(q.QuadPart) /
           static_cast<double>(freq.QuadPart);
}

static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static void fill_page(uint8_t* page, uint64_t page_index) {
    // 25% deliberately hard-to-compress pages.
    if ((page_index & 3ull) == 0) {
        auto* words = reinterpret_cast<uint64_t*>(page);
        for (size_t i = 0; i < PAGE_BYTES / 8; ++i) {
            words[i] = splitmix64(page_index * 0xD6E8FEB86659FD93ull + i);
        }
        return;
    }

    // 75% structured pages: highly reconstructible/compressible but not zero.
    uint8_t base = static_cast<uint8_t>((page_index * 29 + 17) & 0xffu);
    std::memset(page, base, PAGE_BYTES);
    for (size_t off = 0; off < PAGE_BYTES; off += 512) {
        uint64_t x = splitmix64(page_index * 0xA0761D6478BD642Full + off);
        std::memcpy(page + off, &x, sizeof(x));
    }
}

static uint64_t fnv1a64(const uint8_t* data, size_t bytes) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

struct SharedState {
    volatile LONG command; // 1=verify, 2=exit
    volatile LONG ok;
    uint64_t base;
    uint64_t bytes;
    uint64_t initial_hash;
    uint64_t final_hash;
    double verify_ms;
};

struct Names {
    std::wstring ready;
    std::wstring go;
    std::wstring done;
    std::wstring map;
};

static Names make_names(const std::wstring& prefix) {
    return {
        L"Local\\" + prefix + L"_ready",
        L"Local\\" + prefix + L"_go",
        L"Local\\" + prefix + L"_done",
        L"Local\\" + prefix + L"_map"
    };
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

struct ProcMem {
    double working_set_mb = 0.0;
    double private_mb = 0.0;
};

static ProcMem proc_mem(HANDLE process) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    ProcMem out{};
    if (GetProcessMemoryInfo(
            process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc))) {
        out.working_set_mb =
            static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        out.private_mb =
            static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    }
    return out;
}

static int child_main(int mb, const std::wstring& prefix) {
    Names n = make_names(prefix);

    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, n.ready.c_str());
    HANDLE go = OpenEventW(SYNCHRONIZE, FALSE, n.go.c_str());
    HANDLE done = OpenEventW(EVENT_MODIFY_STATE, FALSE, n.done.c_str());
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, n.map.c_str());

    if (!ready || !go || !done || !map) {
        std::cerr << "child IPC open failed " << GetLastError() << "\n";
        return 20;
    }

    auto* shared = static_cast<SharedState*>(
        MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState))
    );
    if (!shared) return 21;

    const size_t bytes = static_cast<size_t>(mb) * 1024ull * 1024ull;
    auto* arena = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
    );
    if (!arena) {
        std::cerr << "child VirtualAlloc failed " << GetLastError() << "\n";
        return 22;
    }

    const size_t pages = bytes / PAGE_BYTES;
    for (size_t p = 0; p < pages; ++p) {
        fill_page(arena + p * PAGE_BYTES, p);
    }

    const uint64_t initial = fnv1a64(arena, bytes);

    shared->base = reinterpret_cast<uint64_t>(arena);
    shared->bytes = bytes;
    shared->initial_hash = initial;
    shared->final_hash = 0;
    shared->verify_ms = 0.0;
    InterlockedExchange(&shared->ok, 1);
    SetEvent(ready);

    for (;;) {
        if (WaitForSingleObject(go, INFINITE) != WAIT_OBJECT_0) return 23;
        LONG cmd = InterlockedCompareExchange(&shared->command, 0, 0);
        if (cmd == 2) break;

        double t0 = now_ms();
        uint64_t h = fnv1a64(arena, bytes);
        shared->verify_ms = now_ms() - t0;
        shared->final_hash = h;
        SetEvent(done);
    }

    VirtualFree(arena, 0, MEM_RELEASE);
    UnmapViewOfFile(shared);
    CloseHandle(map);
    CloseHandle(ready);
    CloseHandle(go);
    CloseHandle(done);
    return 0;
}

struct CapsuleUnit {
    bool compressed = false;
    std::vector<uint8_t> payload;
};

static bool compress_unit(
    COMPRESSOR_HANDLE compressor,
    const std::vector<uint8_t>& raw,
    CapsuleUnit& out
) {
    SIZE_T needed = 0;
    BOOL first = Compress(
        compressor,
        raw.data(),
        raw.size(),
        nullptr,
        0,
        &needed
    );

    if (first) {
        // Empty output is not expected for a nonempty 256 KiB block.
        return false;
    }

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) {
        return false;
    }

    std::vector<uint8_t> tmp(needed);
    SIZE_T got = 0;
    if (!Compress(
            compressor,
            raw.data(),
            raw.size(),
            tmp.data(),
            tmp.size(),
            &got)) {
        return false;
    }
    tmp.resize(got);

    if (tmp.size() < raw.size()) {
        out.compressed = true;
        out.payload = std::move(tmp);
    } else {
        out.compressed = false;
        out.payload = raw;
    }
    return true;
}

static bool decompress_unit(
    DECOMPRESSOR_HANDLE decompressor,
    const CapsuleUnit& unit,
    std::vector<uint8_t>& raw
) {
    if (!unit.compressed) {
        raw = unit.payload;
        return raw.size() == UNIT_BYTES;
    }

    raw.assign(UNIT_BYTES, 0);
    SIZE_T got = 0;
    if (!Decompress(
            decompressor,
            unit.payload.data(),
            unit.payload.size(),
            raw.data(),
            raw.size(),
            &got)) {
        return false;
    }
    return got == UNIT_BYTES;
}

static bool region_is_reserved(HANDLE process, uint64_t base) {
    MEMORY_BASIC_INFORMATION mbi{};
    SIZE_T n = VirtualQueryEx(
        process,
        reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(base)),
        &mbi,
        sizeof(mbi)
    );
    return n == sizeof(mbi) && mbi.State == MEM_RESERVE;
}

static int parent_main(int mb, const std::string& json_path) {
    const size_t bytes = static_cast<size_t>(mb) * 1024ull * 1024ull;
    if (bytes == 0 || (bytes % UNIT_BYTES) != 0) {
        throw std::runtime_error("--mb must produce a nonzero multiple of 256 KiB");
    }

    std::wstring prefix =
        L"StateRAM4_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64());
    Names n = make_names(prefix);

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, n.ready.c_str());
    HANDLE go = CreateEventW(nullptr, FALSE, FALSE, n.go.c_str());
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, n.done.c_str());
    HANDLE map = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(SharedState), n.map.c_str()
    );

    if (!ready || !go || !done || !map) {
        throw std::runtime_error("parent IPC creation failed");
    }

    auto* shared = static_cast<SharedState*>(
        MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState))
    );
    if (!shared) throw std::runtime_error("parent map view failed");
    ZeroMemory(shared, sizeof(*shared));

    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        throw std::runtime_error("GetModuleFileNameW failed");
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

    if (!CreateProcessW(
            nullptr,
            mutable_cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        throw std::runtime_error(
            "CreateProcessW failed " + std::to_string(GetLastError()));
    }
    CloseHandle(pi.hThread);

    if (WaitForSingleObject(ready, 30000) != WAIT_OBJECT_0 ||
        InterlockedCompareExchange(&shared->ok, 0, 0) != 1) {
        TerminateProcess(pi.hProcess, 90);
        throw std::runtime_error("child did not become ready");
    }

    const uint64_t base = shared->base;
    const uint64_t region_bytes = shared->bytes;
    const uint64_t expected_hash = shared->initial_hash;

    ProcMem before = proc_mem(pi.hProcess);

    COMPRESSOR_HANDLE compressor = nullptr;
    DECOMPRESSOR_HANDLE decompressor = nullptr;

    const DWORD alg = COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW;

    if (!CreateCompressor(alg, nullptr, &compressor)) {
        TerminateProcess(pi.hProcess, 91);
        throw std::runtime_error(
            "CreateCompressor failed " + std::to_string(GetLastError()));
    }
    if (!CreateDecompressor(alg, nullptr, &decompressor)) {
        CloseCompressor(compressor);
        TerminateProcess(pi.hProcess, 92);
        throw std::runtime_error(
            "CreateDecompressor failed " + std::to_string(GetLastError()));
    }

    const size_t unit_count =
        static_cast<size_t>(region_bytes / UNIT_BYTES);

    std::vector<CapsuleUnit> capsule;
    capsule.resize(unit_count);
    std::vector<uint8_t> raw(UNIT_BYTES);

    uint64_t capsule_bytes = 0;
    size_t compressed_units = 0;
    size_t raw_units = 0;

    double snapshot_t0 = now_ms();
    bool snapshot_ok = true;

    for (size_t u = 0; u < unit_count; ++u) {
        SIZE_T got = 0;
        const void* remote =
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(base + u * UNIT_BYTES));

        if (!ReadProcessMemory(
                pi.hProcess,
                remote,
                raw.data(),
                raw.size(),
                &got) ||
            got != raw.size()) {
            snapshot_ok = false;
            break;
        }

        if (!compress_unit(compressor, raw, capsule[u])) {
            snapshot_ok = false;
            break;
        }

        capsule_bytes += capsule[u].payload.size();
        if (capsule[u].compressed) ++compressed_units;
        else ++raw_units;
    }

    double snapshot_ms = now_ms() - snapshot_t0;

    bool decommit_ok = false;
    bool reserved_after_decommit = false;
    double decommit_ms = 0.0;

    if (snapshot_ok) {
        double t0 = now_ms();
        decommit_ok = VirtualFreeEx(
            pi.hProcess,
            reinterpret_cast<LPVOID>(static_cast<uintptr_t>(base)),
            static_cast<SIZE_T>(region_bytes),
            MEM_DECOMMIT
        ) != FALSE;
        decommit_ms = now_ms() - t0;

        if (decommit_ok) {
            reserved_after_decommit =
                region_is_reserved(pi.hProcess, base);
        }
    }

    Sleep(100);
    ProcMem dormant = proc_mem(pi.hProcess);

    bool recommit_ok = false;
    bool restore_ok = false;
    double restore_ms = 0.0;

    if (decommit_ok && reserved_after_decommit) {
        double t0 = now_ms();

        void* recommitted = VirtualAllocEx(
            pi.hProcess,
            reinterpret_cast<LPVOID>(static_cast<uintptr_t>(base)),
            static_cast<SIZE_T>(region_bytes),
            MEM_COMMIT,
            PAGE_READWRITE
        );

        recommit_ok =
            recommitted ==
            reinterpret_cast<void*>(static_cast<uintptr_t>(base));

        if (recommit_ok) {
            restore_ok = true;
            for (size_t u = 0; u < unit_count; ++u) {
                if (!decompress_unit(
                        decompressor,
                        capsule[u],
                        raw)) {
                    restore_ok = false;
                    break;
                }

                SIZE_T wrote = 0;
                void* remote =
                    reinterpret_cast<void*>(
                        static_cast<uintptr_t>(base + u * UNIT_BYTES));

                if (!WriteProcessMemory(
                        pi.hProcess,
                        remote,
                        raw.data(),
                        raw.size(),
                        &wrote) ||
                    wrote != raw.size()) {
                    restore_ok = false;
                    break;
                }
            }
        }

        restore_ms = now_ms() - t0;
    }

    ProcMem restored = proc_mem(pi.hProcess);

    uint64_t final_hash = 0;
    double child_verify_ms = 0.0;
    bool child_verify_ok = false;

    if (restore_ok) {
        InterlockedExchange(&shared->command, 1);
        ResetEvent(done);
        SetEvent(go);

        if (WaitForSingleObject(done, 30000) == WAIT_OBJECT_0) {
            final_hash = shared->final_hash;
            child_verify_ms = shared->verify_ms;
            child_verify_ok =
                final_hash == expected_hash && final_hash != 0;
        }
    }

    InterlockedExchange(&shared->command, 2);
    SetEvent(go);
    WaitForSingleObject(pi.hProcess, 10000);

    DWORD child_exit = 999;
    GetExitCodeProcess(pi.hProcess, &child_exit);

    CloseDecompressor(decompressor);
    CloseCompressor(compressor);

    const double original_mb =
        static_cast<double>(region_bytes) / (1024.0 * 1024.0);
    const double capsule_mb =
        static_cast<double>(capsule_bytes) / (1024.0 * 1024.0);
    const double ratio =
        region_bytes ? static_cast<double>(capsule_bytes) /
                           static_cast<double>(region_bytes)
                     : 1.0;
    const double private_drop_mb =
        before.private_mb - dormant.private_mb;

    const bool pass =
        snapshot_ok &&
        decommit_ok &&
        reserved_after_decommit &&
        recommit_ok &&
        restore_ok &&
        child_verify_ok &&
        child_exit == 0 &&
        capsule_bytes < region_bytes &&
        compressed_units > 0;

    std::ofstream out(json_path, std::ios::binary);
    out << "{\n";
    out << "  \"phase\": \"Windows Phase 4 external capsule\",\n";
    out << "  \"region_mb\": " << mb << ",\n";
    out << "  \"unit_kb\": 256,\n";
    out << "  \"units\": " << unit_count << ",\n";
    out << "  \"compression\": \"XPRESS block mode\",\n";
    out << "  \"compressed_units\": " << compressed_units << ",\n";
    out << "  \"raw_fallback_units\": " << raw_units << ",\n";
    out << "  \"capsule_mb\": " << std::fixed << std::setprecision(3)
        << capsule_mb << ",\n";
    out << "  \"capsule_ratio\": " << ratio << ",\n";
    out << "  \"snapshot_ms\": " << snapshot_ms << ",\n";
    out << "  \"decommit_ms\": " << decommit_ms << ",\n";
    out << "  \"restore_ms\": " << restore_ms << ",\n";
    out << "  \"child_verify_ms\": " << child_verify_ms << ",\n";
    out << "  \"private_before_mb\": " << before.private_mb << ",\n";
    out << "  \"private_dormant_mb\": " << dormant.private_mb << ",\n";
    out << "  \"private_restored_mb\": " << restored.private_mb << ",\n";
    out << "  \"private_drop_mb_informational\": "
        << private_drop_mb << ",\n";
    out << "  \"working_set_before_mb\": " << before.working_set_mb << ",\n";
    out << "  \"working_set_dormant_mb\": " << dormant.working_set_mb << ",\n";
    out << "  \"working_set_restored_mb\": " << restored.working_set_mb << ",\n";
    out << "  \"snapshot_ok\": " << (snapshot_ok ? "true" : "false") << ",\n";
    out << "  \"decommit_ok\": " << (decommit_ok ? "true" : "false") << ",\n";
    out << "  \"reserved_after_decommit\": "
        << (reserved_after_decommit ? "true" : "false") << ",\n";
    out << "  \"recommit_ok\": " << (recommit_ok ? "true" : "false") << ",\n";
    out << "  \"restore_ok\": " << (restore_ok ? "true" : "false") << ",\n";
    out << "  \"expected_hash\": " << expected_hash << ",\n";
    out << "  \"restored_hash\": " << final_hash << ",\n";
    out << "  \"child_exit_code\": " << child_exit << ",\n";
    out << "  \"pass\": " << (pass ? "true" : "false") << "\n";
    out << "}\n";
    out.close();

    std::cout << "StateRAM Windows Phase 4 - external capsule\n";
    std::cout << " app-owned region: " << original_mb << " MiB\n";
    std::cout << " capsule: " << capsule_mb << " MiB ("
              << ratio * 100.0 << "% of raw)\n";
    std::cout << " representations: compressed=" << compressed_units
              << " raw-fallback=" << raw_units << "\n";
    std::cout << " snapshot=" << snapshot_ms
              << " ms decommit=" << decommit_ms
              << " ms restore=" << restore_ms << " ms\n";
    std::cout << " private memory: before=" << before.private_mb
              << " MiB dormant=" << dormant.private_mb
              << " MiB restored=" << restored.private_mb << " MiB\n";
    std::cout << " virtual state after decommit: "
              << (reserved_after_decommit ? "RESERVED" : "unexpected") << "\n";
    std::cout << " integrity: "
              << (child_verify_ok ? "OK" : "FAIL") << "\n";
    std::cout << " WINDOWS_STATERAM_PHASE4="
              << (pass ? "PASS" : "FAIL") << "\n";

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
        std::wstring prefix;
        std::string json_path = "windows_phase4.json";

        for (int i = 1; i < argc; ++i) {
            std::string k = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error("missing value for " + k);
                }
                return argv[++i];
            };

            if (k == "--child") child = true;
            else if (k == "--parent") child = false;
            else if (k == "--mb") mb = std::stoi(next());
            else if (k == "--prefix") prefix = widen(next());
            else if (k == "--json") json_path = next();
            else throw std::runtime_error("unknown argument: " + k);
        }

        if (child) {
            if (prefix.empty()) {
                throw std::runtime_error("child missing --prefix");
            }
            return child_main(mb, prefix);
        }

        return parent_main(mb, json_path);

    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        return 2;
    }
}
