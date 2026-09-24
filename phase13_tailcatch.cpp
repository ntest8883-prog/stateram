#define _WIN32_WINNT 0x0602
#define PSAPI_VERSION 1
#define NOMINMAX

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "stateram_sdk.h"

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Advapi32.lib")

static constexpr size_t PAGE_BYTES = 4096;
static constexpr uint64_t MB = 1024ull * 1024ull;

static constexpr size_t LOCAL_WORKSPACES = 6;
static constexpr size_t LOCAL_WORKSPACE_BYTES = 24ull * MB;
static constexpr size_t LOCAL_CORE_BYTES = 2ull * MB;
static constexpr size_t LOCAL_MAX_SWITCHES = 96;
static constexpr double LOCAL_TAIL_TRIGGER_MS = 50.0;

static constexpr uint64_t LOCAL_CHILD_COMMIT_CAP = 320ull * MB;
static constexpr DWORD LOCAL_CPU_RATE = 3500; // unchanged: 35% total CPU
static constexpr uint64_t START_MIN_AVAILABLE = 1024ull * MB;
static constexpr uint64_t ABORT_MIN_AVAILABLE = 640ull * MB;
static constexpr DWORD START_MAX_MEMORY_LOAD = 80;
static constexpr DWORD ABORT_MEMORY_LOAD = 88;
static constexpr DWORD LOCAL_TIMEOUT_MS = 120000;

struct Config {
    size_t workspaces;
    size_t workspace_bytes;
    size_t core_bytes;
    size_t max_switches;
    double tail_trigger_ms;
    uint32_t baseline_units_per_slice;
    uint32_t baseline_sleep_ms;
    bool cloud;
};

static LARGE_INTEGER qpc_frequency() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x{};
        QueryPerformanceFrequency(&x);
        return x;
    }();
    return f;
}

static int64_t qpc_now() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

static double qpc_delta_ms(
    int64_t begin,
    int64_t end
) {
    const auto f =
        qpc_frequency();

    return
        1000.0 *
        static_cast<double>(end - begin) /
        static_cast<double>(f.QuadPart);
}

static uint64_t ft64(
    const FILETIME& ft
) {
    ULARGE_INTEGER x{};
    x.LowPart = ft.dwLowDateTime;
    x.HighPart = ft.dwHighDateTime;
    return x.QuadPart;
}

static bool thread_cycles(
    uint64_t& out
) {
    ULONG64 x = 0;

    if (!QueryThreadCycleTime(
            GetCurrentThread(),
            &x)) {
        out = 0;
        return false;
    }

    out = x;
    return true;
}

static bool process_cycles(
    HANDLE process,
    uint64_t& out
) {
    ULONG64 x = 0;

    if (!QueryProcessCycleTime(
            process,
            &x)) {
        out = 0;
        return false;
    }

    out = x;
    return true;
}

struct MemSnapshot {
    DWORD page_faults = 0;
    double private_mb = 0.0;
    double working_set_mb = 0.0;
};

static bool process_memory(
    HANDLE process,
    MemSnapshot& out
) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    if (!GetProcessMemoryInfo(
            process,
            reinterpret_cast<
                PROCESS_MEMORY_COUNTERS*>(
                    &pmc),
            sizeof(pmc))) {
        return false;
    }

    out.page_faults =
        pmc.PageFaultCount;

    out.private_mb =
        static_cast<double>(
            pmc.PrivateUsage) /
        (1024.0 * 1024.0);

    out.working_set_mb =
        static_cast<double>(
            pmc.WorkingSetSize) /
        (1024.0 * 1024.0);

    return true;
}

struct SystemTimes {
    uint64_t idle = 0;
    uint64_t kernel = 0;
    uint64_t user = 0;
    bool ok = false;
};

static SystemTimes system_times() {
    FILETIME idle{}, kernel{}, user{};

    SystemTimes out{};

    if (GetSystemTimes(
            &idle,
            &kernel,
            &user)) {
        out.idle = ft64(idle);
        out.kernel = ft64(kernel);
        out.user = ft64(user);
        out.ok = true;
    }

    return out;
}

static double system_busy_pct(
    const SystemTimes& a,
    const SystemTimes& b
) {
    if (!a.ok || !b.ok) {
        return -1.0;
    }

    uint64_t idle =
        b.idle - a.idle;

    uint64_t kernel =
        b.kernel - a.kernel;

    uint64_t user =
        b.user - a.user;

    uint64_t total =
        kernel + user;

    if (total == 0 ||
        idle > total) {
        return -1.0;
    }

    return
        100.0 *
        static_cast<double>(
            total - idle) /
        static_cast<double>(
            total);
}

