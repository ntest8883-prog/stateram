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
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "stateram_sdk.h"

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Advapi32.lib")

static constexpr size_t PAGE_BYTES = 4096;
static constexpr uint64_t MB = 1024ull * 1024ull;

struct Config {
    size_t workspaces;
    size_t workspace_bytes;
    size_t core_bytes;
    size_t switches;
    uint32_t baseline_units_per_slice;
    uint32_t baseline_sleep_ms;
    bool cloud_smoke;
};

static constexpr uint64_t LOCAL_CHILD_COMMIT_CAP = 320ull * MB;
static constexpr DWORD LOCAL_CPU_RATE = 3500; // 35% total CPU.
static constexpr uint64_t START_MIN_AVAILABLE = 1024ull * MB;
static constexpr uint64_t ABORT_MIN_AVAILABLE = 640ull * MB;
static constexpr DWORD START_MAX_MEMORY_LOAD = 80;
static constexpr DWORD ABORT_MEMORY_LOAD = 88;
static constexpr DWORD LOCAL_TIMEOUT_MS = 120000;

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

static void fill_page(
    uint8_t* page,
    uint64_t workspace,
    uint64_t page_index
) {
    uint64_t seed =
        workspace * 0xD1B54A32D192ED03ull +
        page_index;

    /*
      Fake application state deliberately has several representations:
      1/4 pseudo-random-ish pages, 3/4 structured/text/cache-like pages.
    */
    if ((page_index & 3ull) == 0) {
        auto* words =
            reinterpret_cast<uint64_t*>(page);

        for (size_t i = 0;
             i < PAGE_BYTES / 8;
             ++i) {
            words[i] =
                splitmix64(
                    seed * 0x9E3779B97F4A7C15ull + i);
        }
        return;
    }

    uint8_t base =
        static_cast<uint8_t>(
            (seed * 37 + 19) & 0xffu);

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
    if (v.empty()) {
        return 0.0;
    }

    std::sort(v.begin(), v.end());

    size_t idx =
        static_cast<size_t>(
            p *
            static_cast<double>(
                v.size() - 1));

    return v[idx];
}

struct ProcMem {
    double private_mb = 0.0;
    double ws_mb = 0.0;
};

static ProcMem proc_mem(
    HANDLE process =
        GetCurrentProcess()
) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    ProcMem out{};

    if (GetProcessMemoryInfo(
            process,
            reinterpret_cast<
                PROCESS_MEMORY_COUNTERS*>(
                    &pmc),
            sizeof(pmc))) {
        out.private_mb =
            static_cast<double>(
                pmc.PrivateUsage) /
            (1024.0 * 1024.0);

        out.ws_mb =
            static_cast<double>(
                pmc.WorkingSetSize) /
            (1024.0 * 1024.0);
    }

    return out;
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

    if (!ok) {
        return true;
    }

    return elevation.TokenIsElevated != 0;
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
    FInt baseline_done = nullptr;
    FWait wait_baseline = nullptr;
    FInt enter_dormant = nullptr;
    FInt resume_core = nullptr;
    FInt start_deep = nullptr;
    FInt deep_done = nullptr;
    FWait wait_deep = nullptr;
    FInt rearm = nullptr;
    FInt release_capsule = nullptr;
    FMetrics metrics = nullptr;
    FDestroy destroy = nullptr;

    explicit SDK() {
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

        baseline_done =
            load_fn<FInt>(
                dll,
                "sr_baseline_done");

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

        release_capsule =
            load_fn<FInt>(
                dll,
                "sr_release_capsule");

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
                "expected Phase 11 SDK ABI");
        }
    }

    ~SDK() {
        if (dll) {
            FreeLibrary(dll);
        }
    }
};

struct Workspace {
    SRHandle handle = nullptr;
    uint8_t* data = nullptr;
    uint64_t expected_hash = 0;
    bool dormant = false;
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

