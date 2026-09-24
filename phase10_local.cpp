#define _WIN32_WINNT 0x0602
#define PSAPI_VERSION 1
#define NOMINMAX

#include <windows.h>
#include <psapi.h>

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

#include "stateram_sdk.h"

#pragma comment(lib, "Psapi.lib")

static constexpr size_t PAGE_BYTES = 4096;
static constexpr uint64_t MB = 1024ull * 1024ull;

static constexpr size_t TEST_ARENA_BYTES = 128ull * MB;
static constexpr size_t TEST_CORE_BYTES = 4ull * MB;
static constexpr size_t TEST_DIRTY_PAGES = 128;

static constexpr uint64_t CHILD_HARD_COMMIT_BYTES = 320ull * MB;
static constexpr DWORD CHILD_CPU_RATE = 3500; // 35% of total CPU cycles.

static constexpr uint64_t START_MIN_AVAILABLE = 1024ull * MB;
static constexpr uint64_t ABORT_MIN_AVAILABLE = 640ull * MB;
static constexpr DWORD START_MAX_MEMORY_LOAD = 80;
static constexpr DWORD ABORT_MEMORY_LOAD = 88;
static constexpr DWORD PARENT_TIMEOUT_MS = 90000;

static double now_ms() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();

    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);

    return 1000.0 *
           static_cast<double>(q.QuadPart) /
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

struct ProcMem {
    double private_mb = 0.0;
    double ws_mb = 0.0;
};

static ProcMem proc_mem(HANDLE process = GetCurrentProcess()) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    ProcMem out{};

    if (GetProcessMemoryInfo(
            process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc))) {
        out.private_mb =
            static_cast<double>(pmc.PrivateUsage) /
            (1024.0 * 1024.0);

        out.ws_mb =
            static_cast<double>(pmc.WorkingSetSize) /
            (1024.0 * 1024.0);
    }

    return out;
}

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;

    std::sort(v.begin(), v.end());

    size_t idx =
        static_cast<size_t>(
            p * static_cast<double>(v.size() - 1));

    return v[idx];
}

template <typename T>
static T load_fn(HMODULE dll, const char* name) {
    FARPROC p = GetProcAddress(dll, name);

    if (!p) {
        throw std::runtime_error(
            std::string("missing SDK export: ") + name);
    }

    return reinterpret_cast<T>(p);
}

static bool current_process_is_elevated() {
    HANDLE token = nullptr;

    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &token)) {
        return true; // Fail closed.
    }

    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;

    BOOL ok =
        GetTokenInformation(
            token,
            TokenElevation,
            &elevation,
            sizeof(elevation),
            &bytes);

    CloseHandle(token);

    if (!ok) {
        return true; // Fail closed.
    }

    return elevation.TokenIsElevated != 0;
}

static bool read_memory_status(MEMORYSTATUSEX& ms) {
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) != FALSE;
}

static std::wstring exe_path() {
    std::vector<wchar_t> buf(32768);

    DWORD n =
        GetModuleFileNameW(
            nullptr,
            buf.data(),
            static_cast<DWORD>(buf.size()));

    if (n == 0 || n >= buf.size()) {
        throw std::runtime_error("cannot locate executable path");
    }

    return std::wstring(buf.data(), n);
}

static void write_guard_json(
    const char* status,
    const char* reason,
    double start_available_mb,
    double min_available_mb,
    DWORD peak_process_mb,
    DWORD child_exit
) {
    std::ofstream out("stateram_guard_result.json", std::ios::binary);

    out << "{\n";
    out << "  \"status\": \"" << status << "\",\n";
    out << "  \"reason\": \"" << reason << "\",\n";
    out << "  \"start_available_mb\": "
        << std::fixed << std::setprecision(1)
        << start_available_mb << ",\n";
    out << "  \"minimum_available_mb\": "
        << min_available_mb << ",\n";
    out << "  \"job_peak_process_memory_mb\": "
        << peak_process_mb << ",\n";
    out << "  \"child_exit_code\": "
        << child_exit << "\n";
    out << "}\n";
}