static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static void fill_page(
    uint8_t* page,
    uint64_t workspace,
    uint64_t page_index
) {
    uint64_t seed =
        workspace * 0xD1B54A32D192ED03ull +
        page_index;

    if ((page_index & 3ull) == 0) {
        auto* words =
            reinterpret_cast<uint64_t*>(page);

        for (size_t i = 0;
             i < PAGE_BYTES / 8;
             ++i) {
            words[i] =
                splitmix64(
                    seed *
                    0x9E3779B97F4A7C15ull +
                    i);
        }

        return;
    }

    uint8_t base =
        static_cast<uint8_t>(
            (seed * 37 + 19) &
            0xffu);

    std::memset(
        page,
        base,
        PAGE_BYTES);

    for (size_t off = 0;
         off < PAGE_BYTES;
         off += 512) {
        uint64_t marker =
            splitmix64(
                seed *
                0xA0761D6478BD642Full +
                off);

        std::memcpy(
            page + off,
            &marker,
            sizeof(marker));
    }
}

static uint64_t fnv1a64(
    const uint8_t* data,
    size_t bytes
) {
    uint64_t h =
        1469598103934665603ull;

    for (size_t i = 0;
         i < bytes;
         ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }

    return h;
}

template <typename T>
static T load_fn(
    HMODULE dll,
    const char* name
) {
    FARPROC p =
        GetProcAddress(
            dll,
            name);

    if (!p) {
        throw std::runtime_error(
            std::string(
                "missing SDK export: ") +
            name);
    }

    return reinterpret_cast<T>(p);
}

static bool current_process_is_elevated() {
    HANDLE token = nullptr;

    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &token)) {
        return true;
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

    return !ok ||
        elevation.TokenIsElevated != 0;
}

static bool read_memory_status(
    MEMORYSTATUSEX& ms
) {
    ZeroMemory(
        &ms,
        sizeof(ms));

    ms.dwLength =
        sizeof(ms);

    return GlobalMemoryStatusEx(
        &ms) != FALSE;
}

static std::wstring exe_path() {
    std::vector<wchar_t>
        buf(32768);

    DWORD n =
        GetModuleFileNameW(
            nullptr,
            buf.data(),
            static_cast<DWORD>(
                buf.size()));

    if (n == 0 ||
        n >= buf.size()) {
        throw std::runtime_error(
            "cannot locate executable");
    }

    return std::wstring(
        buf.data(),
        n);
}

struct SDK {
    using FApi = uint32_t (*)();
    using FCreate =
        SRHandle (*)(
            uint64_t,
            uint64_t);
    using FData =
        void* (*)(SRHandle);
    using FInt =
        int (*)(SRHandle);
    using FBegin =
        int (*)(
            SRHandle,
            uint32_t,
            uint32_t);
    using FWait =
        int (*)(
            SRHandle,
            uint32_t);
    using FMetrics =
        int (*)(
            SRHandle,
            SRMetrics*);
    using FDestroy =
        void (*)(SRHandle);

    HMODULE dll = nullptr;

    FApi api = nullptr;
    FCreate create = nullptr;
    FData data = nullptr;
    FBegin begin_baseline = nullptr;
    FWait wait_baseline = nullptr;
    FInt enter_dormant = nullptr;
    FInt resume_core = nullptr;
    FInt start_deep = nullptr;
    FInt deep_done = nullptr;
    FWait wait_deep = nullptr;
    FInt rearm = nullptr;
    FMetrics metrics = nullptr;
    FDestroy destroy = nullptr;

    SDK() {
        dll =
            LoadLibraryW(
                L"stateram_sdk.dll");

        if (!dll) {
            throw std::runtime_error(
                "could not load stateram_sdk.dll");
        }

        api =
            load_fn<FApi>(
                dll,
                "sr_api_version");

        create =
            load_fn<FCreate>(
                dll,
                "sr_create");

        data =
            load_fn<FData>(
                dll,
                "sr_data");

        begin_baseline =
            load_fn<FBegin>(
                dll,
                "sr_begin_background_baseline");

        wait_baseline =
            load_fn<FWait>(
                dll,
                "sr_wait_baseline");

        enter_dormant =
            load_fn<FInt>(
                dll,
                "sr_enter_dormant");

        resume_core =
            load_fn<FInt>(
                dll,
                "sr_resume_core");

        start_deep =
            load_fn<FInt>(
                dll,
                "sr_start_deep_restore");

        deep_done =
            load_fn<FInt>(
                dll,
                "sr_deep_done");

        wait_deep =
            load_fn<FWait>(
                dll,
                "sr_wait_deep");

        rearm =
            load_fn<FInt>(
                dll,
                "sr_rearm_existing_capsule");

        metrics =
            load_fn<FMetrics>(
                dll,
                "sr_get_metrics");

        destroy =
            load_fn<FDestroy>(
                dll,
                "sr_destroy");

        if ((api() >> 16) != 11) {
            throw std::runtime_error(
                "expected validated Phase 11 SDK");
        }
    }

    ~SDK() {
        if (dll) {
            FreeLibrary(dll);
        }
    }
};

struct Workspace {
    SRHandle h = nullptr;
    uint8_t* data = nullptr;
    uint64_t expected_hash = 0;
    uint64_t mutation_epoch = 0;
};

