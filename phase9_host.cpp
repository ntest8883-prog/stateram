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
#include <stdexcept>
#include <string>
#include <vector>

#include "stateram_sdk.h"

#pragma comment(lib, "Psapi.lib")

static constexpr size_t PAGE_BYTES = 4096;

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
    x = (x ^ (x >> 30)) *
        0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) *
        0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static void fill_page(
    uint8_t* page,
    uint64_t page_index
) {
    if ((page_index & 3ull) == 0) {
        auto* words =
            reinterpret_cast<uint64_t*>(page);

        for (size_t i = 0;
             i < PAGE_BYTES / 8;
             ++i) {
            words[i] =
                splitmix64(
                    page_index *
                        0xD6E8FEB86659FD93ull +
                    i);
        }

        return;
    }

    uint8_t base =
        static_cast<uint8_t>(
            (page_index * 29 + 17) &
            0xffu);

    std::memset(
        page,
        base,
        PAGE_BYTES);

    for (size_t off = 0;
         off < PAGE_BYTES;
         off += 512) {
        uint64_t x =
            splitmix64(
                page_index *
                    0xA0761D6478BD642Full +
                off);

        std::memcpy(
            page + off,
            &x,
            sizeof(x));
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

struct ProcMem {
    double private_mb = 0.0;
    double ws_mb = 0.0;
};

static ProcMem proc_mem() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    ProcMem out{};

    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
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
                "missing export: ") +
            name);
    }

    return reinterpret_cast<T>(p);
}