static int worker_main() {
    std::cout
        << "\nStateRAM isolated worker started.\n"
        << "Managed test state: 128 MiB\n"
        << "No other process memory is touched.\n\n";

    HMODULE dll =
        LoadLibraryW(L"stateram_sdk.dll");

    if (!dll) {
        std::cerr
            << "Could not load stateram_sdk.dll (error "
            << GetLastError() << ").\n";
        return 20;
    }

    using FApi = uint32_t (*)();
    using FCreate = SRHandle (*)(uint64_t, uint64_t);
    using FData = void* (*)(SRHandle);
    using FInt = int (*)(SRHandle);
    using FBegin = int (*)(SRHandle, uint32_t, uint32_t);
    using FWait = int (*)(SRHandle, uint32_t);
    using FMetrics = int (*)(SRHandle, SRMetrics*);
    using FDestroy = void (*)(SRHandle);

    try {
        auto api =
            load_fn<FApi>(dll, "sr_api_version");

        auto create =
            load_fn<FCreate>(dll, "sr_create");

        auto datafn =
            load_fn<FData>(dll, "sr_data");

        auto begin_baseline =
            load_fn<FBegin>(
                dll,
                "sr_begin_background_baseline");

        auto baseline_done =
            load_fn<FInt>(
                dll,
                "sr_baseline_done");

        auto wait_baseline =
            load_fn<FWait>(
                dll,
                "sr_wait_baseline");

        auto dormant =
            load_fn<FInt>(
                dll,
                "sr_enter_dormant");

        auto resume_core =
            load_fn<FInt>(
                dll,
                "sr_resume_core");

        auto start_deep =
            load_fn<FInt>(
                dll,
                "sr_start_deep_restore");

        auto deep_done =
            load_fn<FInt>(
                dll,
                "sr_deep_done");

        auto wait_deep =
            load_fn<FWait>(
                dll,
                "sr_wait_deep");

        auto release_capsule =
            load_fn<FInt>(
                dll,
                "sr_release_capsule");

        auto metricsfn =
            load_fn<FMetrics>(
                dll,
                "sr_get_metrics");

        auto destroy =
            load_fn<FDestroy>(
                dll,
                "sr_destroy");

        uint32_t version = api();

        if ((version >> 16) != 9) {
            std::cerr
                << "Expected Phase 9 SDK ABI, got 0x"
                << std::hex << version << std::dec << ".\n";
            FreeLibrary(dll);
            return 21;
        }

        SRHandle h =
            create(
                TEST_ARENA_BYTES,
                TEST_CORE_BYTES);

        if (!h) {
            std::cerr
                << "StateRAM refused or could not allocate the isolated test arena.\n";
            FreeLibrary(dll);
            return 22;
        }

        auto* arena =
            static_cast<uint8_t*>(
                datafn(h));

        if (!arena) {
            destroy(h);
            FreeLibrary(dll);
            return 23;
        }

        const size_t pages =
            TEST_ARENA_BYTES / PAGE_BYTES;

        for (size_t p = 0; p < pages; ++p) {
            fill_page(
                arena + p * PAGE_BYTES,
                p);
        }

        ProcMem active_initial = proc_mem();

        // Very conservative local pacing:
        // one 256 KiB unit per slice and an 8 ms base yield.
        if (!begin_baseline(h, 1, 8)) {
            std::cerr
                << "Background baseline could not start safely.\n";
            destroy(h);
            FreeLibrary(dll);
            return 24;
        }

        std::vector<double> foreground_ms;
        foreground_ms.reserve(12000);

        uint64_t foreground_passes = 0;
        uint64_t concurrent_writes = 0;
        uint64_t foreground_checksum = 0;

        while (baseline_done(h) == 0) {
            double t0 = now_ms();

            uint64_t local = 0;

            for (size_t off = 0;
                 off < TEST_CORE_BYTES;
                 off += PAGE_BYTES * 4) {
                local += arena[off];
            }

            foreground_checksum ^=
                splitmix64(local + foreground_passes);

            foreground_ms.push_back(
                now_ms() - t0);

            ++foreground_passes;

            if ((foreground_passes % 40ull) == 0) {
                size_t page =
                    (foreground_passes * 997ull + 211ull) %
                    pages;

                arena[page * PAGE_BYTES + 71] ^=
                    static_cast<uint8_t>(
                        foreground_passes & 0xffu);

                ++concurrent_writes;
            }

            Sleep(2);
        }

        if (!wait_baseline(h, 60000)) {
            std::cerr
                << "Background baseline did not complete within the local test window.\n";
            destroy(h);
            FreeLibrary(dll);
            return 25;
        }

        for (size_t i = 0;
             i < TEST_DIRTY_PAGES;
             ++i) {
            size_t page =
                (i * 131ull + 17ull) %
                pages;

            uint8_t* p =
                arena + page * PAGE_BYTES;

            p[13] ^=
                static_cast<uint8_t>(
                    0x5A ^ (i & 0xffu));

            p[2047] ^=
                static_cast<uint8_t>(
                    0xA5 ^ ((i * 7u) & 0xffu));
        }

        const uint64_t expected_core =
            fnv1a64(
                arena,
                TEST_CORE_BYTES);

        const uint64_t expected_full =
            fnv1a64(
                arena,
                TEST_ARENA_BYTES);

        if (!dormant(h)) {
            std::cerr
                << "StateRAM refused dormancy; ending isolated test.\n";
            destroy(h);
            FreeLibrary(dll);
            return 26;
        }

        Sleep(150);

        ProcMem dormant_mem = proc_mem();

        if (!resume_core(h)) {
            std::cerr
                << "Resume-core restoration failed.\n";
            destroy(h);
            FreeLibrary(dll);
            return 27;
        }

        bool core_ok =
            fnv1a64(
                arena,
                TEST_CORE_BYTES) ==
            expected_core;

        if (!start_deep(h)) {
            std::cerr
                << "Deep restoration could not start.\n";
            destroy(h);
            FreeLibrary(dll);
            return 28;
        }

        uint64_t core_passes = 0;
        uint64_t core_checksum = 0;

        while (deep_done(h) == 0) {
            uint64_t local = 0;

            for (size_t off = 0;
                 off < TEST_CORE_BYTES;
                 off += PAGE_BYTES) {
                local += arena[off];
            }

            core_checksum ^=
                splitmix64(local + core_passes);

            ++core_passes;
            Sleep(0);
        }

        bool deep_ok =
            wait_deep(h, 30000) != 0;

        bool full_ok =
            deep_ok &&
            fnv1a64(
                arena,
                TEST_ARENA_BYTES) ==
            expected_full;

        ProcMem restored_with_capsule = proc_mem();

        if (!release_capsule(h)) {
            std::cerr
                << "Capsule release failed.\n";
            destroy(h);
            FreeLibrary(dll);
            return 29;
        }

        Sleep(100);

        ProcMem active_after_release = proc_mem();

        SRMetrics m{};

        if (!metricsfn(h, &m)) {
            destroy(h);
            FreeLibrary(dll);
            return 30;
        }

        double p95 =
            percentile(
                foreground_ms,
                0.95);

        double p99 =
            percentile(
                foreground_ms,
                0.99);

        double max_ms =
            foreground_ms.empty()
                ? 0.0
                : *std::max_element(
                    foreground_ms.begin(),
                    foreground_ms.end());

        double reclaimed =
            restored_with_capsule.private_mb -
            active_after_release.private_mb;

        const double raw_mb =
            static_cast<double>(TEST_ARENA_BYTES) /
            (1024.0 * 1024.0);

        bool correctness_ok =
            core_ok &&
            full_ok &&
            core_passes > 0;

        bool memory_ok =
            dormant_mem.private_mb <
                raw_mb * 0.65 &&
            reclaimed > 30.0 &&
            active_after_release.private_mb <=
                active_initial.private_mb + 12.0;

        bool responsiveness_ok =
            foreground_ms.size() >= 50 &&
            p95 < 10.0 &&
            p99 < 25.0 &&
            max_ms < 100.0;

        bool baseline_ok =
            m.baseline_epochs == 1 &&
            m.baseline_pressure_aborts == 0 &&
            m.background_mode_entered == 1 &&
            m.baseline_wall_ms < 60000.0;

        bool clean_end_ok =
            m.lifecycle_state == SR_ACTIVE_NO_CAPSULE &&
            m.packed_used_bytes == 0 &&
            m.packed_committed_bytes == 0;

        bool pass =
            correctness_ok &&
            memory_ok &&
            responsiveness_ok &&
            baseline_ok &&
            clean_end_ok;

        std::ofstream out(
            "stateram_local_result.json",
            std::ios::binary);

        out << "{\n";
        out << "  \"phase\": \"Windows Phase 10 isolated local safe test\",\n";
        out << "  \"sdk_api_version\": " << version << ",\n";
        out << "  \"managed_state_mb\": 128,\n";
        out << "  \"resume_core_mb\": 4,\n";
        out << "  \"active_initial_private_mb\": "
            << std::fixed << std::setprecision(3)
            << active_initial.private_mb << ",\n";
        out << "  \"baseline_wall_ms\": "
            << m.baseline_wall_ms << ",\n";
        out << "  \"baseline_work_ms\": "
            << m.baseline_work_ms << ",\n";
        out << "  \"baseline_slices\": "
            << m.baseline_slices << ",\n";
        out << "  \"baseline_cpu_backoffs\": "
            << m.baseline_cpu_backoffs << ",\n";
        out << "  \"foreground_probe_count\": "
            << foreground_ms.size() << ",\n";
        out << "  \"foreground_p95_ms\": "
            << p95 << ",\n";
        out << "  \"foreground_p99_ms\": "
            << p99 << ",\n";
        out << "  \"foreground_max_ms\": "
            << max_ms << ",\n";
        out << "  \"concurrent_writes_during_baseline\": "
            << concurrent_writes << ",\n";
        out << "  \"dirty_pages_finalized\": "
            << m.dirty_pages << ",\n";
        out << "  \"dormant_private_mb\": "
            << dormant_mem.private_mb << ",\n";
        out << "  \"finalization_ms\": "
            << m.finalization_ms << ",\n";
        out << "  \"core_restore_ms\": "
            << m.core_restore_ms << ",\n";
        out << "  \"deep_restore_ms\": "
            << m.deep_restore_ms << ",\n";
        out << "  \"core_passes_during_deep_restore\": "
            << core_passes << ",\n";
        out << "  \"restored_with_capsule_private_mb\": "
            << restored_with_capsule.private_mb << ",\n";
        out << "  \"active_after_release_private_mb\": "
            << active_after_release.private_mb << ",\n";
        out << "  \"capsule_reclaimed_mb\": "
            << reclaimed << ",\n";
        out << "  \"core_hash_ok\": "
            << (core_ok ? "true" : "false") << ",\n";
        out << "  \"full_hash_ok\": "
            << (full_ok ? "true" : "false") << ",\n";
        out << "  \"correctness_ok\": "
            << (correctness_ok ? "true" : "false") << ",\n";
        out << "  \"memory_ok\": "
            << (memory_ok ? "true" : "false") << ",\n";
        out << "  \"responsiveness_ok\": "
            << (responsiveness_ok ? "true" : "false") << ",\n";
        out << "  \"pass\": "
            << (pass ? "true" : "false") << "\n";
        out << "}\n";
        out.close();

        std::cout
            << "\n--- Local StateRAM result ---\n"
            << "Background baseline: "
            << m.baseline_wall_ms
            << " ms wall, "
            << m.baseline_work_ms
            << " ms compression work\n"
            << "CPU backoffs: "
            << m.baseline_cpu_backoffs
            << "\n"
            << "Foreground latency: p95="
            << p95
            << " ms p99="
            << p99
            << " ms max="
            << max_ms
            << " ms\n"
            << "Dormant private memory: "
            << dormant_mem.private_mb
            << " MiB\n"
            << "Finalization pause: "
            << m.finalization_ms
            << " ms\n"
            << "Resume core: "
            << m.core_restore_ms
            << " ms\n"
            << "Deep restore: "
            << m.deep_restore_ms
            << " ms\n"
            << "Capsule reclaimed: "
            << reclaimed
            << " MiB\n"
            << "Active after release: "
            << active_after_release.private_mb
            << " MiB\n"
            << "Core integrity: "
            << (core_ok ? "OK" : "FAIL")
            << " | Full integrity: "
            << (full_ok ? "OK" : "FAIL")
            << "\n"
            << "STATERAM_LOCAL_SAFE_TEST="
            << (pass ? "PASS" : "FAIL")
            << "\n";

        destroy(h);
        FreeLibrary(dll);

        return pass ? 0 : 40;

    } catch (const std::exception& e) {
        std::cerr
            << "Worker error: "
            << e.what()
            << "\n";

        FreeLibrary(dll);
        return 41;
    }
}

