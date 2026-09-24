#define _WIN32_WINNT 0x0602
#define PSAPI_VERSION 1
#define NOMINMAX

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
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
static constexpr size_t LOCAL_SWITCHES = 96;
static constexpr double TAIL_THRESHOLD_MS = 50.0;

static constexpr uint64_t CHILD_COMMIT_CAP = 320ull * MB;
static constexpr uint64_t START_MIN_AVAILABLE = 1024ull * MB;
static constexpr uint64_t ABORT_MIN_AVAILABLE = 640ull * MB;
static constexpr DWORD START_MAX_MEMORY_LOAD = 80;
static constexpr DWORD ABORT_MEMORY_LOAD = 88;
static constexpr DWORD TIMEOUT_MS = 120000;

struct Config {
    size_t workspaces;
    size_t workspace_bytes;
    size_t core_bytes;
    size_t switches;
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

static double delta_ms(
    int64_t a,
    int64_t b
) {
    LARGE_INTEGER f =
        qpc_frequency();

    return
        1000.0 *
        static_cast<double>(b - a) /
        static_cast<double>(f.QuadPart);
}

static uint64_t filetime64(
    const FILETIME& ft
) {
    ULARGE_INTEGER x{};
    x.LowPart = ft.dwLowDateTime;
    x.HighPart = ft.dwHighDateTime;
    return x.QuadPart;
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
        workspace *
            0xD1B54A32D192ED03ull +
        page_index;

    if ((page_index & 3ull) == 0) {
        auto* words =
            reinterpret_cast<uint64_t*>(
                page);

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

static double percentile(
    std::vector<double> v,
    double p
) {
    if (v.empty()) return 0.0;

    std::sort(
        v.begin(),
        v.end());

    size_t idx =
        static_cast<size_t>(
            p *
            static_cast<double>(
                v.size() - 1));

    return v[idx];
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

        if ((api() >> 16) != 14) {
            throw std::runtime_error(
                "expected Phase 14 SDK ABI");
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
    size_t pages =
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

struct SwitchRow {
    size_t index = 0;
    size_t from = 0;
    size_t to = 0;
    double total_ms = 0.0;
    double dormant_ms = 0.0;
    double resume_ms = 0.0;
    double finalization_ms = 0.0;
    double decommit_ms = 0.0;
    double core_restore_ms = 0.0;
    double deep_restore_ms = 0.0;
    DWORD memory_load_pct = 0;
    double available_mb = 0.0;
    bool tail = false;
};

static int run_experiment(
    const Config& cfg
) {
    SDK sdk;

    std::vector<Workspace>
        ws(cfg.workspaces);

    auto cleanup = [&]() {
        for (auto& w : ws) {
            if (w.h) {
                sdk.destroy(
                    w.h);

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

            size_t pages =
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
                ? "phase14_cloud_switches.csv"
                : "stateram_phase14_switches.csv";

        std::ofstream csv(
            csv_name,
            std::ios::binary);

        csv
            << "index,from,to,total_ms,dormant_ms,resume_ms,"
            << "sdk_finalization_ms,sdk_decommit_ms,sdk_core_restore_ms,"
            << "deep_restore_ms,memory_load_pct,available_mb,tail\n";

        std::vector<SwitchRow>
            rows;

        rows.reserve(
            cfg.switches);

        size_t tails = 0;
        size_t integrity_checks = 0;
        size_t integrity_failures = 0;

        for (size_t s = 0;
             s < cfg.switches;
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

            MEMORYSTATUSEX mem{};
            read_memory_status(mem);

            SwitchRow row{};
            row.index = s;
            row.from = current;
            row.to = target;
            row.memory_load_pct =
                mem.dwMemoryLoad;
            row.available_mb =
                static_cast<double>(
                    mem.ullAvailPhys) /
                (1024.0 * 1024.0);

            int64_t t0 =
                qpc_now();

            int64_t d0 =
                qpc_now();

            if (!sdk.enter_dormant(
                    ws[current].h)) {
                cleanup();
                return 65;
            }

            int64_t d1 =
                qpc_now();

            SRMetrics out_m{};

            if (!sdk.metrics(
                    ws[current].h,
                    &out_m)) {
                cleanup();
                return 66;
            }

            row.dormant_ms =
                delta_ms(
                    d0,
                    d1);

            row.finalization_ms =
                out_m.finalization_ms;

            row.decommit_ms =
                out_m.decommit_ms;

            int64_t r0 =
                qpc_now();

            if (!sdk.resume_core(
                    ws[target].h)) {
                cleanup();
                return 67;
            }

            int64_t r1 =
                qpc_now();

            SRMetrics in_m{};

            if (!sdk.metrics(
                    ws[target].h,
                    &in_m)) {
                cleanup();
                return 68;
            }

            row.resume_ms =
                delta_ms(
                    r0,
                    r1);

            row.core_restore_ms =
                in_m.core_restore_ms;

            row.total_ms =
                delta_ms(
                    t0,
                    r1);

            row.tail =
                row.total_ms >=
                TAIL_THRESHOLD_MS;

            if (row.tail) {
                ++tails;
            }

            if (!sdk.start_deep(
                    ws[target].h)) {
                cleanup();
                return 69;
            }

            int64_t deep0 =
                qpc_now();

            /*
              Do not busy-spin on the weak machine.
              The deep thread is explicitly background-mode in Phase 14.
              We touch the 2 MiB foreground core at a modest cadence while
              waiting, then sleep 1 ms so the harness itself cannot consume
              a second CPU core continuously.
            */
            while (sdk.deep_done(
                       ws[target].h) == 0) {
                volatile uint64_t sum = 0;

                for (size_t off = 0;
                     off < cfg.core_bytes;
                     off += 64 * 1024) {
                    sum +=
                        ws[target].data[off];
                }

                if (sum ==
                    0xFFFFFFFFFFFFFFFFull) {
                    std::cout << "";
                }

                Sleep(1);
            }

            if (!sdk.wait_deep(
                    ws[target].h,
                    30000)) {
                cleanup();
                return 70;
            }

            row.deep_restore_ms =
                delta_ms(
                    deep0,
                    qpc_now());

            ++integrity_checks;

            if (fnv1a64(
                    ws[target].data,
                    cfg.workspace_bytes) !=
                ws[target].expected_hash) {
                ++integrity_failures;
            }

            if (!sdk.rearm(
                    ws[target].h)) {
                cleanup();
                return 71;
            }

            mutate_workspace(
                ws[target],
                cfg.workspace_bytes,
                target);

            rows.push_back(row);

            csv
                << row.index << ","
                << row.from << ","
                << row.to << ","
                << std::fixed
                << std::setprecision(3)
                << row.total_ms << ","
                << row.dormant_ms << ","
                << row.resume_ms << ","
                << row.finalization_ms << ","
                << row.decommit_ms << ","
                << row.core_restore_ms << ","
                << row.deep_restore_ms << ","
                << row.memory_load_pct << ","
                << row.available_mb << ","
                << (row.tail ? 1 : 0)
                << "\n";

            csv.flush();

            current =
                target;
        }

        std::vector<double>
            switches;

        std::vector<double>
            deep_times;

        for (const auto& r : rows) {
            switches.push_back(
                r.total_ms);

            deep_times.push_back(
                r.deep_restore_ms);
        }

        const double p50 =
            percentile(
                switches,
                0.50);

        const double p95 =
            percentile(
                switches,
                0.95);

        const double p99 =
            percentile(
                switches,
                0.99);

        const double max_ms =
            switches.empty()
                ? 0.0
                : *std::max_element(
                    switches.begin(),
                    switches.end());

        const double deep_p50 =
            percentile(
                deep_times,
                0.50);

        const double deep_p95 =
            percentile(
                deep_times,
                0.95);

        const double deep_max =
            deep_times.empty()
                ? 0.0
                : *std::max_element(
                    deep_times.begin(),
                    deep_times.end());

        uint64_t deep_yields = 0;
        uint64_t deep_slices = 0;
        uint64_t deep_pressure_yields = 0;
        size_t deep_background_regions = 0;

        for (size_t i = 0;
             i < cfg.workspaces;
             ++i) {
            SRMetrics m{};

            if (!sdk.metrics(
                    ws[i].h,
                    &m)) {
                cleanup();
                return 72;
            }

            deep_yields +=
                m.deep_yields;

            deep_slices +=
                m.deep_slices;

            deep_pressure_yields +=
                m.deep_pressure_yields;

            if (m.deep_background_mode_entered) {
                ++deep_background_regions;
            }
        }

        const bool integrity_ok =
            integrity_checks ==
                cfg.switches &&
            integrity_failures == 0;

        const bool latency_ok =
            tails == 0 &&
            p99 < 20.0 &&
            max_ms <
                TAIL_THRESHOLD_MS;

        const bool background_ok =
            deep_background_regions ==
                cfg.workspaces &&
            deep_yields > 0 &&
            deep_slices > 0;

        const bool pass =
            integrity_ok &&
            latency_ok &&
            background_ok;

        const char* json_name =
            cfg.cloud
                ? "phase14_cloud_result.json"
                : "stateram_phase14_result.json";

        std::ofstream out(
            json_name,
            std::ios::binary);

        out
            << "{\n"
            << "  \"phase\": \"Windows Phase 14 foreground-aware CPU governor\",\n"
            << "  \"mode\": \""
            << (cfg.cloud ? "cloud" : "local")
            << "\",\n"
            << "  \"sdk_api_version\": "
            << sdk.api() << ",\n"
            << "  \"switches\": "
            << rows.size() << ",\n"
            << "  \"tail_threshold_ms\": "
            << TAIL_THRESHOLD_MS << ",\n"
            << "  \"tails_ge_50ms\": "
            << tails << ",\n"
            << "  \"switch_p50_ms\": "
            << std::fixed
            << std::setprecision(3)
            << p50 << ",\n"
            << "  \"switch_p95_ms\": "
            << p95 << ",\n"
            << "  \"switch_p99_ms\": "
            << p99 << ",\n"
            << "  \"switch_max_ms\": "
            << max_ms << ",\n"
            << "  \"deep_restore_p50_ms\": "
            << deep_p50 << ",\n"
            << "  \"deep_restore_p95_ms\": "
            << deep_p95 << ",\n"
            << "  \"deep_restore_max_ms\": "
            << deep_max << ",\n"
            << "  \"deep_slices\": "
            << deep_slices << ",\n"
            << "  \"deep_yields\": "
            << deep_yields << ",\n"
            << "  \"deep_pressure_yields\": "
            << deep_pressure_yields << ",\n"
            << "  \"deep_background_regions\": "
            << deep_background_regions << ",\n"
            << "  \"integrity_checks\": "
            << integrity_checks << ",\n"
            << "  \"integrity_failures\": "
            << integrity_failures << ",\n"
            << "  \"integrity_ok\": "
            << (integrity_ok ? "true" : "false") << ",\n"
            << "  \"latency_ok\": "
            << (latency_ok ? "true" : "false") << ",\n"
            << "  \"background_governor_ok\": "
            << (background_ok ? "true" : "false") << ",\n"
            << "  \"pass\": "
            << (pass ? "true" : "false") << "\n"
            << "}\n";

        out.close();
        csv.close();

        std::cout
            << "StateRAM Phase 14 - foreground-aware governor\n"
            << " switches="
            << rows.size()
            << " tails>=50ms="
            << tails
            << "\n"
            << " switch p50="
            << p50
            << " ms p95="
            << p95
            << " ms p99="
            << p99
            << " ms max="
            << max_ms
            << " ms\n"
            << " deep p50="
            << deep_p50
            << " ms p95="
            << deep_p95
            << " ms max="
            << deep_max
            << " ms\n"
            << " deep background regions="
            << deep_background_regions
            << "/"
            << cfg.workspaces
            << " yields="
            << deep_yields
            << "\n"
            << " integrity="
            << (integrity_ok ? "OK" : "FAIL")
            << "\n"
            << " WINDOWS_STATERAM_PHASE14="
            << (pass ? "PASS" : "FAIL")
            << "\n";

        cleanup();

        return pass ? 0 : 80;

    } catch (...) {
        cleanup();
        throw;
    }
}

struct CpuWatch {
    bool initialized = false;
    uint64_t process_100ns = 0;
    int64_t qpc = 0;
};

static double sample_child_cpu_pct(
    HANDLE process,
    CpuWatch& s,
    DWORD logical_processors
) {
    FILETIME create{}, exit{}, kernel{}, user{};

    if (!GetProcessTimes(
            process,
            &create,
            &exit,
            &kernel,
            &user)) {
        return -1.0;
    }

    uint64_t now_proc =
        filetime64(kernel) +
        filetime64(user);

    int64_t now_qpc =
        qpc_now();

    if (!s.initialized) {
        s.initialized = true;
        s.process_100ns =
            now_proc;
        s.qpc =
            now_qpc;
        return 0.0;
    }

    uint64_t dp =
        now_proc -
        s.process_100ns;

    double wall_ms =
        delta_ms(
            s.qpc,
            now_qpc);

    s.process_100ns =
        now_proc;
    s.qpc =
        now_qpc;

    if (wall_ms <= 0.0 ||
        logical_processors == 0) {
        return 0.0;
    }

    double proc_ms =
        static_cast<double>(
            dp) /
        10000.0;

    return
        100.0 *
        proc_ms /
        (wall_ms *
         logical_processors);
}

struct SystemCpuWatch {
    bool initialized = false;
    uint64_t idle = 0;
    uint64_t kernel = 0;
    uint64_t user = 0;
};

static double sample_system_busy(
    SystemCpuWatch& s
) {
    FILETIME idle{}, kernel{}, user{};

    if (!GetSystemTimes(
            &idle,
            &kernel,
            &user)) {
        return -1.0;
    }

    uint64_t ni =
        filetime64(idle);

    uint64_t nk =
        filetime64(kernel);

    uint64_t nu =
        filetime64(user);

    if (!s.initialized) {
        s.initialized = true;
        s.idle = ni;
        s.kernel = nk;
        s.user = nu;
        return 0.0;
    }

    uint64_t di =
        ni - s.idle;

    uint64_t dk =
        nk - s.kernel;

    uint64_t du =
        nu - s.user;

    s.idle = ni;
    s.kernel = nk;
    s.user = nu;

    uint64_t total =
        dk + du;

    if (total == 0 ||
        di > total) {
        return 0.0;
    }

    return
        100.0 *
        static_cast<double>(
            total - di) /
        static_cast<double>(
            total);
}

static void write_guard_json(
    const char* status,
    const char* reason,
    double start_available_mb,
    double min_available_mb,
    DWORD peak_process_mb,
    double peak_child_cpu_pct,
    double peak_system_busy_pct,
    DWORD child_exit
) {
    std::ofstream out(
        "stateram_phase14_guard_result.json",
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
        << "  \"peak_child_cpu_pct_total_machine\": "
        << peak_child_cpu_pct << ",\n"
        << "  \"peak_system_busy_pct\": "
        << peak_system_busy_pct << ",\n"
        << "  \"child_exit_code\": "
        << child_exit << "\n"
        << "}\n";
}

static int launcher_main() {
    std::cout
        << "======================================================\n"
        << " StateRAM Phase 14 - foreground-aware CPU governor\n"
        << "======================================================\n\n"
        << "Phase 13 showed the whole-process 35% hard CPU quota could\n"
        << "freeze the tiny foreground core-resume path for ~250 ms.\n\n"
        << "Phase 14 removes ONLY that whole-process hard CPU quota.\n"
        << "Memory containment remains hard. Background StateRAM work is\n"
        << "explicitly background-mode and cooperatively yields.\n"
        << "The launcher watches for abnormal sustained CPU use and exits\n"
        << "the isolated child if the new governor misbehaves.\n\n";

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
        write_guard_json(
            "ABORTED_BEFORE_START",
            "insufficient_memory_headroom",
            start_available_mb,
            start_available_mb,
            0,
            0.0,
            0.0,
            0);

        std::cout
            << "\nSAFETY STOP: insufficient memory headroom.\n";

        return 4;
    }

    HANDLE job =
        CreateJobObjectW(
            nullptr,
            nullptr);

    if (!job) return 5;

    /*
      IMPORTANT: Phase 14 intentionally has NO JobObject CPU hard cap.
      The job still provides hard memory containment, one-child containment,
      and kill-on-close.
    */
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
            CHILD_COMMIT_CAP);

    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))) {
        CloseHandle(job);
        return 6;
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

    /*
      Normal priority for the tiny synchronous resume path.
      Background baseline/deep threads lower themselves inside the SDK.
    */
    if (!CreateProcessW(
            nullptr,
            mutable_cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED |
            NORMAL_PRIORITY_CLASS,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        CloseHandle(job);
        return 7;
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

        return 8;
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

        return 9;
    }

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);

    DWORD logical =
        std::max<DWORD>(
            1,
            sys.dwNumberOfProcessors);

    std::cout
        << "\nSafety envelope:\n"
        << "  hard child commit cap: 320 MiB\n"
        << "  one isolated child only\n"
        << "  kill child if launcher exits\n"
        << "  NO whole-process CPU hard quota\n"
        << "  background workers: Windows background mode + cooperative yield\n"
        << "  sustained-CPU watchdog: abnormal >70% total-machine child CPU\n"
        << "  memory abort: >=88% load or <640 MiB available\n"
        << "  120 second timeout\n\n";

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

    DWORD last_memory_check =
        started;

    CpuWatch child_cpu{};
    SystemCpuWatch system_cpu{};

    double peak_child_cpu = 0.0;
    double peak_system_busy = 0.0;

    int child_cpu_over70_streak = 0;
    int overloaded_system_streak = 0;

    for (;;) {
        DWORD w =
            WaitForSingleObject(
                pi.hProcess,
                250);

        if (w ==
            WAIT_OBJECT_0) {
            break;
        }

        if (w != WAIT_TIMEOUT) {
            safety_abort = true;
            abort_reason =
                "worker_wait_failure";
            break;
        }

        double child_pct =
            sample_child_cpu_pct(
                pi.hProcess,
                child_cpu,
                logical);

        double system_pct =
            sample_system_busy(
                system_cpu);

        if (child_pct >= 0.0) {
            peak_child_cpu =
                std::max(
                    peak_child_cpu,
                    child_pct);
        }

        if (system_pct >= 0.0) {
            peak_system_busy =
                std::max(
                    peak_system_busy,
                    system_pct);
        }

        /*
          These are FAIL-SAFE limits, not a normal throttle.
          Brief foreground bursts cannot trip them because they require
          sustained violation over multiple 250 ms samples.
        */
        if (child_pct > 70.0) {
            ++child_cpu_over70_streak;
        } else {
            child_cpu_over70_streak = 0;
        }

        if (system_pct > 95.0 &&
            child_pct > 25.0) {
            ++overloaded_system_streak;
        } else {
            overloaded_system_streak = 0;
        }

        if (child_cpu_over70_streak >= 8) {
            safety_abort = true;
            abort_reason =
                "sustained_child_cpu_limit";
            break;
        }

        if (overloaded_system_streak >= 8) {
            safety_abort = true;
            abort_reason =
                "sustained_system_cpu_pressure";
            break;
        }

        MEMORYSTATUSEX ms{};

        if (!read_memory_status(ms)) {
            safety_abort = true;
            abort_reason =
                "memory_status_failure";
            break;
        }

        double available_mb =
            static_cast<double>(
                ms.ullAvailPhys) /
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

        if (GetTickCount() -
                started >=
            TIMEOUT_MS) {
            safety_abort = true;
            abort_reason =
                "timeout";
            break;
        }
    }

    if (safety_abort) {
        std::cout
            << "\nSAFETY WATCHDOG: "
            << abort_reason
            << "\nStopping only the isolated StateRAM child.\n";

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
                : "worker_gate_failed"),
        start_available_mb,
        min_available_mb,
        peak_mb,
        peak_child_cpu,
        peak_system_busy,
        child_exit);

    std::cout
        << "\n--- Phase 14 safety summary ---\n"
        << "Minimum available RAM: "
        << min_available_mb
        << " MiB\n"
        << "Peak child commit: "
        << peak_mb
        << " MiB\n"
        << "Peak sampled child CPU: "
        << peak_child_cpu
        << "% of total machine\n"
        << "Peak sampled system busy: "
        << peak_system_busy
        << "%\n"
        << "Worker exit code: "
        << child_exit
        << "\n";

    if (safety_abort) {
        std::cout
            << "STATE: SAFE_ABORT\n";
    } else if (child_exit == 0) {
        std::cout
            << "STATE: COMPLETED_PASS\n"
            << "Send ChatGPT these THREE files:\n"
            << "  stateram_phase14_result.json\n"
            << "  stateram_phase14_switches.csv\n"
            << "  stateram_phase14_guard_result.json\n";
    } else {
        std::cout
            << "STATE: COMPLETED_WITH_FAILED_GATE\n";
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
                    24,
                    2,
                    0,
                    true
                };

                return
                    run_experiment(
                        cfg);
            }

            if (mode ==
                "--worker") {
                Config cfg{
                    LOCAL_WORKSPACES,
                    LOCAL_WORKSPACE_BYTES,
                    LOCAL_CORE_BYTES,
                    LOCAL_SWITCHES,
                    1,
                    3,
                    false
                };

                return
                    run_experiment(
                        cfg);
            }
        }

        return launcher_main();

    } catch (
        const std::exception& e
    ) {
        std::cerr
            << "Phase 14 error: "
            << e.what()
            << "\n";

        return 99;
    }
}
