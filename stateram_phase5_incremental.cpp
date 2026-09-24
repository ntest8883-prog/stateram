#define _WIN32_WINNT 0x0602
#define PSAPI_VERSION 1
#define NOMINMAX

#include <windows.h>
#include <psapi.h>
#include <compressapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Cabinet.lib")

static constexpr size_t PAGE_BYTES = 4096;
static constexpr size_t UNIT_BYTES = 256 * 1024;
static constexpr size_t PAGES_PER_UNIT = UNIT_BYTES / PAGE_BYTES;
static constexpr size_t MAX_DIRTY_PAGES = 65536;

enum Command : LONG {
    CMD_MUTATE = 1,
    CMD_REPORT_DIRTY = 2,
    CMD_VERIFY_CORE = 3,
    CMD_CORE_SPIN = 4,
    CMD_VERIFY_FULL = 5,
    CMD_EXIT = 6
};

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
    if ((page_index & 3ull) == 0) {
        auto* words = reinterpret_cast<uint64_t*>(page);
        for (size_t i = 0; i < PAGE_BYTES / 8; ++i) {
            words[i] =
                splitmix64(page_index * 0xD6E8FEB86659FD93ull + i);
        }
        return;
    }

    uint8_t base =
        static_cast<uint8_t>((page_index * 29 + 17) & 0xffu);
    std::memset(page, base, PAGE_BYTES);
    for (size_t off = 0; off < PAGE_BYTES; off += 512) {
        uint64_t x =
            splitmix64(page_index * 0xA0761D6478BD642Full + off);
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

struct SharedHeader {
    volatile LONG command;
    volatile LONG child_ok;
    volatile LONG stop_spin;

    uint64_t base;
    uint64_t bytes;
    uint64_t core_bytes;

    uint64_t expected_full_hash;
    uint64_t expected_core_hash;
    uint64_t observed_full_hash;
    uint64_t observed_core_hash;

    uint64_t dirty_count;
    uint64_t write_watch_granularity;

    double mutate_ms;
    double write_watch_ms;
    double verify_core_ms;
    double verify_full_ms;

    uint64_t core_spin_passes;
    uint64_t core_spin_checksum;
};

static uint32_t* dirty_indices(SharedHeader* h) {
    return reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(h) + sizeof(SharedHeader));
}

static size_t shared_bytes() {
    return sizeof(SharedHeader) +
           MAX_DIRTY_PAGES * sizeof(uint32_t);
}

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
    int n = MultiByteToWideChar(
        CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, s.c_str(), -1, out.data(), n);
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
            static_cast<double>(pmc.WorkingSetSize) /
            (1024.0 * 1024.0);
        out.private_mb =
            static_cast<double>(pmc.PrivateUsage) /
            (1024.0 * 1024.0);
    }
    return out;
}

static bool signal_and_wait(
    HANDLE go,
    HANDLE done,
    SharedHeader* shared,
    LONG command,
    DWORD timeout_ms = 30000
) {
    InterlockedExchange(&shared->command, command);
    ResetEvent(done);
    if (!SetEvent(go)) return false;
    return WaitForSingleObject(done, timeout_ms) == WAIT_OBJECT_0;
}

