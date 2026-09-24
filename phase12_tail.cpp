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
static constexpr size_t LOCAL_SWITCHES = 18;

static constexpr uint64_t LOCAL_CHILD_COMMIT_CAP = 320ull * MB;
static constexpr DWORD LOCAL_CPU_RATE = 3500; // 35% total CPU: same Phase 11 safety envelope.
static constexpr uint64_t START_MIN_AVAILABLE = 1024ull * MB;
static constexpr uint64_t ABORT_MIN_AVAILABLE = 640ull * MB;
static constexpr DWORD START_MAX_MEMORY_LOAD = 80;
static constexpr DWORD ABORT_MEMORY_LOAD = 88;
static constexpr DWORD LOCAL_TIMEOUT_MS = 120000;

struct Config {
    size_t workspaces;
    size_t workspace_bytes;
    size_t core_bytes;
    size_t switches;
    uint32_t baseline_units_per_slice;
    uint32_t baseline_sleep_ms;
    bool cloud;
};

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

static double thread_cpu_ms() {
    FILETIME create{}, exit{}, kernel{}, user{};

    if (!GetThreadTimes(
            GetCurrentThread(),
            &create,
            &exit,
            &kernel,
            &user)) {
        return 0.0;
    }

    ULARGE_INTEGER k{};
    ULARGE_INTEGER u{};

    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;

    return static_cast<double>(
        k.QuadPart + u.QuadPart) /
        10000.0;
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
                    seed * 0x9E3779B97F4A7C15ull +
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
                seed * 0xA0761D6478BD642Full +
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

        if ((api() >> 16) != 11) {
            throw std::runtime_error(
                "expected the validated Phase 11 SDK");
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

struct SwitchSample {
    size_t index = 0;
    size_t from = 0;
    size_t to = 0;

    DWORD memory_load_pct = 0;
    double available_mb = 0.0;

    double total_wall_ms = 0.0;
    double total_thread_cpu_ms = 0.0;

    double dormant_wall_ms = 0.0;
    double dormant_thread_cpu_ms = 0.0;
    double sdk_finalization_ms = 0.0;
    double sdk_decommit_ms = 0.0;
    uint64_t outgoing_dirty_pages = 0;
    double outgoing_packed_mb = 0.0;

    double bridge_wall_ms = 0.0;

    double resume_wall_ms = 0.0;
    double resume_thread_cpu_ms = 0.0;
    double sdk_core_restore_ms = 0.0;
    double incoming_packed_mb = 0.0;

    double total_nonrunning_ms = 0.0;
    double dormant_nonrunning_ms = 0.0;
    double resume_nonrunning_ms = 0.0;

    double deep_restore_wall_ms = 0.0;
};

static std::string classify_tail(
    const SwitchSample& s
) {
    const double internal_dormant =
        s.sdk_finalization_ms +
        s.sdk_decommit_ms;

    const double unattributed_dormant =
        std::max(
            0.0,
            s.dormant_wall_ms -
            internal_dormant);

    const double unattributed_resume =
        std::max(
            0.0,
            s.resume_wall_ms -
            s.sdk_core_restore_ms);

    double best =
        s.sdk_finalization_ms;

    std::string label =
        "OUTGOING_FINALIZATION";

    if (s.sdk_decommit_ms > best) {
        best =
            s.sdk_decommit_ms;

        label =
            "OUTGOING_DECOMMIT";
    }

    if (s.sdk_core_restore_ms > best) {
        best =
            s.sdk_core_restore_ms;

        label =
            "INCOMING_CORE_RESTORE";
    }

    if (s.total_nonrunning_ms > best) {
        best =
            s.total_nonrunning_ms;

        label =
            "THREAD_NOT_RUNNING_OR_BLOCKED";
    }

    if (unattributed_dormant > best) {
        best =
            unattributed_dormant;

        label =
            "DORMANT_CALL_UNATTRIBUTED";
    }

    if (unattributed_resume > best) {
        best =
            unattributed_resume;

        label =
            "RESUME_CALL_UNATTRIBUTED";
    }

    return label;
}

static int run_diagnostic(
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

        /*
          Same preparation policy as Phase 11:
          build one compact capsule at a time, then make every workspace dormant.
        */
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

        std::vector<SwitchSample>
            samples;

        samples.reserve(
            cfg.switches);

        const std::array<size_t, 24>
            pattern = {
                1,2,3,4,5,0,
                3,1,4,2,5,0,
                2,4,1,5,3,0,
                4,2,0,5,1,3
            };

        const char* csv_name =
            cfg.cloud
                ? "phase12_cloud_switches.csv"
                : "stateram_phase12_switches.csv";

        std::ofstream csv(
            csv_name,
            std::ios::binary);

        csv
            << "index,from,to,memory_load_pct,available_mb,"
            << "total_wall_ms,total_thread_cpu_ms,total_nonrunning_ms,"
            << "dormant_wall_ms,dormant_thread_cpu_ms,dormant_nonrunning_ms,"
            << "sdk_finalization_ms,sdk_decommit_ms,outgoing_dirty_pages,outgoing_packed_mb,"
            << "bridge_wall_ms,"
            << "resume_wall_ms,resume_thread_cpu_ms,resume_nonrunning_ms,"
            << "sdk_core_restore_ms,incoming_packed_mb,deep_restore_wall_ms\n";

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

            SwitchSample sample{};
            sample.index = s;
            sample.from = current;
            sample.to = target;
            sample.memory_load_pct =
                mem.dwMemoryLoad;
            sample.available_mb =
                static_cast<double>(
                    mem.ullAvailPhys) /
                (1024.0 * 1024.0);

            double total_wall_0 =
                now_ms();

            double total_cpu_0 =
                thread_cpu_ms();

            double dormant_wall_0 =
                now_ms();

            double dormant_cpu_0 =
                thread_cpu_ms();

            if (!sdk.enter_dormant(
                    ws[current].h)) {
                cleanup();
                return 65;
            }

            sample.dormant_thread_cpu_ms =
                thread_cpu_ms() -
                dormant_cpu_0;

            sample.dormant_wall_ms =
                now_ms() -
                dormant_wall_0;

            SRMetrics out_m{};

            if (!sdk.metrics(
                    ws[current].h,
                    &out_m)) {
                cleanup();
                return 66;
            }

            sample.sdk_finalization_ms =
                out_m.finalization_ms;

            sample.sdk_decommit_ms =
                out_m.decommit_ms;

            sample.outgoing_dirty_pages =
                out_m.dirty_pages;

            sample.outgoing_packed_mb =
                static_cast<double>(
                    out_m.packed_committed_bytes) /
                (1024.0 * 1024.0);

            double bridge_0 =
                now_ms();

            double resume_wall_0 =
                now_ms();

            double resume_cpu_0 =
                thread_cpu_ms();

            if (!sdk.resume_core(
                    ws[target].h)) {
                cleanup();
                return 67;
            }

            sample.resume_thread_cpu_ms =
                thread_cpu_ms() -
                resume_cpu_0;

            sample.resume_wall_ms =
                now_ms() -
                resume_wall_0;

            sample.bridge_wall_ms =
                std::max(
                    0.0,
                    resume_wall_0 -
                    bridge_0);

            SRMetrics in_m{};

            if (!sdk.metrics(
                    ws[target].h,
                    &in_m)) {
                cleanup();
                return 68;
            }

            sample.sdk_core_restore_ms =
                in_m.core_restore_ms;

            sample.incoming_packed_mb =
                static_cast<double>(
                    in_m.packed_committed_bytes) /
                (1024.0 * 1024.0);

            sample.total_thread_cpu_ms =
                thread_cpu_ms() -
                total_cpu_0;

            sample.total_wall_ms =
                now_ms() -
                total_wall_0;

            sample.dormant_nonrunning_ms =
                std::max(
                    0.0,
                    sample.dormant_wall_ms -
                    sample.dormant_thread_cpu_ms);

            sample.resume_nonrunning_ms =
                std::max(
                    0.0,
                    sample.resume_wall_ms -
                    sample.resume_thread_cpu_ms);

            sample.total_nonrunning_ms =
                std::max(
                    0.0,
                    sample.total_wall_ms -
                    sample.total_thread_cpu_ms);

            if (!sdk.start_deep(
                    ws[target].h)) {
                cleanup();
                return 69;
            }

            double deep_0 =
                now_ms();

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

            sample.deep_restore_wall_ms =
                now_ms() - deep_0;

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

            samples.push_back(
                sample);

            csv
                << sample.index << ","
                << sample.from << ","
                << sample.to << ","
                << sample.memory_load_pct << ","
                << std::fixed
                << std::setprecision(3)
                << sample.available_mb << ","
                << sample.total_wall_ms << ","
                << sample.total_thread_cpu_ms << ","
                << sample.total_nonrunning_ms << ","
                << sample.dormant_wall_ms << ","
                << sample.dormant_thread_cpu_ms << ","
                << sample.dormant_nonrunning_ms << ","
                << sample.sdk_finalization_ms << ","
                << sample.sdk_decommit_ms << ","
                << sample.outgoing_dirty_pages << ","
                << sample.outgoing_packed_mb << ","
                << sample.bridge_wall_ms << ","
                << sample.resume_wall_ms << ","
                << sample.resume_thread_cpu_ms << ","
                << sample.resume_nonrunning_ms << ","
                << sample.sdk_core_restore_ms << ","
                << sample.incoming_packed_mb << ","
                << sample.deep_restore_wall_ms
                << "\n";

            csv.flush();

            current =
                target;
        }

        bool integrity_ok = true;

        /*
          Final check of currently active workspace; all incoming workspaces
          were also hash-checked after every restoration above.
        */
        if (fnv1a64(
                ws[current].data,
                cfg.workspace_bytes) !=
            ws[current].expected_hash) {
            integrity_ok = false;
        }

        std::vector<double>
            totals;

        totals.reserve(
            samples.size());

        for (const auto& s : samples) {
            totals.push_back(
                s.total_wall_ms);
        }

        auto max_it =
            std::max_element(
                samples.begin(),
                samples.end(),
                [](
                    const SwitchSample& a,
                    const SwitchSample& b
                ) {
                    return a.total_wall_ms <
                           b.total_wall_ms;
                });

        if (max_it ==
            samples.end()) {
            cleanup();
            return 73;
        }

        const SwitchSample worst =
            *max_it;

        const double p50 =
            percentile(
                totals,
                0.50);

        const double p95 =
            percentile(
                totals,
                0.95);

        const double p99 =
            percentile(
                totals,
                0.99);

        const bool tail_over_100 =
            worst.total_wall_ms >
            100.0;

        const std::string likely_component =
            classify_tail(
                worst);

        const char* json_name =
            cfg.cloud
                ? "phase12_cloud_result.json"
                : "stateram_phase12_tail_result.json";

        std::ofstream out(
            json_name,
            std::ios::binary);

        out
            << "{\n"
            << "  \"phase\": \"Windows Phase 12 tail-latency diagnosis\",\n"
            << "  \"mode\": \""
            << (cfg.cloud ? "cloud" : "local")
            << "\",\n"
            << "  \"switches\": "
            << samples.size() << ",\n"
            << "  \"integrity_ok\": "
            << (integrity_ok ? "true" : "false") << ",\n"
            << "  \"switch_p50_ms\": "
            << std::fixed
            << std::setprecision(3)
            << p50 << ",\n"
            << "  \"switch_p95_ms\": "
            << p95 << ",\n"
            << "  \"switch_p99_ms\": "
            << p99 << ",\n"
            << "  \"switch_max_ms\": "
            << worst.total_wall_ms << ",\n"
            << "  \"tail_over_100ms\": "
            << (tail_over_100 ? "true" : "false") << ",\n"
            << "  \"worst_switch_index\": "
            << worst.index << ",\n"
            << "  \"worst_from_workspace\": "
            << worst.from << ",\n"
            << "  \"worst_to_workspace\": "
            << worst.to << ",\n"
            << "  \"worst_memory_load_pct\": "
            << worst.memory_load_pct << ",\n"
            << "  \"worst_available_mb\": "
            << worst.available_mb << ",\n"
            << "  \"worst_total_thread_cpu_ms\": "
            << worst.total_thread_cpu_ms << ",\n"
            << "  \"worst_total_nonrunning_ms\": "
            << worst.total_nonrunning_ms << ",\n"
            << "  \"worst_dormant_wall_ms\": "
            << worst.dormant_wall_ms << ",\n"
            << "  \"worst_dormant_thread_cpu_ms\": "
            << worst.dormant_thread_cpu_ms << ",\n"
            << "  \"worst_dormant_nonrunning_ms\": "
            << worst.dormant_nonrunning_ms << ",\n"
            << "  \"worst_sdk_finalization_ms\": "
            << worst.sdk_finalization_ms << ",\n"
            << "  \"worst_sdk_decommit_ms\": "
            << worst.sdk_decommit_ms << ",\n"
            << "  \"worst_outgoing_dirty_pages\": "
            << worst.outgoing_dirty_pages << ",\n"
            << "  \"worst_outgoing_packed_mb\": "
            << worst.outgoing_packed_mb << ",\n"
            << "  \"worst_resume_wall_ms\": "
            << worst.resume_wall_ms << ",\n"
            << "  \"worst_resume_thread_cpu_ms\": "
            << worst.resume_thread_cpu_ms << ",\n"
            << "  \"worst_resume_nonrunning_ms\": "
            << worst.resume_nonrunning_ms << ",\n"
            << "  \"worst_sdk_core_restore_ms\": "
            << worst.sdk_core_restore_ms << ",\n"
            << "  \"worst_incoming_packed_mb\": "
            << worst.incoming_packed_mb << ",\n"
            << "  \"worst_deep_restore_wall_ms\": "
            << worst.deep_restore_wall_ms << ",\n"
            << "  \"likely_dominant_component\": \""
            << likely_component << "\",\n"
            << "  \"diagnostic_pass\": "
            << (integrity_ok &&
                samples.size() == cfg.switches
                ? "true" : "false")
            << "\n"
            << "}\n";

        out.close();
        csv.close();

        std::cout
            << "StateRAM Phase 12 - tail-latency diagnosis\n"
            << " switch p50="
            << p50
            << " ms p95="
            << p95
            << " ms p99="
            << p99
            << " ms max="
            << worst.total_wall_ms
            << " ms\n"
            << " worst switch #"
            << worst.index
            << " "
            << worst.from
            << "->"
            << worst.to
            << "\n"
            << "   total thread CPU="
            << worst.total_thread_cpu_ms
            << " ms; non-running/blocking="
            << worst.total_nonrunning_ms
            << " ms\n"
            << "   outgoing finalization="
            << worst.sdk_finalization_ms
            << " ms; decommit="
            << worst.sdk_decommit_ms
            << " ms\n"
            << "   incoming core restore="
            << worst.sdk_core_restore_ms
            << " ms\n"
            << "   likely dominant component="
            << likely_component
            << "\n"
            << "   integrity="
            << (integrity_ok ? "OK" : "FAIL")
            << "\n"
            << " WINDOWS_STATERAM_PHASE12_DIAGNOSTIC="
            << (integrity_ok &&
                samples.size() == cfg.switches
                ? "PASS" : "FAIL")
            << "\n";

        cleanup();

        return integrity_ok &&
               samples.size() ==
                   cfg.switches
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
        "stateram_phase12_guard_result.json",
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
        << " StateRAM Phase 12 - narrow tail-latency diagnosis\n"
        << "======================================================\n\n"
        << "This does NOT test more capacity.\n"
        << "It repeats the validated Phase 11 switch pattern and measures\n"
        << "where any long switch actually spends its time.\n\n";

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

    if (!job) {
        return 5;
    }

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

        CloseHandle(
            pi.hThread);
        CloseHandle(
            pi.hProcess);
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

        CloseHandle(
            pi.hThread);
        CloseHandle(
            pi.hProcess);
        CloseHandle(job);

        return 10;
    }

    std::cout
        << "\nSame Phase 11 safety envelope:\n"
        << "  child hard commit cap: 320 MiB\n"
        << "  child hard CPU cap: 35% total CPU\n"
        << "  BELOW_NORMAL priority\n"
        << "  abort at >=88% memory load or <640 MiB available\n"
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
            LOCAL_TIMEOUT_MS) {
            safety_abort = true;
            abort_reason =
                "timeout";
            break;
        }
    }

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
        << "\n--- Phase 12 safety summary ---\n"
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
            << "Send ChatGPT these THREE files:\n"
            << "  stateram_phase12_tail_result.json\n"
            << "  stateram_phase12_switches.csv\n"
            << "  stateram_phase12_guard_result.json\n";
    } else {
        std::cout
            << "STATE: COMPLETED_WITH_FAILED_DIAGNOSTIC\n";
    }

    CloseHandle(
        low_mem);
    CloseHandle(
        pi.hProcess);
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
                    8,
                    2,
                    0,
                    true
                };

                return
                    run_diagnostic(
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
                    run_diagnostic(
                        cfg);
            }
        }

        return launcher_main();

    } catch (
        const std::exception& e
    ) {
        std::cerr
            << "Phase 12 error: "
            << e.what()
            << "\n";

        return 99;
    }
}