    /*
      Simulate edits, scrolling/history updates, and cache metadata changes.
      Only a small set of pages is touched per foreground dwell.
    */
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
            page *
                PAGE_BYTES;

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

static double one_interaction(
    Workspace& w,
    size_t bytes,
    size_t workspace_id,
    uint64_t step
) {
    double t0 =
        now_ms();

    /*
      A small mixed interactive action:
      - visible UI/core metadata
      - document-like region
      - code-buffer-like region
      - history/cache-like region
    */
    volatile uint64_t sum = 0;

    const size_t core_span =
        std::min<size_t>(
            2 * MB,
            bytes);

    for (size_t off = 0;
         off < core_span;
         off += 32 * 1024) {
        sum +=
            w.data[
                (off +
                 (step * 17)) %
                core_span];
    }

    const std::array<size_t, 3>
        anchors = {
            bytes / 4,
            bytes / 2,
            (bytes * 3) / 4
        };

    for (size_t a : anchors) {
        size_t base =
            std::min(
                a +
                ((workspace_id * 8192 +
                  step * 4096) %
                 (2 * MB)),
                bytes - 64 * 1024);

        for (size_t off = 0;
             off < 64 * 1024;
             off += 4096) {
            sum +=
                w.data[
                    base + off];
        }
    }

    if (sum ==
        0xFFFFFFFFFFFFFFFFull) {
        std::cout << "";
    }

    return now_ms() - t0;
}

static int run_workspace_experiment(
    const Config& cfg
) {
    SDK sdk;

    const ProcMem before_create =
        proc_mem();

    std::vector<Workspace>
        ws(cfg.workspaces);

    auto cleanup = [&]() {
        for (auto& w : ws) {
            if (w.handle) {
                sdk.destroy(
                    w.handle);

                w.handle = nullptr;
                w.data = nullptr;
            }
        }
    };

    try {
        for (size_t i = 0;
             i < cfg.workspaces;
             ++i) {
            ws[i].handle =
                sdk.create(
                    cfg.workspace_bytes,
                    cfg.core_bytes);

            if (!ws[i].handle) {
                cleanup();
                return 60;
            }

            ws[i].data =
                static_cast<uint8_t*>(
                    sdk.data(
                        ws[i].handle));

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

        const ProcMem all_raw =
            proc_mem();

        /*
          Prepare each capsule in the background, one workspace at a time.
          Then move every workspace to compact dormant form.
        */
        for (size_t i = 0;
             i < cfg.workspaces;
             ++i) {
            if (!sdk.begin_baseline(
                    ws[i].handle,
                    cfg.baseline_units_per_slice,
                    cfg.baseline_sleep_ms)) {
                cleanup();
                return 62;
            }

            if (!sdk.wait_baseline(
                    ws[i].handle,
                    60000)) {
                cleanup();
                return 63;
            }

            if (!sdk.enter_dormant(
                    ws[i].handle)) {
                cleanup();
                return 64;
            }

            ws[i].dormant = true;
        }

        const ProcMem all_dormant =
            proc_mem();

        /*
          Bring workspace 0 foreground. Keep its compact capsule instead of
          discarding it; Phase 11's rearm operation lets it return to dormant
          cheaply after a short foreground dwell.
        */
        if (!sdk.resume_core(
                ws[0].handle) ||
            !sdk.start_deep(
                ws[0].handle) ||
            !sdk.wait_deep(
                ws[0].handle,
                30000)) {
            cleanup();
            return 65;
        }

        if (fnv1a64(
                ws[0].data,
                cfg.workspace_bytes) !=
            ws[0].expected_hash) {
            cleanup();
            return 66;
        }

        if (!sdk.rearm(
                ws[0].handle)) {
            cleanup();
            return 67;
        }

        ws[0].dormant = false;

        size_t current = 0;

        mutate_workspace(
            ws[current],
            cfg.workspace_bytes,
            current);

        std::vector<double>
            switch_to_core_ms;

        std::vector<double>
            outgoing_dormancy_ms;

        std::vector<double>
            deep_settle_ms;

        std::vector<double>
            core_ui_during_restore_ms;

        std::vector<double>
            active_interaction_ms;

        uint64_t integrity_checks = 1;
        uint64_t integrity_failures = 0;
        uint64_t total_core_ui_passes = 0;

        const std::array<size_t, 24>
            pattern = {
                1,2,3,4,5,0,
                3,1,4,2,5,0,
                2,4,1,5,3,0,
                4,2,0,5,1,3
            };

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

            double switch_t0 =
                now_ms();

            double d0 =
                now_ms();

            if (!sdk.enter_dormant(
                    ws[current].handle)) {
                cleanup();
                return 68;
            }

            outgoing_dormancy_ms.push_back(
                now_ms() - d0);

            ws[current].dormant = true;

            if (!sdk.resume_core(
                    ws[target].handle)) {
                cleanup();
                return 69;
            }

            switch_to_core_ms.push_back(
                now_ms() -
                switch_t0);

            ws[target].dormant = false;

            if (!sdk.start_deep(
                    ws[target].handle)) {
                cleanup();
                return 70;
            }

            double deep_t0 =
                now_ms();

            while (sdk.deep_done(
                       ws[target].handle) == 0) {
                double u0 =
                    now_ms();

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

                core_ui_during_restore_ms.push_back(
                    now_ms() - u0);

                ++total_core_ui_passes;

                Sleep(0);
            }

            if (!sdk.wait_deep(
                    ws[target].handle,
                    30000)) {
                cleanup();
                return 71;
            }

            deep_settle_ms.push_back(
                now_ms() - deep_t0);

            ++integrity_checks;

            if (fnv1a64(
                    ws[target].data,
                    cfg.workspace_bytes) !=
                ws[target].expected_hash) {
                ++integrity_failures;
            }

            if (!sdk.rearm(
                    ws[target].handle)) {
                cleanup();
                return 72;
            }

            /*
              Full state is now active. Simulate several ordinary actions.
            */
            for (size_t i = 0;
                 i < 30;
                 ++i) {
                active_interaction_ms.push_back(
                    one_interaction(
                        ws[target],
                        cfg.workspace_bytes,
                        target,
                        s * 30 + i));
            }

            mutate_workspace(
                ws[target],
                cfg.workspace_bytes,
                target);

            current =
                target;
        }

        const ProcMem steady_after_switches =
            proc_mem();

        /*
          One final suspend of the foreground workspace gives us an
          all-dormant aggregate again after repeated delta layering.
        */
        if (!sdk.enter_dormant(
                ws[current].handle)) {
            cleanup();
            return 73;
        }

        ws[current].dormant = true;

        const ProcMem final_all_dormant =
            proc_mem();

        /*
          Verify every workspace one final time before teardown.
        */
        for (size_t i = 0;
             i < cfg.workspaces;
             ++i) {
            if (!sdk.resume_core(
                    ws[i].handle) ||
                !sdk.start_deep(
                    ws[i].handle) ||
                !sdk.wait_deep(
                    ws[i].handle,
                    30000)) {
                cleanup();
                return 74;
            }

            ++integrity_checks;

            if (fnv1a64(
                    ws[i].data,
                    cfg.workspace_bytes) !=
                ws[i].expected_hash) {
                ++integrity_failures;
            }

            /*
              No need to keep these states active after verification.
              Destroying the handle later releases raw + capsule together.
            */
        }

        uint64_t total_rearms = 0;
        uint64_t total_dirty_pages = 0;

        for (size_t i = 0;
             i < cfg.workspaces;
             ++i) {
            SRMetrics m{};

            if (!sdk.metrics(
                    ws[i].handle,
                    &m)) {
                cleanup();
                return 75;
            }

            total_rearms +=
                m.capsule_rearms;

            total_dirty_pages +=
                m.dirty_pages;
        }

        cleanup();

        Sleep(100);

        const ProcMem after_destroy =
            proc_mem();

        const double raw_payload_mb =
            static_cast<double>(
                cfg.workspaces *
                cfg.workspace_bytes) /
            (1024.0 * 1024.0);

        const double compact_saved_mb =
            all_raw.private_mb -
            all_dormant.private_mb;

        const double final_compact_saved_mb =
            all_raw.private_mb -
            final_all_dormant.private_mb;

        const double switch_p50 =
            percentile(
                switch_to_core_ms,
                0.50);

        const double switch_p95 =
            percentile(
                switch_to_core_ms,
                0.95);

        const double switch_p99 =
            percentile(
                switch_to_core_ms,
                0.99);

        const double switch_max =
            switch_to_core_ms.empty()
                ? 0.0
                : *std::max_element(
                    switch_to_core_ms.begin(),
                    switch_to_core_ms.end());

        const double settle_p95 =
            percentile(
                deep_settle_ms,
                0.95);

        const double ui_p95 =
            percentile(
                core_ui_during_restore_ms,
                0.95);

        const double ui_p99 =
            percentile(
                core_ui_during_restore_ms,
                0.99);

        const double interaction_p95 =
            percentile(
                active_interaction_ms,
                0.95);

        const double interaction_p99 =
            percentile(
                active_interaction_ms,
                0.99);

        bool integrity_ok =
            integrity_failures == 0 &&
            integrity_checks >=
                cfg.switches +
                cfg.workspaces;

        bool memory_ok =
            compact_saved_mb >
                raw_payload_mb * 0.30 &&
            final_compact_saved_mb >
                raw_payload_mb * 0.25 &&
            after_destroy.private_mb <=
                before_create.private_mb + 12.0;

        /*
          Local gate is intentionally about visible switching, not deep settle.
          Deep restoration is allowed to continue after core/UI becomes usable.
        */
        bool switching_ok =
            switch_to_core_ms.size() ==
                cfg.switches &&
            switch_p95 < 35.0 &&
            switch_p99 < 60.0 &&
            switch_max < 120.0;

        bool foreground_ok =
            !core_ui_during_restore_ms.empty() &&
            ui_p95 < 10.0 &&
            ui_p99 < 25.0 &&
            interaction_p95 < 10.0 &&
            interaction_p99 < 25.0;

        bool lifecycle_ok =
            total_rearms >=
                cfg.switches + 1;

        bool pass =
            integrity_ok &&
            memory_ok &&
            switching_ok &&
            foreground_ok &&
            lifecycle_ok;

        const char* json_name =
            cfg.cloud_smoke
                ? "phase11_cloud_smoke.json"
                : "stateram_phase11_local_result.json";

        std::ofstream out(
            json_name,
            std::ios::binary);

        out << "{\n";
        out << "  \"phase\": \"Windows Phase 11 realistic isolated workspace switching\",\n";
        out << "  \"mode\": \""
            << (cfg.cloud_smoke ? "cloud_smoke" : "local")
            << "\",\n";
        out << "  \"sdk_api_version\": "
            << sdk.api() << ",\n";
        out << "  \"workspaces\": "
            << cfg.workspaces << ",\n";
        out << "  \"workspace_mb_each\": "
            << (cfg.workspace_bytes / MB) << ",\n";
        out << "  \"raw_payload_mb\": "
            << raw_payload_mb << ",\n";
        out << "  \"switches\": "
            << cfg.switches << ",\n";
        out << "  \"process_private_before_create_mb\": "
            << std::fixed << std::setprecision(3)
            << before_create.private_mb << ",\n";
        out << "  \"process_private_all_raw_mb\": "
            << all_raw.private_mb << ",\n";
        out << "  \"process_private_all_dormant_mb\": "
            << all_dormant.private_mb << ",\n";
        out << "  \"compact_saved_mb\": "
            << compact_saved_mb << ",\n";
        out << "  \"process_private_steady_after_switches_mb\": "
            << steady_after_switches.private_mb << ",\n";
        out << "  \"process_private_final_all_dormant_mb\": "
            << final_all_dormant.private_mb << ",\n";
        out << "  \"final_compact_saved_mb\": "
            << final_compact_saved_mb << ",\n";
        out << "  \"process_private_after_destroy_mb\": "
            << after_destroy.private_mb << ",\n";
        out << "  \"switch_to_core_p50_ms\": "
            << switch_p50 << ",\n";
        out << "  \"switch_to_core_p95_ms\": "
            << switch_p95 << ",\n";
        out << "  \"switch_to_core_p99_ms\": "
            << switch_p99 << ",\n";
        out << "  \"switch_to_core_max_ms\": "
            << switch_max << ",\n";
        out << "  \"deep_settle_p95_ms\": "
            << settle_p95 << ",\n";
        out << "  \"core_ui_during_restore_p95_ms\": "
            << ui_p95 << ",\n";
        out << "  \"core_ui_during_restore_p99_ms\": "
            << ui_p99 << ",\n";
        out << "  \"active_interaction_p95_ms\": "
            << interaction_p95 << ",\n";
        out << "  \"active_interaction_p99_ms\": "
            << interaction_p99 << ",\n";
        out << "  \"core_ui_passes_during_restore\": "
            << total_core_ui_passes << ",\n";
        out << "  \"capsule_rearms\": "
            << total_rearms << ",\n";
        out << "  \"last_reported_dirty_pages_sum\": "
            << total_dirty_pages << ",\n";
        out << "  \"integrity_checks\": "
            << integrity_checks << ",\n";
        out << "  \"integrity_failures\": "
            << integrity_failures << ",\n";
        out << "  \"integrity_ok\": "
            << (integrity_ok ? "true" : "false") << ",\n";
        out << "  \"memory_ok\": "
            << (memory_ok ? "true" : "false") << ",\n";
        out << "  \"switching_ok\": "
            << (switching_ok ? "true" : "false") << ",\n";
        out << "  \"foreground_ok\": "
            << (foreground_ok ? "true" : "false") << ",\n";
        out << "  \"lifecycle_ok\": "
            << (lifecycle_ok ? "true" : "false") << ",\n";
        out << "  \"pass\": "
            << (pass ? "true" : "false") << "\n";
        out << "}\n";

        out.close();

        std::cout
            << "StateRAM Windows Phase 11 - realistic isolated workspace switching\n"
            << " workspaces="
            << cfg.workspaces
            << " x "
            << (cfg.workspace_bytes / MB)
            << " MiB, switches="
            << cfg.switches
            << "\n"
            << " all raw private="
            << all_raw.private_mb
            << " MiB, all dormant="
            << all_dormant.private_mb
            << " MiB, saved="
            << compact_saved_mb
            << " MiB\n"
            << " switch-to-core p50="
            << switch_p50
            << " ms p95="
            << switch_p95
            << " ms p99="
            << switch_p99
            << " ms max="
            << switch_max
            << " ms\n"
            << " deep-settle p95="
            << settle_p95
            << " ms\n"
            << " core-UI while restoring p95="
            << ui_p95
            << " ms p99="
            << ui_p99
            << " ms\n"
            << " active interaction p95="
            << interaction_p95
            << " ms p99="
            << interaction_p99
            << " ms\n"
            << " capsule rearms="
            << total_rearms
            << " integrity failures="
            << integrity_failures
            << "\n"
            << " after destroy private="
            << after_destroy.private_mb
            << " MiB\n"
            << " WINDOWS_STATERAM_PHASE11="
            << (pass ? "PASS" : "FAIL")
            << "\n";

        return pass ? 0 : 80;

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
        "stateram_phase11_guard_result.json",
        std::ios::binary);

    out << "{\n";
    out << "  \"status\": \""
        << status << "\",\n";
    out << "  \"reason\": \""
        << reason << "\",\n";
    out << "  \"start_available_mb\": "
        << std::fixed
        << std::setprecision(1)
        << start_available_mb << ",\n";
    out << "  \"minimum_available_mb\": "
        << min_available_mb << ",\n";
    out << "  \"job_peak_process_memory_mb\": "
        << peak_process_mb << ",\n";
    out << "  \"child_exit_code\": "
        << child_exit << "\n";
    out << "}\n";
}

static int local_launcher() {
    std::cout
        << "==========================================================\n"
        << " StateRAM Phase 11 - realistic isolated workspace test\n"
        << "==========================================================\n\n"
        << "This still touches ONLY StateRAM's own child process.\n"
        << "It simulates six document/code/tab workspaces and repeated\n"
        << "switching between them. No real application is modified.\n\n";

    if (current_process_is_elevated()) {
        std::cout
            << "SAFETY STOP: Do not run this as Administrator.\n";
        return 2;
    }

    MEMORYSTATUSEX start{};

    if (!read_memory_status(start)) {
        std::cout
            << "SAFETY STOP: Cannot read Windows memory status.\n";
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
            << "\nSAFETY STOP: Not enough current memory headroom.\n"
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

    std::wstring exe =
        exe_path();

    std::wstring cmd =
        L"\"" + exe +
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
        << "\nContainment installed BEFORE the test starts:\n"
        << "  child hard commit cap: 320 MiB\n"
        << "  child hard CPU cap: 35% total CPU\n"
        << "  BELOW_NORMAL process priority\n"
        << "  watchdog every 250 ms\n"
        << "  abort at >=88% memory load or <640 MiB available\n"
        << "  120 second hard timeout\n\n";

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
        std::cout
            << "\nSAFETY WATCHDOG TRIGGERED: "
            << abort_reason
            << "\nStopping only the isolated StateRAM worker...\n";

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
        end_info{};

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
        child_exit);

    std::cout
        << "\n--- Phase 11 safety summary ---\n"
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
            << "Send stateram_phase11_local_result.json and\n"
            << "stateram_phase11_guard_result.json to ChatGPT.\n";
    } else {
        std::cout
            << "STATE: COMPLETED_WITH_FAILED_GATE\n"
            << "The safety harness still contained the experiment.\n";
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
                    run_workspace_experiment(
                        cfg);
            }

            if (mode ==
                "--worker") {
                Config cfg{
                    6,
                    24ull * MB,
                    2ull * MB,
                    18,
                    1,
                    3,
                    false
                };

                return
                    run_workspace_experiment(
                        cfg);
            }
        }

        return local_launcher();

    } catch (
        const std::exception& e
    ) {
        std::cerr
            << "Phase 11 error: "
            << e.what()
            << "\n";

        return 99;
    }
}