static int child_main(
    int mb,
    int core_mb,
    int dirty_pages_target,
    const std::wstring& prefix
) {
    Names n = make_names(prefix);

    HANDLE ready =
        OpenEventW(EVENT_MODIFY_STATE, FALSE, n.ready.c_str());
    HANDLE go =
        OpenEventW(SYNCHRONIZE, FALSE, n.go.c_str());
    HANDLE done =
        OpenEventW(EVENT_MODIFY_STATE, FALSE, n.done.c_str());
    HANDLE map =
        OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, n.map.c_str());

    if (!ready || !go || !done || !map) {
        std::cerr << "child IPC open failed "
                  << GetLastError() << "\n";
        return 20;
    }

    auto* shared = static_cast<SharedHeader*>(
        MapViewOfFile(
            map, FILE_MAP_ALL_ACCESS, 0, 0, shared_bytes())
    );
    if (!shared) return 21;

    const size_t bytes =
        static_cast<size_t>(mb) * 1024ull * 1024ull;
    const size_t core_bytes =
        static_cast<size_t>(core_mb) * 1024ull * 1024ull;

    auto* arena = static_cast<uint8_t*>(
        VirtualAlloc(
            nullptr,
            bytes,
            MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
            PAGE_READWRITE)
    );

    if (!arena) {
        std::cerr << "child VirtualAlloc failed "
                  << GetLastError() << "\n";
        return 22;
    }

    const size_t page_count = bytes / PAGE_BYTES;

    for (size_t p = 0; p < page_count; ++p) {
        fill_page(arena + p * PAGE_BYTES, p);
    }

    shared->base = reinterpret_cast<uint64_t>(arena);
    shared->bytes = bytes;
    shared->core_bytes = core_bytes;
    shared->dirty_count = 0;
    shared->write_watch_granularity = 0;
    shared->core_spin_passes = 0;
    shared->core_spin_checksum = 0;
    InterlockedExchange(&shared->child_ok, 1);
    SetEvent(ready);

    std::vector<void*> watch_addresses(MAX_DIRTY_PAGES);

    for (;;) {
        if (WaitForSingleObject(go, INFINITE) != WAIT_OBJECT_0) {
            return 23;
        }

        LONG cmd =
            InterlockedCompareExchange(&shared->command, 0, 0);

        if (cmd == CMD_EXIT) break;

        if (cmd == CMD_MUTATE) {
            double t0 = now_ms();

            UINT rr = ResetWriteWatch(arena, bytes);
            if (rr != 0) {
                std::cerr << "ResetWriteWatch failed "
                          << rr << "\n";
                return 24;
            }

            const size_t target =
                std::min<size_t>(
                    static_cast<size_t>(dirty_pages_target),
                    page_count);

            for (size_t i = 0; i < target; ++i) {
                size_t page =
                    (i * 131ull + 17ull) % page_count;
                uint8_t* p = arena + page * PAGE_BYTES;
                p[13] ^= static_cast<uint8_t>(
                    0x5A ^ (i & 0xffu));
                p[2047] ^= static_cast<uint8_t>(
                    0xA5 ^ ((i * 7u) & 0xffu));
            }

            shared->expected_core_hash =
                fnv1a64(arena, core_bytes);
            shared->expected_full_hash =
                fnv1a64(arena, bytes);
            shared->mutate_ms = now_ms() - t0;

            SetEvent(done);
            continue;
        }

        if (cmd == CMD_REPORT_DIRTY) {
            double t0 = now_ms();

            ULONG_PTR count = watch_addresses.size();
            DWORD granularity = 0;

            UINT r = GetWriteWatch(
                WRITE_WATCH_FLAG_RESET,
                arena,
                bytes,
                watch_addresses.data(),
                &count,
                &granularity
            );

            if (r != 0) {
                std::cerr << "GetWriteWatch failed "
                          << r << "\n";
                return 25;
            }

            if (count > MAX_DIRTY_PAGES) {
                std::cerr << "dirty page overflow\n";
                return 26;
            }

            auto* out = dirty_indices(shared);
            for (ULONG_PTR i = 0; i < count; ++i) {
                uintptr_t addr =
                    reinterpret_cast<uintptr_t>(
                        watch_addresses[i]);
                uintptr_t base =
                    reinterpret_cast<uintptr_t>(arena);
                out[i] =
                    static_cast<uint32_t>(
                        (addr - base) / granularity);
            }

            shared->dirty_count =
                static_cast<uint64_t>(count);
            shared->write_watch_granularity = granularity;
            shared->write_watch_ms = now_ms() - t0;

            SetEvent(done);
            continue;
        }

        if (cmd == CMD_VERIFY_CORE) {
            double t0 = now_ms();
            shared->observed_core_hash =
                fnv1a64(arena, core_bytes);
            shared->verify_core_ms =
                now_ms() - t0;
            SetEvent(done);
            continue;
        }

        if (cmd == CMD_CORE_SPIN) {
            InterlockedExchange(&shared->stop_spin, 0);

            uint64_t passes = 0;
            uint64_t checksum = 0;

            while (InterlockedCompareExchange(
                       &shared->stop_spin, 0, 0) == 0) {
                uint64_t local = 0;

                for (size_t off = 0;
                     off < core_bytes;
                     off += PAGE_BYTES) {
                    local += arena[off];
                }

                checksum ^= splitmix64(local + passes);
                ++passes;
                Sleep(0);
            }

            shared->core_spin_passes = passes;
            shared->core_spin_checksum = checksum;
            SetEvent(done);
            continue;
        }

        if (cmd == CMD_VERIFY_FULL) {
            double t0 = now_ms();
            shared->observed_full_hash =
                fnv1a64(arena, bytes);
            shared->verify_full_ms =
                now_ms() - t0;
            SetEvent(done);
            continue;
        }

        std::cerr << "unknown child command\n";
        return 27;
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

using DeltaPage = std::array<uint8_t, PAGE_BYTES>;

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

    if (first) return false;

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        needed == 0) {
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

static bool snapshot_full(
    HANDLE process,
    uint64_t base,
    size_t bytes,
    COMPRESSOR_HANDLE compressor,
    std::vector<CapsuleUnit>& capsule,
    uint64_t& capsule_bytes,
    size_t& compressed_units,
    size_t& raw_units
) {
    const size_t unit_count = bytes / UNIT_BYTES;
    capsule.clear();
    capsule.resize(unit_count);

    capsule_bytes = 0;
    compressed_units = 0;
    raw_units = 0;

    std::vector<uint8_t> raw(UNIT_BYTES);

    for (size_t u = 0; u < unit_count; ++u) {
        SIZE_T got = 0;
        const void* remote =
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(
                    base + u * UNIT_BYTES));

        if (!ReadProcessMemory(
                process,
                remote,
                raw.data(),
                raw.size(),
                &got) ||
            got != raw.size()) {
            return false;
        }

        if (!compress_unit(
                compressor, raw, capsule[u])) {
            return false;
        }

        capsule_bytes += capsule[u].payload.size();
        if (capsule[u].compressed) {
            ++compressed_units;
        } else {
            ++raw_units;
        }
    }

    return true;
}