static void mutate_sparse(
    uint8_t* arena,
    size_t pages,
    size_t target,
    uint64_t salt
) {
    for (size_t i = 0;
         i < target;
         ++i) {
        size_t page =
            (i * 131ull +
             17ull +
             salt * 97ull) %
            pages;

        uint8_t* p =
            arena +
            page * PAGE_BYTES;

        p[13] ^=
            static_cast<uint8_t>(
                0x5A ^
                ((i + salt) &
                 0xffu));

        p[2047] ^=
            static_cast<uint8_t>(
                0xA5 ^
                (((i * 7u) +
                  salt) &
                 0xffu));
    }
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

int main() {
    const size_t arena_bytes =
        256ull *
        1024ull *
        1024ull;

    const size_t core_bytes =
        8ull *
        1024ull *
        1024ull;

    const size_t pages =
        arena_bytes /
        PAGE_BYTES;

    const size_t dirty_target =
        512;

    HMODULE dll =
        LoadLibraryW(
            L"stateram_sdk.dll");

    if (!dll) {
        std::cerr
            << "LoadLibrary failed "
            << GetLastError()
            << "\n";

        return 2;
    }

    using FApi =
        uint32_t (*)();

    using FCreate =
        SRHandle (*)(
            uint64_t,
            uint64_t);

    using FData =
        void* (*)(
            SRHandle);

    using FInt =
        int (*)(
            SRHandle);

    using FBegin =
        int (*)(
            SRHandle,
            uint32_t,
            uint32_t);

    using FProgress =
        uint32_t (*)(
            SRHandle);

    using FWait =
        int (*)(
            SRHandle,
            uint32_t);

    using FMetrics =
        int (*)(
            SRHandle,
            SRMetrics*);

    using FDestroy =
        void (*)(
            SRHandle);

    auto api =
        load_fn<FApi>(
            dll,
            "sr_api_version");

    auto create =
        load_fn<FCreate>(
            dll,
            "sr_create");

    auto datafn =
        load_fn<FData>(
            dll,
            "sr_data");

    auto begin_baseline =
        load_fn<FBegin>(
            dll,
            "sr_begin_background_baseline");

    auto baseline_done =
        load_fn<FInt>(
            dll,
            "sr_baseline_done");

    auto baseline_progress =
        load_fn<FProgress>(
            dll,
            "sr_baseline_progress_permille");

    auto wait_baseline =
        load_fn<FWait>(
            dll,
            "sr_wait_baseline");

    auto cancel_baseline =
        load_fn<FInt>(
            dll,
            "sr_cancel_background_baseline");

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

    uint32_t version =
        api();

    SRHandle h =
        create(
            arena_bytes,
            core_bytes);

    if (!h) {
        FreeLibrary(dll);
        return 3;
    }

    auto* arena =
        static_cast<uint8_t*>(
            datafn(h));

    if (!arena) {
        return 4;
    }

    for (size_t p = 0;
         p < pages;
         ++p) {
        fill_page(
            arena +
                p * PAGE_BYTES,
            p);
    }

    ProcMem active_initial =
        proc_mem();

    /*
      Attempt 1 proves interruptibility. Build only part of a baseline while
      the host keeps touching its foreground core, then cancel and require the
      partial capsule to disappear.
    */
    if (!begin_baseline(
            h,
            2,
            3)) {
        std::cerr
            << "first background baseline failed to start\n";
        return 5;
    }

    uint64_t cancel_probe_passes = 0;

    while (baseline_progress(h) < 200 &&
           baseline_done(h) == 0) {
        volatile uint64_t sum = 0;

        for (size_t off = 0;
             off < core_bytes;
             off += PAGE_BYTES * 8) {
            sum += arena[off];
        }

        (void)sum;
        ++cancel_probe_passes;
        Sleep(1);
    }

    if (!cancel_baseline(h)) {
        std::cerr
            << "background baseline cancel failed\n";
        return 6;
    }

    Sleep(30);

    ProcMem after_cancel =
        proc_mem();

    SRMetrics after_cancel_metrics{};

    if (!metricsfn(
            h,
            &after_cancel_metrics)) {
        return 7;
    }

    /*
      Attempt 2 is the real baseline. The host remains active throughout it.
      We also mutate pages DURING the build so correctness depends on the
      write-watch delta covering writes that race with baseline capture.
    */
    if (!begin_baseline(
            h,
            2,
            3)) {
        std::cerr
            << "second background baseline failed to start\n";
        return 8;
    }

    std::vector<double>
        foreground_probe_ms;

    foreground_probe_ms.reserve(
        4096);

    uint64_t foreground_passes = 0;
    uint64_t concurrent_writes = 0;
    uint64_t foreground_checksum = 0;

    while (baseline_done(h) == 0) {
        double t0 =
            now_ms();

        uint64_t local = 0;

        for (size_t off = 0;
             off < core_bytes;
             off += PAGE_BYTES * 4) {
            local += arena[off];
        }

        foreground_checksum ^=
            splitmix64(
                local +
                foreground_passes);

        double dt =
            now_ms() - t0;

        foreground_probe_ms.push_back(
            dt);

        ++foreground_passes;

        if ((foreground_passes %
                25ull) == 0) {
            size_t page =
                (foreground_passes *
                    997ull +
                 211ull) %
                pages;

            arena[
                page * PAGE_BYTES +
                71] ^=
                    static_cast<uint8_t>(
                        foreground_passes &
                        0xffu);

            ++concurrent_writes;
        }

        Sleep(1);
    }

    if (!wait_baseline(
            h,
            30000)) {
        std::cerr
            << "background baseline did not complete\n";
        return 9;
    }

    SRMetrics baseline_metrics{};

    if (!metricsfn(
            h,
            &baseline_metrics)) {
        return 10;
    }

    /*
      More active changes after the background baseline. All changes since
      baseline start must be captured by the final tiny delta.
    */
    mutate_sparse(
        arena,
        pages,
        dirty_target,
        9);

    const uint64_t expected_core =
        fnv1a64(
            arena,
            core_bytes);

    const uint64_t expected_full =
        fnv1a64(
            arena,
            arena_bytes);

    if (!dormant(h)) {
        std::cerr
            << "enter dormant failed\n";
        return 11;
    }

    Sleep(50);

    ProcMem dormant_mem =
        proc_mem();

    if (!resume_core(h)) {
        std::cerr
            << "resume core failed\n";
        return 12;
    }

    bool core_ok =
        fnv1a64(
            arena,
            core_bytes) ==
        expected_core;

    if (!start_deep(h)) {
        std::cerr
            << "deep restore start failed\n";
        return 13;
    }

    uint64_t core_passes = 0;
    uint64_t core_checksum = 0;

    while (deep_done(h) == 0) {
        uint64_t local = 0;

        for (size_t off = 0;
             off < core_bytes;
             off += PAGE_BYTES) {
            local += arena[off];
        }

        core_checksum ^=
            splitmix64(
                local +
                core_passes);

        ++core_passes;
        Sleep(0);
    }

    bool deep_ok =
        wait_deep(
            h,
            30000) != 0;

    bool full_ok =
        deep_ok &&
        fnv1a64(
            arena,
            arena_bytes) ==
        expected_full;

    ProcMem restored_with_capsule =
        proc_mem();

    if (!release_capsule(h)) {
        std::cerr
            << "capsule release failed\n";
        return 14;
    }

    Sleep(30);

    ProcMem active_after_release =
        proc_mem();

    SRMetrics final_metrics{};

    if (!metricsfn(
            h,
            &final_metrics)) {
        return 15;
    }

    const double raw_mb =
        static_cast<double>(
            arena_bytes) /
        (1024.0 * 1024.0);

    const double fg_p95 =
        percentile(
            foreground_probe_ms,
            0.95);

    const double fg_p99 =
        percentile(
            foreground_probe_ms,
            0.99);

    const double fg_max =
        foreground_probe_ms.empty()
            ? 0.0
            : *std::max_element(
                foreground_probe_ms.begin(),
                foreground_probe_ms.end());

    const double reclaimed_mb =
        restored_with_capsule.private_mb -
        active_after_release.private_mb;

    bool version_ok =
        (version >> 16) == 9;

    bool cancel_ok =
        after_cancel_metrics.baseline_attempts == 1 &&
        after_cancel_metrics.baseline_cancellations == 1 &&
        after_cancel_metrics.baseline_epochs == 0 &&
        after_cancel.private_mb <=
            active_initial.private_mb +
            12.0;

    bool background_ok =
        final_metrics.baseline_attempts == 2 &&
        final_metrics.baseline_cancellations == 1 &&
        final_metrics.baseline_pressure_aborts == 0 &&
        final_metrics.baseline_epochs == 1 &&
        final_metrics.background_mode_entered == 1 &&
        final_metrics.baseline_wall_ms >
            final_metrics.baseline_work_ms &&
        foreground_passes > 100 &&
        fg_max < 50.0;

    bool dirty_ok =
        final_metrics.dirty_pages >=
            dirty_target &&
        final_metrics.dirty_pages <=
            dirty_target +
            concurrent_writes +
            32;

    bool lifecycle_ok =
        dormant_mem.private_mb <
            raw_mb * 0.50 &&
        core_ok &&
        full_ok &&
        core_passes > 0 &&
        reclaimed_mb > 70.0 &&
        active_after_release.private_mb <=
            active_initial.private_mb +
            12.0 &&
        final_metrics.lifecycle_state ==
            SR_ACTIVE_NO_CAPSULE &&
        final_metrics.packed_used_bytes == 0 &&
        final_metrics.packed_committed_bytes == 0;

    bool pass =
        version_ok &&
        cancel_ok &&
        background_ok &&
        dirty_ok &&
        lifecycle_ok;

    std::ofstream out(
        "windows_phase9.json",
        std::ios::binary);

    out << "{\n";
    out << "  \"phase\": "
        << "\"Windows Phase 9 paced interruptible background baseline\",\n";
    out << "  \"api_version\": "
        << version << ",\n";
    out << "  \"arena_mb\": 256,\n";
    out << "  \"resume_core_mb\": 8,\n";
    out << "  \"active_initial_private_mb\": "
        << std::fixed
        << std::setprecision(3)
        << active_initial.private_mb
        << ",\n";
    out << "  \"cancel_probe_passes\": "
        << cancel_probe_passes << ",\n";
    out << "  \"private_after_cancel_mb\": "
        << after_cancel.private_mb << ",\n";
    out << "  \"baseline_attempts\": "
        << final_metrics.baseline_attempts << ",\n";
    out << "  \"baseline_cancellations\": "
        << final_metrics.baseline_cancellations << ",\n";
    out << "  \"baseline_pressure_aborts\": "
        << final_metrics.baseline_pressure_aborts << ",\n";
    out << "  \"baseline_epochs\": "
        << final_metrics.baseline_epochs << ",\n";
    out << "  \"background_mode_entered\": "
        << final_metrics.background_mode_entered << ",\n";
    out << "  \"baseline_slices\": "
        << final_metrics.baseline_slices << ",\n";
    out << "  \"baseline_cpu_backoffs\": "
        << final_metrics.baseline_cpu_backoffs << ",\n";
    out << "  \"baseline_wall_ms\": "
        << final_metrics.baseline_wall_ms << ",\n";
    out << "  \"baseline_work_ms\": "
        << final_metrics.baseline_work_ms << ",\n";
    out << "  \"foreground_probe_count\": "
        << foreground_probe_ms.size() << ",\n";
    out << "  \"foreground_probe_p95_ms\": "
        << fg_p95 << ",\n";
    out << "  \"foreground_probe_p99_ms\": "
        << fg_p99 << ",\n";
    out << "  \"foreground_probe_max_ms\": "
        << fg_max << ",\n";
    out << "  \"foreground_checksum\": "
        << foreground_checksum << ",\n";
    out << "  \"concurrent_writes_during_baseline\": "
        << concurrent_writes << ",\n";
    out << "  \"dirty_pages_finalized\": "
        << final_metrics.dirty_pages << ",\n";
    out << "  \"finalization_ms\": "
        << final_metrics.finalization_ms << ",\n";
    out << "  \"dormant_private_mb\": "
        << dormant_mem.private_mb << ",\n";
    out << "  \"core_restore_ms\": "
        << final_metrics.core_restore_ms << ",\n";
    out << "  \"deep_restore_ms\": "
        << final_metrics.deep_restore_ms << ",\n";
    out << "  \"core_passes_during_deep_restore\": "
        << core_passes << ",\n";
    out << "  \"restored_with_capsule_private_mb\": "
        << restored_with_capsule.private_mb << ",\n";
    out << "  \"active_after_release_private_mb\": "
        << active_after_release.private_mb << ",\n";
    out << "  \"capsule_reclaimed_mb\": "
        << reclaimed_mb << ",\n";
    out << "  \"core_hash_ok\": "
        << (core_ok ? "true" : "false")
        << ",\n";
    out << "  \"full_hash_ok\": "
        << (full_ok ? "true" : "false")
        << ",\n";
    out << "  \"pass\": "
        << (pass ? "true" : "false")
        << "\n";
    out << "}\n";

    out.close();

    std::cout
        << "StateRAM Windows Phase 9 - paced interruptible background baseline\n";

    std::cout
        << " cancelled partial baseline and returned to "
        << after_cancel.private_mb
        << " MiB private commit\n";

    std::cout
        << " successful baseline: wall="
        << final_metrics.baseline_wall_ms
        << " ms actual compression work="
        << final_metrics.baseline_work_ms
        << " ms slices="
        << final_metrics.baseline_slices
        << " cpu_backoffs="
        << final_metrics.baseline_cpu_backoffs
        << "\n";

    std::cout
        << " foreground while building: passes="
        << foreground_passes
        << " p95="
        << fg_p95
        << " ms p99="
        << fg_p99
        << " ms max="
        << fg_max
        << " ms writes="
        << concurrent_writes
        << "\n";

    std::cout
        << " dormant private="
        << dormant_mem.private_mb
        << " MiB finalization="
        << final_metrics.finalization_ms
        << " ms core_restore="
        << final_metrics.core_restore_ms
        << " ms\n";

    std::cout
        << " core integrity="
        << (core_ok ? "OK" : "FAIL")
        << " full integrity="
        << (full_ok ? "OK" : "FAIL")
        << "\n";

    std::cout
        << " active after release="
        << active_after_release.private_mb
        << " MiB reclaimed="
        << reclaimed_mb
        << " MiB\n";

    std::cout
        << " WINDOWS_STATERAM_PHASE9="
        << (pass ? "PASS" : "FAIL")
        << "\n";

    destroy(h);
    FreeLibrary(dll);

    return pass ? 0 : 10;
}