static void mutate_workspace(
    Workspace& w,
    size_t bytes,
    size_t workspace_id
) {
    const size_t pages =
        bytes /
        PAGE_BYTES;

    for (size_t i = 0;
         i < 24;
         ++i) {
        size_t page =
            (workspace_id * 911ull +
             w.mutation_epoch * 131ull +
             i * 977ull +
             17ull) %
            pages;

        uint8_t* p =
            w.data +
            page * PAGE_BYTES;

        p[31] ^=
            static_cast<uint8_t>(
                0x3D ^
                ((w.mutation_epoch + i) &
                 0xffu));

        p[2049] ^=
            static_cast<uint8_t>(
                0xC7 ^
                ((workspace_id * 13 + i) &
                 0xffu));
    }

    ++w.mutation_epoch;

    w.expected_hash =
        fnv1a64(
            w.data,
            bytes);
}

struct SwitchRecord {
    size_t index = 0;
    size_t from = 0;
    size_t to = 0;

    int64_t qpc_start = 0;
    int64_t qpc_after_dormant = 0;
    int64_t qpc_end = 0;

    double total_ms = 0.0;
    double dormant_ms = 0.0;
    double resume_ms = 0.0;

    uint64_t thread_cycles_before = 0;
    uint64_t thread_cycles_after = 0;
    uint64_t process_cycles_before = 0;
    uint64_t process_cycles_after = 0;

    DWORD page_faults_before = 0;
    DWORD page_faults_after = 0;

    double private_before_mb = 0.0;
    double private_after_mb = 0.0;
    double ws_before_mb = 0.0;
    double ws_after_mb = 0.0;

    DWORD memory_load_pct = 0;
    double available_mb = 0.0;

    double sdk_finalization_ms = 0.0;
    double sdk_decommit_ms = 0.0;
    double sdk_core_restore_ms = 0.0;
    uint64_t dirty_pages = 0;

    double system_busy_pct = -1.0;
    double deep_restore_ms = 0.0;

    bool tail = false;
};