static bool capture_delta(
    HANDLE process,
    uint64_t base,
    const SharedHeader* shared,
    std::unordered_map<uint32_t, DeltaPage>& deltas
) {
    deltas.clear();

    const uint64_t count = shared->dirty_count;
    const uint64_t granularity =
        shared->write_watch_granularity;

    if (granularity != PAGE_BYTES) {
        std::cerr << "Unexpected write-watch granularity: "
                  << granularity << "\n";
        return false;
    }

    const auto* indices =
        reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(shared) +
            sizeof(SharedHeader));

    for (uint64_t i = 0; i < count; ++i) {
        uint32_t page_index = indices[i];
        DeltaPage page{};
        SIZE_T got = 0;

        const void* remote =
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(
                    base +
                    static_cast<uint64_t>(page_index) *
                    PAGE_BYTES));

        if (!ReadProcessMemory(
                process,
                remote,
                page.data(),
                page.size(),
                &got) ||
            got != page.size()) {
            return false;
        }

        deltas.emplace(page_index, std::move(page));
    }

    return true;
}

static bool build_unit(
    DECOMPRESSOR_HANDLE decompressor,
    const CapsuleUnit& capsule_unit,
    size_t unit_index,
    const std::unordered_map<uint32_t, DeltaPage>& deltas,
    std::vector<uint8_t>& raw
) {
    if (!decompress_unit(
            decompressor, capsule_unit, raw)) {
        return false;
    }

    const uint32_t first_page =
        static_cast<uint32_t>(
            unit_index * PAGES_PER_UNIT);

    for (uint32_t p = 0;
         p < static_cast<uint32_t>(PAGES_PER_UNIT);
         ++p) {
        auto it = deltas.find(first_page + p);
        if (it != deltas.end()) {
            std::memcpy(
                raw.data() + p * PAGE_BYTES,
                it->second.data(),
                PAGE_BYTES);
        }
    }

    return true;
}

static bool restore_units(
    HANDLE process,
    uint64_t base,
    size_t first_unit,
    size_t end_unit,
    const std::vector<CapsuleUnit>& capsule,
    const std::unordered_map<uint32_t, DeltaPage>& deltas,
    DECOMPRESSOR_HANDLE decompressor
) {
    std::vector<uint8_t> raw(UNIT_BYTES);

    for (size_t u = first_unit; u < end_unit; ++u) {
        if (!build_unit(
                decompressor,
                capsule[u],
                u,
                deltas,
                raw)) {
            return false;
        }

        void* remote =
            reinterpret_cast<void*>(
                static_cast<uintptr_t>(
                    base + u * UNIT_BYTES));

        void* committed = VirtualAllocEx(
            process,
            remote,
            UNIT_BYTES,
            MEM_COMMIT,
            PAGE_READWRITE
        );

        if (committed != remote) {
            return false;
        }

        SIZE_T wrote = 0;

        if (!WriteProcessMemory(
                process,
                remote,
                raw.data(),
                raw.size(),
                &wrote) ||
            wrote != raw.size()) {
            return false;
        }
    }

    return true;
}

