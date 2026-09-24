#define _WIN32_WINNT 0x0602
#define PSAPI_VERSION 1
#define NOMINMAX

#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include "stateram_sdk.h"

#pragma comment(lib, "Psapi.lib")

static constexpr size_t PAGE_BYTES = 4096;

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
            reinterpret_cast<
                uint64_t*>(page);

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
        h *=
            1099511628211ull;
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

struct CycleResult {
    ProcMem dormant{};
    ProcMem restored_with_capsule{};
    ProcMem active_after_release{};

    SRMetrics before_release{};
    SRMetrics after_release{};

    uint64_t core_passes = 0;

    bool core_ok = false;
    bool full_ok = false;
    bool deep_ok = false;
    bool release_ok = false;
};

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

    auto checkpoint =
        load_fn<FInt>(
            dll,
            "sr_checkpoint_baseline");

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

    ProcMem active_raw_initial =
        proc_mem();

    auto run_cycle =
        [&](uint64_t salt,
            CycleResult& out) -> bool {

        if (!checkpoint(h)) {
            std::cerr
                << "checkpoint failed\n";
            return false;
        }

        mutate_sparse(
            arena,
            pages,
            dirty_target,
            salt);

        uint64_t expected_core =
            fnv1a64(
                arena,
                core_bytes);

        uint64_t expected_full =
            fnv1a64(
                arena,
                arena_bytes);

        if (!dormant(h)) {
            std::cerr
                << "dormant failed\n";
            return false;
        }

        Sleep(50);

        out.dormant =
            proc_mem();

        if (!resume_core(h)) {
            std::cerr
                << "resume core failed\n";
            return false;
        }

        out.core_ok =
            fnv1a64(
                arena,
                core_bytes) ==
            expected_core;

        if (!start_deep(h)) {
            std::cerr
                << "start deep failed\n";
            return false;
        }

        uint64_t passes = 0;
        uint64_t checksum = 0;

        while (deep_done(h) == 0) {
            uint64_t local = 0;

            for (size_t off = 0;
                 off < core_bytes;
                 off += PAGE_BYTES) {
                local +=
                    arena[off];
            }

            checksum ^=
                splitmix64(
                    local + passes);

            ++passes;

            Sleep(0);
        }

        out.core_passes =
            passes;

        out.deep_ok =
            wait_deep(
                h,
                30000) != 0;

        out.full_ok =
            out.deep_ok &&
            fnv1a64(
                arena,
                arena_bytes) ==
            expected_full;

        out.restored_with_capsule =
            proc_mem();

        if (!metricsfn(
                h,
                &out.before_release)) {
            return false;
        }

        out.release_ok =
            release_capsule(h) != 0;

        Sleep(25);

        out.active_after_release =
            proc_mem();

        if (!metricsfn(
                h,
                &out.after_release)) {
            return false;
        }

        return
            out.core_ok &&
            out.deep_ok &&
            out.full_ok &&
            out.release_ok &&
            out.core_passes > 0;
    };

    CycleResult c1{};
    CycleResult c2{};

    bool cycle1_ok =
        run_cycle(
            1,
            c1);

    bool cycle2_ok =
        cycle1_ok &&
        run_cycle(
            2,
            c2);

    SRMetrics final_metrics{};

    if (!metricsfn(
            h,
            &final_metrics)) {
        return 9;
    }

    const double raw_mb =
        static_cast<double>(
            arena_bytes) /
        (1024.0 * 1024.0);

    const double c1_reclaimed =
        c1.restored_with_capsule.private_mb -
        c1.active_after_release.private_mb;

    const double c2_reclaimed =
        c2.restored_with_capsule.private_mb -
        c2.active_after_release.private_mb;

    const double active_tolerance_mb =
        12.0;

    bool version_ok =
        (version >> 16) == 8;

    bool c1_dormant_ok =
        c1.dormant.private_mb <
        raw_mb * 0.50;

    bool c2_dormant_ok =
        c2.dormant.private_mb <
        raw_mb * 0.50;

    bool c1_release_memory_ok =
        c1_reclaimed > 70.0 &&
        c1.active_after_release.private_mb <=
            active_raw_initial.private_mb +
            active_tolerance_mb;

    bool c2_release_memory_ok =
        c2_reclaimed > 70.0 &&
        c2.active_after_release.private_mb <=
            active_raw_initial.private_mb +
            active_tolerance_mb;

    bool lifecycle_counts_ok =
        final_metrics.baseline_epochs == 2 &&
        final_metrics.capsule_releases == 2 &&
        final_metrics.lifecycle_state ==
            SR_ACTIVE_NO_CAPSULE;

    bool pack_cleared_ok =
        final_metrics.packed_used_bytes == 0 &&
        final_metrics.packed_committed_bytes == 0;

    bool cycle_integrity_ok =
        cycle1_ok &&
        cycle2_ok;

    bool pass =
        version_ok &&
        cycle_integrity_ok &&
        c1_dormant_ok &&
        c2_dormant_ok &&
        c1_release_memory_ok &&
        c2_release_memory_ok &&
        lifecycle_counts_ok &&
        pack_cleared_ok;

    std::ofstream out(
        "windows_phase8.json",
        std::ios::binary);

    out << "{\n";
    out << "  \"phase\": "
        << "\"Windows Phase 8 full lifecycle reclamation\",\n";
    out << "  \"api_version\": "
        << version << ",\n";
    out << "  \"runtime_linked_dll\": true,\n";
    out << "  \"arena_mb\": 256,\n";
    out << "  \"resume_core_mb\": 8,\n";
    out << "  \"active_raw_initial_private_mb\": "
        << std::fixed
        << std::setprecision(3)
        << active_raw_initial.private_mb
        << ",\n";

    out << "  \"cycle1\": {\n";
    out << "    \"dormant_private_mb\": "
        << c1.dormant.private_mb << ",\n";
    out << "    \"restored_with_capsule_private_mb\": "
        << c1.restored_with_capsule.private_mb << ",\n";
    out << "    \"active_after_release_private_mb\": "
        << c1.active_after_release.private_mb << ",\n";
    out << "    \"capsule_reclaimed_mb\": "
        << c1_reclaimed << ",\n";
    out << "    \"packed_before_release_mb\": "
        << (double)c1.before_release.packed_committed_bytes /
           (1024.0 * 1024.0) << ",\n";
    out << "    \"finalization_ms\": "
        << c1.before_release.finalization_ms << ",\n";
    out << "    \"core_restore_ms\": "
        << c1.before_release.core_restore_ms << ",\n";
    out << "    \"deep_restore_ms\": "
        << c1.before_release.deep_restore_ms << ",\n";
    out << "    \"release_ms\": "
        << c1.after_release.capsule_release_ms << ",\n";
    out << "    \"core_passes\": "
        << c1.core_passes << ",\n";
    out << "    \"core_hash_ok\": "
        << (c1.core_ok ? "true" : "false") << ",\n";
    out << "    \"full_hash_ok\": "
        << (c1.full_ok ? "true" : "false") << "\n";
    out << "  },\n";

    out << "  \"cycle2\": {\n";
    out << "    \"dormant_private_mb\": "
        << c2.dormant.private_mb << ",\n";
    out << "    \"restored_with_capsule_private_mb\": "
        << c2.restored_with_capsule.private_mb << ",\n";
    out << "    \"active_after_release_private_mb\": "
        << c2.active_after_release.private_mb << ",\n";
    out << "    \"capsule_reclaimed_mb\": "
        << c2_reclaimed << ",\n";
    out << "    \"packed_before_release_mb\": "
        << (double)c2.before_release.packed_committed_bytes /
           (1024.0 * 1024.0) << ",\n";
    out << "    \"finalization_ms\": "
        << c2.before_release.finalization_ms << ",\n";
    out << "    \"core_restore_ms\": "
        << c2.before_release.core_restore_ms << ",\n";
    out << "    \"deep_restore_ms\": "
        << c2.before_release.deep_restore_ms << ",\n";
    out << "    \"release_ms\": "
        << c2.after_release.capsule_release_ms << ",\n";
    out << "    \"core_passes\": "
        << c2.core_passes << ",\n";
    out << "    \"core_hash_ok\": "
        << (c2.core_ok ? "true" : "false") << ",\n";
    out << "    \"full_hash_ok\": "
        << (c2.full_ok ? "true" : "false") << "\n";
    out << "  },\n";

    out << "  \"baseline_epochs\": "
        << final_metrics.baseline_epochs << ",\n";
    out << "  \"capsule_releases\": "
        << final_metrics.capsule_releases << ",\n";
    out << "  \"final_lifecycle_state\": "
        << final_metrics.lifecycle_state << ",\n";
    out << "  \"final_packed_used_bytes\": "
        << final_metrics.packed_used_bytes << ",\n";
    out << "  \"final_packed_committed_bytes\": "
        << final_metrics.packed_committed_bytes << ",\n";
    out << "  \"pass\": "
        << (pass ? "true" : "false")
        << "\n";
    out << "}\n";

    out.close();

    std::cout
        << "StateRAM Windows Phase 8 - full lifecycle reclamation\n";

    std::cout
        << " initial raw active private: "
        << active_raw_initial.private_mb
        << " MiB\n";

    std::cout
        << " cycle1: dormant="
        << c1.dormant.private_mb
        << " MiB restored+capsule="
        << c1.restored_with_capsule.private_mb
        << " MiB after-release="
        << c1.active_after_release.private_mb
        << " MiB reclaimed="
        << c1_reclaimed
        << " MiB\n";

    std::cout
        << " cycle2: dormant="
        << c2.dormant.private_mb
        << " MiB restored+capsule="
        << c2.restored_with_capsule.private_mb
        << " MiB after-release="
        << c2.active_after_release.private_mb
        << " MiB reclaimed="
        << c2_reclaimed
        << " MiB\n";

    std::cout
        << " lifecycle epochs="
        << final_metrics.baseline_epochs
        << " releases="
        << final_metrics.capsule_releases
        << " final_state="
        << final_metrics.lifecycle_state
        << "\n";

    std::cout
        << " WINDOWS_STATERAM_PHASE8="
        << (pass ? "PASS" : "FAIL")
        << "\n";

    destroy(h);
    FreeLibrary(dll);

    return pass ? 0 : 10;
}