static int run_worker(
    const Config& cfg
) {
    SDK sdk;

    std::vector<Workspace>
        ws(cfg.workspaces);

    auto cleanup = [&]() {
        for (auto& w : ws) {
            if (w.h) {
                sdk.destroy(w.h);
                w.h = nullptr;
                w.data = nullptr;
            }
        }
    };

    try {
        for (size_t i = 0;
             i < cfg.workspaces;
             ++i) {
            ws[i].h =
                sdk.create(
                    cfg.workspace_bytes,
                    cfg.core_bytes);

            if (!ws[i].h) {
                cleanup();
                return 60;
            }

            ws[i].data =
                static_cast<uint8_t*>(
                    sdk.data(
                        ws[i].h));

            if (!ws[i].data) {
                cleanup();
                return 61;
            }

            const size_t pages =
                cfg.workspace_bytes /
                PAGE_BYTES;

            for (size_t p = 0;
                 p < pages;
                 ++p) {
                fill_page(
                    ws[i].data +
                        p * PAGE_BYTES,
                    i,
                    p);
            }

            ws[i].expected_hash =
                fnv1a64(
                    ws[i].data,
                    cfg.workspace_bytes);
        }

        for (size_t i = 0;
             i < cfg.workspaces;
             ++i) {
            if (!sdk.begin_baseline(
                    ws[i].h,
                    cfg.baseline_units_per_slice,
                    cfg.baseline_sleep_ms) ||
                !sdk.wait_baseline(
                    ws[i].h,
                    60000) ||
                !sdk.enter_dormant(
                    ws[i].h)) {
                cleanup();
                return 62;
            }
        }

        if (!sdk.resume_core(
                ws[0].h) ||
            !sdk.start_deep(
                ws[0].h) ||
            !sdk.wait_deep(
                ws[0].h,
                30000) ||
            !sdk.rearm(
                ws[0].h)) {
            cleanup();
            return 63;
        }

        if (fnv1a64(
                ws[0].data,
                cfg.workspace_bytes) !=
            ws[0].expected_hash) {
            cleanup();
            return 64;
        }

        size_t current = 0;

        mutate_workspace(
            ws[current],
            cfg.workspace_bytes,
            current);

        const std::array<size_t, 24>
            pattern = {
                1,2,3,4,5,0,
                3,1,4,2,5,0,
                2,4,1,5,3,0,
                4,2,0,5,1,3
            };

        const char* csv_name =
            cfg.cloud
                ? "phase13_cloud_switches.csv"
                : "stateram_phase13_switches.csv";

        std::ofstream csv(
            csv_name,
            std::ios::binary);

        csv
            << "index,from,to,qpc_start,qpc_after_dormant,qpc_end,"
            << "total_ms,dormant_ms,resume_ms,"
            << "thread_cycle_delta,process_cycle_delta,"
            << "page_fault_delta,"
            << "private_before_mb,private_after_mb,"
            << "working_set_before_mb,working_set_after_mb,"
            << "memory_load_pct,available_mb,"
            << "sdk_finalization_ms,sdk_decommit_ms,sdk_core_restore_ms,"
            << "dirty_pages,system_busy_pct,deep_restore_ms,tail\n";

        std::vector<SwitchRecord>
            records;

        records.reserve(
            cfg.max_switches);

        bool tail_captured = false;
        size_t tail_index = 0;

        for (size_t s = 0;
             s < cfg.max_switches;
             ++s) {
            size_t target =
                pattern[
                    s %
                    pattern.size()] %
                cfg.workspaces;

            if (target == current) {
                target =
                    (target + 1) %
                    cfg.workspaces;
            }

            SwitchRecord r{};
            r.index = s;
            r.from = current;
            r.to = target;

            MEMORYSTATUSEX mem{};
            read_memory_status(mem);

            r.memory_load_pct =
                mem.dwMemoryLoad;

            r.available_mb =
                static_cast<double>(
                    mem.ullAvailPhys) /
                (1024.0 * 1024.0);

            MemSnapshot before{};
            MemSnapshot after{};

            process_memory(
                GetCurrentProcess(),
                before);

            r.page_faults_before =
                before.page_faults;
            r.private_before_mb =
                before.private_mb;
            r.ws_before_mb =
                before.working_set_mb;

            thread_cycles(
                r.thread_cycles_before);

            process_cycles(
                GetCurrentProcess(),
                r.process_cycles_before);

            SystemTimes sys0 =
                system_times();

            r.qpc_start =
                qpc_now();

            if (!sdk.enter_dormant(
                    ws[current].h)) {
                cleanup();
                return 65;
            }

            r.qpc_after_dormant =
                qpc_now();

            SRMetrics out_m{};

            if (!sdk.metrics(
                    ws[current].h,
                    &out_m)) {
                cleanup();
                return 66;
            }

            r.sdk_finalization_ms =
                out_m.finalization_ms;

            r.sdk_decommit_ms =
                out_m.decommit_ms;

            r.dirty_pages =
                out_m.dirty_pages;

            if (!sdk.resume_core(
                    ws[target].h)) {
                cleanup();
                return 67;
            }

            r.qpc_end =
                qpc_now();

            thread_cycles(
                r.thread_cycles_after);

            process_cycles(
                GetCurrentProcess(),
                r.process_cycles_after);

            process_memory(
                GetCurrentProcess(),
                after);

            SystemTimes sys1 =
                system_times();

            SRMetrics in_m{};

            if (!sdk.metrics(
                    ws[target].h,
                    &in_m)) {
                cleanup();
                return 68;
            }

            r.sdk_core_restore_ms =
                in_m.core_restore_ms;

            r.page_faults_after =
                after.page_faults;

            r.private_after_mb =
                after.private_mb;

            r.ws_after_mb =
                after.working_set_mb;

            r.dormant_ms =
                qpc_delta_ms(
                    r.qpc_start,
                    r.qpc_after_dormant);

            r.resume_ms =
                qpc_delta_ms(
                    r.qpc_after_dormant,
                    r.qpc_end);

            r.total_ms =
                qpc_delta_ms(
                    r.qpc_start,
                    r.qpc_end);

            r.system_busy_pct =
                system_busy_pct(
                    sys0,
                    sys1);

            r.tail =
                r.total_ms >=
                cfg.tail_trigger_ms;

            if (!sdk.start_deep(
                    ws[target].h)) {
                cleanup();
                return 69;
            }

            int64_t deep0 =
                qpc_now();

            while (sdk.deep_done(
                       ws[target].h) == 0) {
                volatile uint64_t sum = 0;

                for (size_t off = 0;
                     off < cfg.core_bytes;
                     off += 16 * 1024) {
                    sum +=
                        ws[target].data[off];
                }

                if (sum ==
                    0xFFFFFFFFFFFFFFFFull) {
                    std::cout << "";
                }

                Sleep(0);
            }

            if (!sdk.wait_deep(
                    ws[target].h,
                    30000)) {
                cleanup();
                return 70;
            }

            r.deep_restore_ms =
                qpc_delta_ms(
                    deep0,
                    qpc_now());

            if (fnv1a64(
                    ws[target].data,
                    cfg.workspace_bytes) !=
                ws[target].expected_hash) {
                cleanup();
                return 71;
            }

            if (!sdk.rearm(
                    ws[target].h)) {
                cleanup();
                return 72;
            }

            mutate_workspace(
                ws[target],
                cfg.workspace_bytes,
                target);

            records.push_back(r);

            const uint64_t thread_delta =
                r.thread_cycles_after >=
                        r.thread_cycles_before
                    ? r.thread_cycles_after -
                        r.thread_cycles_before
                    : 0;

            const uint64_t process_delta =
                r.process_cycles_after >=
                        r.process_cycles_before
                    ? r.process_cycles_after -
                        r.process_cycles_before
                    : 0;

            const uint64_t pf_delta =
                r.page_faults_after >=
                        r.page_faults_before
                    ? static_cast<uint64_t>(
                          r.page_faults_after -
                          r.page_faults_before)
                    : 0;

            csv
                << r.index << ","
                << r.from << ","
                << r.to << ","
                << r.qpc_start << ","
                << r.qpc_after_dormant << ","
                << r.qpc_end << ","
                << std::fixed
                << std::setprecision(3)
                << r.total_ms << ","
                << r.dormant_ms << ","
                << r.resume_ms << ","
                << thread_delta << ","
                << process_delta << ","
                << pf_delta << ","
                << r.private_before_mb << ","
                << r.private_after_mb << ","
                << r.ws_before_mb << ","
                << r.ws_after_mb << ","
                << r.memory_load_pct << ","
                << r.available_mb << ","
                << r.sdk_finalization_ms << ","
                << r.sdk_decommit_ms << ","
                << r.sdk_core_restore_ms << ","
                << r.dirty_pages << ","
                << r.system_busy_pct << ","
                << r.deep_restore_ms << ","
                << (r.tail ? 1 : 0)
                << "\n";

            csv.flush();

            current =
                target;

            if (r.tail) {
                tail_captured = true;
                tail_index = s;
                break;
            }
        }

        bool integrity_ok = true;

        if (fnv1a64(
                ws[current].data,
                cfg.workspace_bytes) !=
            ws[current].expected_hash) {
            integrity_ok = false;
        }

        if (records.empty()) {
            cleanup();
            return 73;
        }

        std::vector<double> normal_ms;
        std::vector<uint64_t> normal_thread_cycles;
        std::vector<uint64_t> normal_process_cycles;

        for (const auto& r : records) {
            if (!r.tail) {
                normal_ms.push_back(
                    r.total_ms);

                normal_thread_cycles.push_back(
                    r.thread_cycles_after -
                    r.thread_cycles_before);

                normal_process_cycles.push_back(
                    r.process_cycles_after -
                    r.process_cycles_before);
            }
        }

        auto median_double =
            [](std::vector<double> v) {
                if (v.empty()) return 0.0;
                std::sort(v.begin(), v.end());
                return v[v.size() / 2];
            };

        auto median_u64 =
            [](std::vector<uint64_t> v) {
                if (v.empty()) return uint64_t{0};
                std::sort(v.begin(), v.end());
                return v[v.size() / 2];
            };

        double normal_median_ms =
            median_double(normal_ms);

        uint64_t normal_median_thread_cycles =
            median_u64(normal_thread_cycles);

        uint64_t normal_median_process_cycles =
            median_u64(normal_process_cycles);

        SwitchRecord interesting =
            tail_captured
                ? records[tail_index]
                : *std::max_element(
                    records.begin(),
                    records.end(),
                    [](
                        const SwitchRecord& a,
                        const SwitchRecord& b
                    ) {
                        return a.total_ms <
                               b.total_ms;
                    });

        uint64_t interesting_thread_delta =
            interesting.thread_cycles_after -
            interesting.thread_cycles_before;

        uint64_t interesting_process_delta =
            interesting.process_cycles_after -
            interesting.process_cycles_before;

        uint64_t interesting_pf_delta =
            interesting.page_faults_after -
            interesting.page_faults_before;

        double thread_cycle_ratio =
            normal_median_thread_cycles > 0
                ? static_cast<double>(
                      interesting_thread_delta) /
                  static_cast<double>(
                      normal_median_thread_cycles)
                : 0.0;

        double process_cycle_ratio =
            normal_median_process_cycles > 0
                ? static_cast<double>(
                      interesting_process_delta) /
                  static_cast<double>(
                      normal_median_process_cycles)
                : 0.0;

        std::string interpretation =
            "NO_RARE_STALL_CAPTURED";

        if (tail_captured) {
            const double wall_ratio =
                normal_median_ms > 0.0
                    ? interesting.total_ms /
                      normal_median_ms
                    : 0.0;

            /*
              We do NOT convert cycles to milliseconds. Microsoft explicitly
              warns against that. We compare raw cycle deltas with the median
              normal switch on the same machine/run.

              Heuristic:
              - huge wall inflation with much smaller cycle inflation:
                execution was delayed/blocking/throttled rather than CPU work.
              - cycles scale with wall:
                CPU work itself expanded.
              - page faults jump sharply:
                memory-fault activity is correlated with the event.
            */
            if (interesting_pf_delta >= 128) {
                interpretation =
                    "TAIL_CORRELATED_WITH_PAGE_FAULT_BURST";
            } else if (
                wall_ratio >= 4.0 &&
                thread_cycle_ratio <=
                    std::max(
                        2.0,
                        wall_ratio * 0.35)) {
                interpretation =
                    "TAIL_MOSTLY_NOT_FOREGROUND_CPU";
            } else if (
                wall_ratio >= 4.0 &&
                thread_cycle_ratio >=
                    wall_ratio * 0.60) {
                interpretation =
                    "TAIL_CPU_WORK_EXPANDED";
            } else {
                interpretation =
                    "TAIL_CAPTURED_MIXED_OR_AMBIGUOUS";
            }
        }

        const char* json_name =
            cfg.cloud
                ? "phase13_cloud_result.json"
                : "stateram_phase13_tailcatch_result.json";

        std::ofstream out(
            json_name,
            std::ios::binary);

        out
            << "{\n"
            << "  \"phase\": \"Windows Phase 13 rare-stall catcher\",\n"
            << "  \"mode\": \""
            << (cfg.cloud ? "cloud" : "local")
            << "\",\n"
            << "  \"tail_trigger_ms\": "
            << cfg.tail_trigger_ms << ",\n"
            << "  \"switches_completed\": "
            << records.size() << ",\n"
            << "  \"max_switches_allowed\": "
            << cfg.max_switches << ",\n"
            << "  \"tail_captured\": "
            << (tail_captured ? "true" : "false") << ",\n"
            << "  \"integrity_ok\": "
            << (integrity_ok ? "true" : "false") << ",\n"
            << "  \"normal_median_switch_ms\": "
            << std::fixed
            << std::setprecision(3)
            << normal_median_ms << ",\n"
            << "  \"normal_median_thread_cycles\": "
            << normal_median_thread_cycles << ",\n"
            << "  \"normal_median_process_cycles\": "
            << normal_median_process_cycles << ",\n"
            << "  \"interesting_switch_index\": "
            << interesting.index << ",\n"
            << "  \"interesting_from\": "
            << interesting.from << ",\n"
            << "  \"interesting_to\": "
            << interesting.to << ",\n"
            << "  \"interesting_total_ms\": "
            << interesting.total_ms << ",\n"
            << "  \"interesting_dormant_ms\": "
            << interesting.dormant_ms << ",\n"
            << "  \"interesting_resume_ms\": "
            << interesting.resume_ms << ",\n"
            << "  \"interesting_thread_cycle_delta\": "
            << interesting_thread_delta << ",\n"
            << "  \"interesting_process_cycle_delta\": "
            << interesting_process_delta << ",\n"
            << "  \"thread_cycle_ratio_vs_normal_median\": "
            << thread_cycle_ratio << ",\n"
            << "  \"process_cycle_ratio_vs_normal_median\": "
            << process_cycle_ratio << ",\n"
            << "  \"interesting_page_fault_delta\": "
            << interesting_pf_delta << ",\n"
            << "  \"interesting_sdk_finalization_ms\": "
            << interesting.sdk_finalization_ms << ",\n"
            << "  \"interesting_sdk_decommit_ms\": "
            << interesting.sdk_decommit_ms << ",\n"
            << "  \"interesting_sdk_core_restore_ms\": "
            << interesting.sdk_core_restore_ms << ",\n"
            << "  \"interesting_system_busy_pct\": "
            << interesting.system_busy_pct << ",\n"
            << "  \"interesting_memory_load_pct\": "
            << interesting.memory_load_pct << ",\n"
            << "  \"interesting_available_mb\": "
            << interesting.available_mb << ",\n"
            << "  \"interesting_deep_restore_ms\": "
            << interesting.deep_restore_ms << ",\n"
            << "  \"interpretation\": \""
            << interpretation << "\",\n"
            << "  \"diagnostic_pass\": "
            << (integrity_ok
                ? "true" : "false")
            << "\n"
            << "}\n";

        out.close();
        csv.close();

        std::cout
            << "StateRAM Phase 13 - rare-stall catcher\n"
            << " completed switches="
            << records.size()
            << "/"
            << cfg.max_switches
            << "\n"
            << " tail >= "
            << cfg.tail_trigger_ms
            << " ms captured="
            << (tail_captured ? "YES" : "NO")
            << "\n"
            << " normal median="
            << normal_median_ms
            << " ms\n"
            << " interesting switch #"
            << interesting.index
            << " "
            << interesting.from
            << "->"
            << interesting.to
            << " wall="
            << interesting.total_ms
            << " ms\n"
            << " thread cycles delta="
            << interesting_thread_delta
            << " ratio-vs-normal="
            << thread_cycle_ratio
            << "\n"
            << " process cycles delta="
            << interesting_process_delta
            << " ratio-vs-normal="
            << process_cycle_ratio
            << "\n"
            << " page-fault delta="
            << interesting_pf_delta
            << "\n"
            << " interpretation="
            << interpretation
            << "\n"
            << " integrity="
            << (integrity_ok ? "OK" : "FAIL")
            << "\n"
            << " WINDOWS_STATERAM_PHASE13_DIAGNOSTIC="
            << (integrity_ok ? "PASS" : "FAIL")
            << "\n";

        cleanup();

        return integrity_ok
            ? 0
            : 80;

    } catch (...) {
        cleanup();
        throw;
    }
}