static bool region_reserved(
    HANDLE process,
    uint64_t base
) {
    MEMORY_BASIC_INFORMATION mbi{};

    SIZE_T n = VirtualQueryEx(
        process,
        reinterpret_cast<LPCVOID>(
            static_cast<uintptr_t>(base)),
        &mbi,
        sizeof(mbi)
    );

    return n == sizeof(mbi) &&
           mbi.State == MEM_RESERVE;
}

static int parent_main(
    int mb,
    int core_mb,
    int dirty_pages_target,
    const std::string& json_path
) {
    const size_t bytes =
        static_cast<size_t>(mb) * 1024ull * 1024ull;
    const size_t core_bytes =
        static_cast<size_t>(core_mb) *
        1024ull * 1024ull;

    if (bytes == 0 ||
        bytes % UNIT_BYTES != 0 ||
        core_bytes == 0 ||
        core_bytes % UNIT_BYTES != 0 ||
        core_bytes >= bytes) {
        throw std::runtime_error(
            "invalid arena/core size");
    }

    std::wstring prefix =
        L"StateRAM5_" +
        std::to_wstring(GetCurrentProcessId()) +
        L"_" +
        std::to_wstring(GetTickCount64());

    Names n = make_names(prefix);

    HANDLE ready =
        CreateEventW(nullptr, FALSE, FALSE, n.ready.c_str());
    HANDLE go =
        CreateEventW(nullptr, FALSE, FALSE, n.go.c_str());
    HANDLE done =
        CreateEventW(nullptr, FALSE, FALSE, n.done.c_str());

    const uint64_t sb = shared_bytes();

    HANDLE map = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        static_cast<DWORD>(sb >> 32),
        static_cast<DWORD>(sb & 0xffffffffu),
        n.map.c_str()
    );

    if (!ready || !go || !done || !map) {
        throw std::runtime_error(
            "parent IPC creation failed");
    }

    auto* shared = static_cast<SharedHeader*>(
        MapViewOfFile(
            map,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            shared_bytes())
    );

    if (!shared) {
        throw std::runtime_error(
            "parent map view failed");
    }

    ZeroMemory(shared, shared_bytes());

    wchar_t exe[MAX_PATH]{};

    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        throw std::runtime_error(
            "GetModuleFileNameW failed");
    }

    std::wstringstream cmd;

    cmd << L"\"" << exe << L"\""
        << L" --child"
        << L" --mb " << mb
        << L" --core-mb " << core_mb
        << L" --dirty-pages " << dirty_pages_target
        << L" --prefix \"" << prefix << L"\"";

    std::wstring cmdline = cmd.str();

    std::vector<wchar_t> mutable_cmd(
        cmdline.begin(), cmdline.end());
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
            "CreateProcessW failed " +
            std::to_string(GetLastError()));
    }

    CloseHandle(pi.hThread);

    if (WaitForSingleObject(
            ready, 30000) != WAIT_OBJECT_0 ||
        InterlockedCompareExchange(
            &shared->child_ok, 0, 0) != 1) {
        TerminateProcess(pi.hProcess, 90);
        throw std::runtime_error(
            "child did not become ready");
    }

    const uint64_t base = shared->base;

    COMPRESSOR_HANDLE compressor = nullptr;
    DECOMPRESSOR_HANDLE decompressor = nullptr;

    const DWORD alg =
        COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW;

    if (!CreateCompressor(
            alg, nullptr, &compressor)) {
        TerminateProcess(pi.hProcess, 91);
        throw std::runtime_error(
            "CreateCompressor failed " +
            std::to_string(GetLastError()));
    }

    if (!CreateDecompressor(
            alg, nullptr, &decompressor)) {
        CloseCompressor(compressor);
        TerminateProcess(pi.hProcess, 92);
        throw std::runtime_error(
            "CreateDecompressor failed " +
            std::to_string(GetLastError()));
    }

    std::vector<CapsuleUnit> capsule;
    uint64_t capsule_bytes = 0;
    size_t compressed_units = 0;
    size_t raw_units = 0;

    double snapshot_t0 = now_ms();

    bool snapshot_ok = snapshot_full(
        pi.hProcess,
        base,
        bytes,
        compressor,
        capsule,
        capsule_bytes,
        compressed_units,
        raw_units
    );

    double full_snapshot_ms =
        now_ms() - snapshot_t0;

    if (!snapshot_ok) {
        TerminateProcess(pi.hProcess, 93);
        throw std::runtime_error(
            "full capsule snapshot failed");
    }

    ProcMem parent_after_snapshot =
        proc_mem(GetCurrentProcess());
    ProcMem child_before =
        proc_mem(pi.hProcess);

    if (!signal_and_wait(
            go, done, shared, CMD_MUTATE, 30000)) {
        TerminateProcess(pi.hProcess, 94);
        throw std::runtime_error(
            "child mutation failed");
    }

    double finalize_t0 = now_ms();

    if (!signal_and_wait(
            go, done, shared,
            CMD_REPORT_DIRTY, 30000)) {
        TerminateProcess(pi.hProcess, 95);
        throw std::runtime_error(
            "write-watch report failed");
    }

    std::unordered_map<uint32_t, DeltaPage>
        deltas;

    double delta_t0 = now_ms();

    bool delta_ok = capture_delta(
        pi.hProcess,
        base,
        shared,
        deltas
    );

    double delta_capture_ms =
        now_ms() - delta_t0;
    double finalization_ms =
        now_ms() - finalize_t0;

    if (!delta_ok) {
        TerminateProcess(pi.hProcess, 96);
        throw std::runtime_error(
            "delta capture failed");
    }

    const uint64_t delta_bytes =
        static_cast<uint64_t>(deltas.size()) *
        PAGE_BYTES;

    double decommit_t0 = now_ms();

    bool decommit_ok = VirtualFreeEx(
        pi.hProcess,
        reinterpret_cast<void*>(
            static_cast<uintptr_t>(base)),
        bytes,
        MEM_DECOMMIT
    ) != FALSE;

    double decommit_ms =
        now_ms() - decommit_t0;

    bool reserved_after_decommit =
        decommit_ok &&
        region_reserved(pi.hProcess, base);

    Sleep(50);

    ProcMem child_dormant =
        proc_mem(pi.hProcess);
    ProcMem parent_dormant =
        proc_mem(GetCurrentProcess());

    const size_t total_units =
        bytes / UNIT_BYTES;
    const size_t core_units =
        core_bytes / UNIT_BYTES;

    double core_restore_t0 = now_ms();

    bool core_restore_ok =
        reserved_after_decommit &&
        restore_units(
            pi.hProcess,
            base,
            0,
            core_units,
            capsule,
            deltas,
            decompressor
        );

    double core_restore_ms =
        now_ms() - core_restore_t0;

    bool core_verify_signal_ok = false;
    bool core_hash_ok = false;

    if (core_restore_ok) {
        core_verify_signal_ok =
            signal_and_wait(
                go, done, shared,
                CMD_VERIFY_CORE, 30000);

        core_hash_ok =
            core_verify_signal_ok &&
            shared->observed_core_hash ==
                shared->expected_core_hash &&
            shared->observed_core_hash != 0;
    }

    bool spin_started = false;
    bool deep_restore_ok = false;
    double deep_restore_ms = 0.0;

    if (core_hash_ok) {
        InterlockedExchange(
            &shared->stop_spin, 0);
        InterlockedExchange(
            &shared->command, CMD_CORE_SPIN);
        ResetEvent(done);

        spin_started = SetEvent(go) != FALSE;

        if (spin_started) {
            Sleep(5);

            double deep_t0 = now_ms();

            deep_restore_ok =
                restore_units(
                    pi.hProcess,
                    base,
                    core_units,
                    total_units,
                    capsule,
                    deltas,
                    decompressor
                );

            deep_restore_ms =
                now_ms() - deep_t0;

            InterlockedExchange(
                &shared->stop_spin, 1);

            if (WaitForSingleObject(
                    done, 30000) != WAIT_OBJECT_0) {
                deep_restore_ok = false;
            }
        }
    }

    bool full_verify_signal_ok = false;
    bool full_hash_ok = false;

    if (deep_restore_ok) {
        full_verify_signal_ok =
            signal_and_wait(
                go, done, shared,
                CMD_VERIFY_FULL, 30000);

        full_hash_ok =
            full_verify_signal_ok &&
            shared->observed_full_hash ==
                shared->expected_full_hash &&
            shared->observed_full_hash != 0;
    }

    ProcMem child_restored =
        proc_mem(pi.hProcess);

    InterlockedExchange(
        &shared->command, CMD_EXIT);
    SetEvent(go);
    WaitForSingleObject(
        pi.hProcess, 10000);

    DWORD child_exit = 999;
    GetExitCodeProcess(
        pi.hProcess, &child_exit);

    CloseDecompressor(decompressor);
    CloseCompressor(compressor);

    const double capsule_mb =
        static_cast<double>(capsule_bytes) /
        (1024.0 * 1024.0);

    const double delta_mb =
        static_cast<double>(delta_bytes) /
        (1024.0 * 1024.0);

    const double preservation_payload_mb =
        capsule_mb + delta_mb;

    const double total_dormant_private_mb =
        child_dormant.private_mb +
        parent_dormant.private_mb;

    const double raw_mb =
        static_cast<double>(bytes) /
        (1024.0 * 1024.0);

    const double payload_ratio =
        preservation_payload_mb / raw_mb;

    const double finalize_vs_full_pct =
        full_snapshot_ms > 0.0
            ? 100.0 *
              finalization_ms /
              full_snapshot_ms
            : 0.0;

    const double core_fraction_pct =
        100.0 *
        static_cast<double>(core_bytes) /
        static_cast<double>(bytes);

    const bool pass =
        snapshot_ok &&
        delta_ok &&
        shared->dirty_count > 0 &&
        shared->dirty_count <=
            static_cast<uint64_t>(
                dirty_pages_target + 16) &&
        decommit_ok &&
        reserved_after_decommit &&
        core_restore_ok &&
        core_hash_ok &&
        spin_started &&
        deep_restore_ok &&
        shared->core_spin_passes > 0 &&
        full_hash_ok &&
        child_exit == 0 &&
        payload_ratio < 0.50 &&
        core_restore_ms < deep_restore_ms &&
        finalization_ms < full_snapshot_ms;

    std::ofstream out(
        json_path, std::ios::binary);

    out << "{\n";
    out << "  \"phase\": "
        << "\"Windows Phase 5 incremental capsule + resume core\",\n";
    out << "  \"arena_mb\": " << mb << ",\n";
    out << "  \"resume_core_mb\": "
        << core_mb << ",\n";
    out << "  \"resume_core_fraction_pct\": "
        << std::fixed << std::setprecision(3)
        << core_fraction_pct << ",\n";
    out << "  \"unit_kb\": 256,\n";
    out << "  \"dirty_pages_requested\": "
        << dirty_pages_target << ",\n";
    out << "  \"dirty_pages_reported\": "
        << shared->dirty_count << ",\n";
    out << "  \"write_watch_granularity\": "
        << shared->write_watch_granularity
        << ",\n";
    out << "  \"baseline_capsule_mb\": "
        << capsule_mb << ",\n";
    out << "  \"delta_mb\": "
        << delta_mb << ",\n";
    out << "  \"preservation_payload_mb\": "
        << preservation_payload_mb << ",\n";
    out << "  \"payload_ratio\": "
        << payload_ratio << ",\n";
    out << "  \"full_snapshot_ms\": "
        << full_snapshot_ms << ",\n";
    out << "  \"child_mutate_and_test_hash_ms\": "
        << shared->mutate_ms << ",\n";
    out << "  \"write_watch_report_ms\": "
        << shared->write_watch_ms << ",\n";
    out << "  \"delta_capture_ms\": "
        << delta_capture_ms << ",\n";
    out << "  \"quiescent_finalization_ms\": "
        << finalization_ms << ",\n";
    out << "  \"finalization_vs_full_snapshot_pct\": "
        << finalize_vs_full_pct << ",\n";
    out << "  \"decommit_ms\": "
        << decommit_ms << ",\n";
    out << "  \"resume_core_restore_ms\": "
        << core_restore_ms << ",\n";
    out << "  \"resume_core_verify_ms\": "
        << shared->verify_core_ms << ",\n";
    out << "  \"deep_restore_ms\": "
        << deep_restore_ms << ",\n";
    out << "  \"full_verify_ms\": "
        << shared->verify_full_ms << ",\n";
    out << "  \"core_spin_passes_during_deep_restore\": "
        << shared->core_spin_passes << ",\n";
    out << "  \"child_private_before_mb\": "
        << child_before.private_mb << ",\n";
    out << "  \"child_private_dormant_mb\": "
        << child_dormant.private_mb << ",\n";
    out << "  \"child_private_restored_mb\": "
        << child_restored.private_mb << ",\n";
    out << "  \"parent_private_after_snapshot_mb\": "
        << parent_after_snapshot.private_mb
        << ",\n";
    out << "  \"parent_private_dormant_mb\": "
        << parent_dormant.private_mb << ",\n";
    out << "  \"total_dormant_private_mb\": "
        << total_dormant_private_mb << ",\n";
    out << "  \"reserved_after_decommit\": "
        << (reserved_after_decommit
            ? "true" : "false") << ",\n";
    out << "  \"core_hash_ok\": "
        << (core_hash_ok
            ? "true" : "false") << ",\n";
    out << "  \"full_hash_ok\": "
        << (full_hash_ok
            ? "true" : "false") << ",\n";
    out << "  \"child_exit_code\": "
        << child_exit << ",\n";
    out << "  \"pass\": "
        << (pass ? "true" : "false")
        << "\n";
    out << "}\n";

    out.close();

    std::cout
        << "StateRAM Windows Phase 5 - incremental capsule + resume core\n";
    std::cout
        << " baseline capsule: "
        << capsule_mb << " MiB\n";
    std::cout
        << " dirty delta: "
        << delta_mb << " MiB from "
        << shared->dirty_count
        << " pages\n";
    std::cout
        << " preservation payload: "
        << preservation_payload_mb
        << " MiB ("
        << payload_ratio * 100.0
        << "% of raw state)\n";
    std::cout
        << " full snapshot: "
        << full_snapshot_ms
        << " ms\n";
    std::cout
        << " final quiescent delta capture: "
        << finalization_ms
        << " ms ("
        << finalize_vs_full_pct
        << "% of full snapshot time)\n";
    std::cout
        << " decommit: "
        << decommit_ms
        << " ms\n";
    std::cout
        << " resume core "
        << core_mb << " MiB restore: "
        << core_restore_ms
        << " ms\n";
    std::cout
        << " deep restore: "
        << deep_restore_ms
        << " ms while app completed "
        << shared->core_spin_passes
        << " core-only passes\n";
    std::cout
        << " core integrity: "
        << (core_hash_ok ? "OK" : "FAIL")
        << " full integrity: "
        << (full_hash_ok ? "OK" : "FAIL")
        << "\n";
    std::cout
        << " WINDOWS_STATERAM_PHASE5="
        << (pass ? "PASS" : "FAIL")
        << "\n";

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
        int core_mb = 8;
        int dirty_pages_target = 512;
        std::wstring prefix;
        std::string json_path =
            "windows_phase5.json";

        for (int i = 1; i < argc; ++i) {
            std::string k = argv[i];

            auto next = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error(
                        "missing value for " + k);
                }
                return argv[++i];
            };

            if (k == "--child") {
                child = true;
            } else if (k == "--parent") {
                child = false;
            } else if (k == "--mb") {
                mb = std::stoi(next());
            } else if (k == "--core-mb") {
                core_mb = std::stoi(next());
            } else if (k == "--dirty-pages") {
                dirty_pages_target =
                    std::stoi(next());
            } else if (k == "--prefix") {
                prefix = widen(next());
            } else if (k == "--json") {
                json_path = next();
            } else {
                throw std::runtime_error(
                    "unknown argument: " + k);
            }
        }

        if (child) {
            if (prefix.empty()) {
                throw std::runtime_error(
                    "child missing --prefix");
            }

            return child_main(
                mb,
                core_mb,
                dirty_pages_target,
                prefix);
        }

        return parent_main(
            mb,
            core_mb,
            dirty_pages_target,
            json_path);

    } catch (const std::exception& e) {
        std::cerr
            << "FATAL: "
            << e.what()
            << "\n";
        return 2;
    }
}