static int launcher_main() {
    std::cout
        << "====================================================\n"
        << " StateRAM Phase 10 - isolated local safety harness\n"
        << "====================================================\n\n"
        << "This test touches ONLY its own child process.\n"
        << "It does not change the registry, page file, boot settings,\n"
        << "drivers, or memory belonging to other applications.\n\n";

    if (current_process_is_elevated()) {
        std::cout
            << "SAFETY STOP: This program is running elevated.\n"
            << "Close it and run it normally, NOT as Administrator.\n";
        return 2;
    }

    MEMORYSTATUSEX start{};

    if (!read_memory_status(start)) {
        std::cout
            << "SAFETY STOP: Windows memory status could not be read.\n";
        return 3;
    }

    double start_available_mb =
        static_cast<double>(start.ullAvailPhys) /
        (1024.0 * 1024.0);

    std::cout
        << "Windows memory load now: "
        << start.dwMemoryLoad
        << "%\n"
        << "Available physical memory: "
        << std::fixed << std::setprecision(1)
        << start_available_mb
        << " MiB\n";

    if (start.dwMemoryLoad > START_MAX_MEMORY_LOAD ||
        start.ullAvailPhys < START_MIN_AVAILABLE) {
        std::cout
            << "\nSAFETY STOP: The PC does not currently have enough headroom.\n"
            << "Nothing was started. Close some programs and try another time.\n";

        write_guard_json(
            "ABORTED_BEFORE_START",
            "insufficient_memory_headroom",
            start_available_mb,
            start_available_mb,
            0,
            0);

        return 4;
    }

    HANDLE job =
        CreateJobObjectW(
            nullptr,
            nullptr);

    if (!job) {
        std::cout
            << "SAFETY STOP: Could not create the containment job.\n";
        return 5;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};

    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit =
        static_cast<SIZE_T>(CHILD_HARD_COMMIT_BYTES);

    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))) {
        std::cout
            << "SAFETY STOP: Could not install the hard memory limit.\n";

        CloseHandle(job);
        return 6;
    }

    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};

    cpu.ControlFlags =
        JOB_OBJECT_CPU_RATE_CONTROL_ENABLE |
        JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;

    cpu.CpuRate = CHILD_CPU_RATE;

    if (!SetInformationJobObject(
            job,
            JobObjectCpuRateControlInformation,
            &cpu,
            sizeof(cpu))) {
        std::cout
            << "SAFETY STOP: Could not install the hard CPU limit.\n";

        CloseHandle(job);
        return 7;
    }

    std::wstring exe = exe_path();

    std::wstring cmd =
        L"\"" + exe + L"\" --worker";

    std::vector<wchar_t> mutable_cmd(
        cmd.begin(),
        cmd.end());

    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi{};

    DWORD flags =
        CREATE_SUSPENDED |
        BELOW_NORMAL_PRIORITY_CLASS;

    if (!CreateProcessW(
            nullptr,
            mutable_cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            flags,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        std::cout
            << "SAFETY STOP: Could not create the isolated worker.\n";

        CloseHandle(job);
        return 8;
    }

    if (!AssignProcessToJobObject(
            job,
            pi.hProcess)) {
        TerminateProcess(pi.hProcess, 70);

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);

        std::cout
            << "SAFETY STOP: Worker containment could not be established.\n";

        return 9;
    }

    HANDLE low_mem =
        CreateMemoryResourceNotification(
            LowMemoryResourceNotification);

    if (!low_mem) {
        TerminateJobObject(job, 71);

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);

        std::cout
            << "SAFETY STOP: Low-memory watchdog could not be created.\n";

        return 10;
    }

    std::cout
        << "\nContainment installed BEFORE the worker starts:\n"
        << "  hard child commit cap: 320 MiB\n"
        << "  hard CPU cap: 35% of total CPU\n"
        << "  process priority: BELOW_NORMAL\n"
        << "  watchdog aborts at >=88% memory load or <640 MiB available\n"
        << "  maximum test time: 90 seconds\n\n"
        << "Starting isolated StateRAM test...\n\n";

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    double min_available_mb =
        start_available_mb;

    bool safety_abort = false;
    const char* abort_reason = "";
    DWORD start_tick = GetTickCount();

    for (;;) {
        DWORD w =
            WaitForSingleObject(
                pi.hProcess,
                250);

        if (w == WAIT_OBJECT_0) {
            break;
        }

        if (w != WAIT_TIMEOUT) {
            safety_abort = true;
            abort_reason = "worker_wait_failure";
            break;
        }

        MEMORYSTATUSEX ms{};

        if (!read_memory_status(ms)) {
            safety_abort = true;
            abort_reason = "memory_status_failure";
            break;
        }

        double available_mb =
            static_cast<double>(ms.ullAvailPhys) /
            (1024.0 * 1024.0);

        min_available_mb =
            std::min(
                min_available_mb,
                available_mb);

        BOOL low = FALSE;

        if (!QueryMemoryResourceNotification(
                low_mem,
                &low)) {
            safety_abort = true;
            abort_reason = "low_memory_watchdog_failure";
            break;
        }

        if (low) {
            safety_abort = true;
            abort_reason = "windows_low_memory_signal";
            break;
        }

        if (ms.dwMemoryLoad >= ABORT_MEMORY_LOAD) {
            safety_abort = true;
            abort_reason = "memory_load_limit";
            break;
        }

        if (ms.ullAvailPhys < ABORT_MIN_AVAILABLE) {
            safety_abort = true;
            abort_reason = "available_memory_limit";
            break;
        }

        if (GetTickCount() - start_tick >= PARENT_TIMEOUT_MS) {
            safety_abort = true;
            abort_reason = "timeout";
            break;
        }
    }

    if (safety_abort) {
        std::cout
            << "\nSAFETY WATCHDOG TRIGGERED: "
            << abort_reason
            << "\nStopping only the isolated StateRAM worker...\n";

        TerminateJobObject(job, 72);

        WaitForSingleObject(
            pi.hProcess,
            5000);
    }

    DWORD child_exit = 999;

    GetExitCodeProcess(
        pi.hProcess,
        &child_exit);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION end_info{};

    QueryInformationJobObject(
        job,
        JobObjectExtendedLimitInformation,
        &end_info,
        sizeof(end_info),
        nullptr);

    DWORD peak_mb =
        static_cast<DWORD>(
            end_info.PeakProcessMemoryUsed /
            MB);

    if (safety_abort) {
        write_guard_json(
            "ABORTED_BY_SAFETY_WATCHDOG",
            abort_reason,
            start_available_mb,
            min_available_mb,
            peak_mb,
            child_exit);
    } else {
        write_guard_json(
            child_exit == 0
                ? "COMPLETED_PASS"
                : "COMPLETED_WORKER_FAIL",
            child_exit == 0
                ? "none"
                : "worker_gate_failed",
            start_available_mb,
            min_available_mb,
            peak_mb,
            child_exit);
    }

    std::cout
        << "\n--- Safety harness summary ---\n"
        << "Minimum Windows available memory observed: "
        << min_available_mb
        << " MiB\n"
        << "Peak isolated worker commit: "
        << peak_mb
        << " MiB\n"
        << "Worker exit code: "
        << child_exit
        << "\n";

    if (safety_abort) {
        std::cout
            << "STATE: SAFE_ABORT\n"
            << "The harness stopped StateRAM before allowing further pressure.\n";
    } else if (child_exit == 0) {
        std::cout
            << "STATE: COMPLETED_PASS\n"
            << "Please send me the console output or stateram_local_result.json.\n";
    } else {
        std::cout
            << "STATE: COMPLETED_WITH_FAILED_GATE\n"
            << "The PC itself was protected; send me the result so we can inspect the bottleneck.\n";
    }

    CloseHandle(low_mem);
    CloseHandle(pi.hProcess);
    CloseHandle(job);

    return safety_abort
        ? 50
        : static_cast<int>(child_exit);
}

int main(int argc, char** argv) {
    try {
        if (argc >= 2 &&
            std::string(argv[1]) == "--worker") {
            return worker_main();
        }

        return launcher_main();

    } catch (const std::exception& e) {
        std::cerr
            << "Fatal local harness error: "
            << e.what()
            << "\n";

        return 99;
    }
}