static void write_guard_json(
    const char* status,
    const char* reason,
    double start_available_mb,
    double min_available_mb,
    DWORD peak_process_mb,
    DWORD child_exit
) {
    std::ofstream out(
        "stateram_phase13_guard_result.json",
        std::ios::binary);

    out
        << "{\n"
        << "  \"status\": \""
        << status << "\",\n"
        << "  \"reason\": \""
        << reason << "\",\n"
        << "  \"start_available_mb\": "
        << std::fixed
        << std::setprecision(1)
        << start_available_mb << ",\n"
        << "  \"minimum_available_mb\": "
        << min_available_mb << ",\n"
        << "  \"job_peak_process_memory_mb\": "
        << peak_process_mb << ",\n"
        << "  \"child_exit_code\": "
        << child_exit << "\n"
        << "}\n";
}

static int launcher_main() {
    std::cout
        << "======================================================\n"
        << " StateRAM Phase 13 - rare-stall catcher\n"
        << "======================================================\n\n"
        << "This keeps the Phase 11 engine unchanged.\n"
        << "It repeats the same safe switching pattern and stops at the\n"
        << "FIRST switch >= 50 ms, or after 96 switches if no tail appears.\n\n";

    if (current_process_is_elevated()) {
        std::cout
            << "SAFETY STOP: run normally, not as Administrator.\n";
        return 2;
    }

    MEMORYSTATUSEX start{};

    if (!read_memory_status(
            start)) {
        return 3;
    }

    double start_available_mb =
        static_cast<double>(
            start.ullAvailPhys) /
        (1024.0 * 1024.0);

    std::cout
        << "Windows memory load now: "
        << start.dwMemoryLoad
        << "%\n"
        << "Available physical memory: "
        << std::fixed
        << std::setprecision(1)
        << start_available_mb
        << " MiB\n";

    if (start.dwMemoryLoad >
            START_MAX_MEMORY_LOAD ||
        start.ullAvailPhys <
            START_MIN_AVAILABLE) {
        std::cout
            << "\nSAFETY STOP: insufficient current memory headroom.\n"
            << "Nothing was started.\n";

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

    if (!job) return 5;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        limits{};

    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    limits.BasicLimitInformation.ActiveProcessLimit =
        1;

    limits.ProcessMemoryLimit =
        static_cast<SIZE_T>(
            LOCAL_CHILD_COMMIT_CAP);

    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))) {
        CloseHandle(job);
        return 6;
    }

    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION
        cpu{};

    cpu.ControlFlags =
        JOB_OBJECT_CPU_RATE_CONTROL_ENABLE |
        JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;

    cpu.CpuRate =
        LOCAL_CPU_RATE;

    if (!SetInformationJobObject(
            job,
            JobObjectCpuRateControlInformation,
            &cpu,
            sizeof(cpu))) {
        CloseHandle(job);
        return 7;
    }

    std::wstring cmd =
        L"\"" +
        exe_path() +
        L"\" --worker";

    std::vector<wchar_t>
        mutable_cmd(
            cmd.begin(),
            cmd.end());

    mutable_cmd.push_back(
        L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(
            nullptr,
            mutable_cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED |
            BELOW_NORMAL_PRIORITY_CLASS,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        CloseHandle(job);
        return 8;
    }

    if (!AssignProcessToJobObject(
            job,
            pi.hProcess)) {
        TerminateProcess(
            pi.hProcess,
            90);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return 9;
    }

    HANDLE low_mem =
        CreateMemoryResourceNotification(
            LowMemoryResourceNotification);

    if (!low_mem) {
        TerminateJobObject(
            job,
            91);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return 10;
    }

    std::ofstream trace(
        "stateram_phase13_process_trace.csv",
        std::ios::binary);

    trace
        << "qpc,process_cycle_count,page_fault_count,"
        << "private_mb,working_set_mb,"
        << "system_idle_100ns,system_kernel_100ns,system_user_100ns,"
        << "memory_load_pct,available_mb\n";

    std::cout
        << "\nSafety remains unchanged:\n"
        << "  child hard commit cap: 320 MiB\n"
        << "  child hard CPU cap: 35% total CPU\n"
        << "  BELOW_NORMAL priority\n"
        << "  abort at >=88% memory load or <640 MiB available\n"
        << "  120 second timeout\n"
        << "  launcher samples child telemetry every ~10 ms\n\n";

    ResumeThread(
        pi.hThread);

    CloseHandle(
        pi.hThread);

    double min_available_mb =
        start_available_mb;

    bool safety_abort = false;
    const char* abort_reason = "";

    DWORD started =
        GetTickCount();

    DWORD last_safety_check =
        started;

    for (;;) {
        DWORD w =
            WaitForSingleObject(
                pi.hProcess,
                10);

        if (w == WAIT_OBJECT_0) {
            break;
        }

        if (w != WAIT_TIMEOUT) {
            safety_abort = true;
            abort_reason =
                "worker_wait_failure";
            break;
        }

        MEMORYSTATUSEX ms{};
        read_memory_status(ms);

        MemSnapshot pm{};
        process_memory(
            pi.hProcess,
            pm);

        uint64_t proc_cycles = 0;
        process_cycles(
            pi.hProcess,
            proc_cycles);

        SystemTimes st =
            system_times();

        double available_mb =
            static_cast<double>(
                ms.ullAvailPhys) /
            (1024.0 * 1024.0);

        min_available_mb =
            std::min(
                min_available_mb,
                available_mb);

        trace
            << qpc_now() << ","
            << proc_cycles << ","
            << pm.page_faults << ","
            << std::fixed
            << std::setprecision(3)
            << pm.private_mb << ","
            << pm.working_set_mb << ","
            << st.idle << ","
            << st.kernel << ","
            << st.user << ","
            << ms.dwMemoryLoad << ","
            << available_mb
            << "\n";

        DWORD now_tick =
            GetTickCount();

        if (now_tick -
                last_safety_check >=
            250) {
            last_safety_check =
                now_tick;

            BOOL low = FALSE;

            if (!QueryMemoryResourceNotification(
                    low_mem,
                    &low)) {
                safety_abort = true;
                abort_reason =
                    "low_memory_watchdog_failure";
                break;
            }

            if (low) {
                safety_abort = true;
                abort_reason =
                    "windows_low_memory_signal";
                break;
            }

            if (ms.dwMemoryLoad >=
                ABORT_MEMORY_LOAD) {
                safety_abort = true;
                abort_reason =
                    "memory_load_limit";
                break;
            }

            if (ms.ullAvailPhys <
                ABORT_MIN_AVAILABLE) {
                safety_abort = true;
                abort_reason =
                    "available_memory_limit";
                break;
            }
        }

        if (now_tick -
                started >=
            LOCAL_TIMEOUT_MS) {
            safety_abort = true;
            abort_reason =
                "timeout";
            break;
        }
    }

    trace.flush();
    trace.close();

    if (safety_abort) {
        TerminateJobObject(
            job,
            92);

        WaitForSingleObject(
            pi.hProcess,
            5000);
    }

    DWORD child_exit = 999;

    GetExitCodeProcess(
        pi.hProcess,
        &child_exit);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        info{};

    QueryInformationJobObject(
        job,
        JobObjectExtendedLimitInformation,
        &info,
        sizeof(info),
        nullptr);

    DWORD peak_mb =
        static_cast<DWORD>(
            info.PeakProcessMemoryUsed /
            MB);

    write_guard_json(
        safety_abort
            ? "ABORTED_BY_SAFETY_WATCHDOG"
            : (child_exit == 0
                ? "COMPLETED_PASS"
                : "COMPLETED_WORKER_FAIL"),
        safety_abort
            ? abort_reason
            : (child_exit == 0
                ? "none"
                : "worker_diagnostic_failed"),
        start_available_mb,
        min_available_mb,
        peak_mb,
        child_exit);

    std::cout
        << "\n--- Phase 13 safety summary ---\n"
        << "Minimum available RAM observed: "
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
            << "STATE: SAFE_ABORT\n";
    } else if (child_exit == 0) {
        std::cout
            << "STATE: COMPLETED_PASS\n"
            << "Send ChatGPT these FOUR files:\n"
            << "  stateram_phase13_tailcatch_result.json\n"
            << "  stateram_phase13_switches.csv\n"
            << "  stateram_phase13_process_trace.csv\n"
            << "  stateram_phase13_guard_result.json\n";
    } else {
        std::cout
            << "STATE: COMPLETED_WITH_FAILED_DIAGNOSTIC\n";
    }

    CloseHandle(low_mem);
    CloseHandle(pi.hProcess);
    CloseHandle(job);

    return safety_abort
        ? 50
        : static_cast<int>(
            child_exit);
}

int main(
    int argc,
    char** argv
) {
    try {
        if (argc >= 2) {
            std::string mode =
                argv[1];

            if (mode ==
                "--cloud-smoke") {
                Config cfg{
                    4,
                    8ull * MB,
                    1ull * MB,
                    12,
                    50.0,
                    2,
                    0,
                    true
                };

                return
                    run_worker(cfg);
            }

            if (mode ==
                "--worker") {
                Config cfg{
                    LOCAL_WORKSPACES,
                    LOCAL_WORKSPACE_BYTES,
                    LOCAL_CORE_BYTES,
                    LOCAL_MAX_SWITCHES,
                    LOCAL_TAIL_TRIGGER_MS,
                    1,
                    3,
                    false
                };

                return
                    run_worker(cfg);
            }
        }

        return launcher_main();

    } catch (
        const std::exception& e
    ) {
        std::cerr
            << "Phase 13 error: "
            << e.what()
            << "\n";

        return 99;
    }
}
