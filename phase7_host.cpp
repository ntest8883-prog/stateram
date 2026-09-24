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

#include "stateram_sdk.h"

#pragma comment(lib, "Psapi.lib")

static constexpr size_t PAGE_BYTES = 4096;

static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static void fill_page(uint8_t* page, uint64_t page_index) {
    if ((page_index & 3ull) == 0) {
        auto* words = reinterpret_cast<uint64_t*>(page);
        for (size_t i = 0; i < PAGE_BYTES / 8; ++i)
            words[i] = splitmix64(
                page_index * 0xD6E8FEB86659FD93ull + i);
        return;
    }

    uint8_t base = (uint8_t)((page_index * 29 + 17) & 0xffu);
    std::memset(page, base, PAGE_BYTES);

    for (size_t off = 0; off < PAGE_BYTES; off += 512) {
        uint64_t x = splitmix64(
            page_index * 0xA0761D6478BD642Full + off);
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
    double private_mb = 0;
    double ws_mb = 0;
};

static ProcMem proc_mem() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    ProcMem out{};

    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc))) {
        out.private_mb = (double)pmc.PrivateUsage / (1024.0 * 1024.0);
        out.ws_mb = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    }
    return out;
}

template <typename T>
static T load_fn(HMODULE dll, const char* name) {
    FARPROC p = GetProcAddress(dll, name);
    if (!p) throw std::runtime_error(std::string("missing export: ") + name);
    return reinterpret_cast<T>(p);
}

int main() {
    const size_t arena_bytes = 256ull * 1024ull * 1024ull;
    const size_t core_bytes = 8ull * 1024ull * 1024ull;
    const size_t pages = arena_bytes / PAGE_BYTES;
    const size_t dirty_target = 512;

    HMODULE dll = LoadLibraryW(L"stateram_sdk.dll");
    if (!dll) {
        std::cerr << "LoadLibrary failed " << GetLastError() << "\n";
        return 2;
    }

    using FApi = uint32_t (*)();
    using FCreate = SRHandle (*)(uint64_t, uint64_t);
    using FData = void* (*)(SRHandle);
    using FInt = int (*)(SRHandle);
    using FWait = int (*)(SRHandle, uint32_t);
    using FMetrics = int (*)(SRHandle, SRMetrics*);
    using FDestroy = void (*)(SRHandle);

    auto api = load_fn<FApi>(dll, "sr_api_version");
    auto create = load_fn<FCreate>(dll, "sr_create");
    auto datafn = load_fn<FData>(dll, "sr_data");
    auto checkpoint = load_fn<FInt>(dll, "sr_checkpoint_baseline");
    auto dormant = load_fn<FInt>(dll, "sr_enter_dormant");
    auto resume_core = load_fn<FInt>(dll, "sr_resume_core");
    auto start_deep = load_fn<FInt>(dll, "sr_start_deep_restore");
    auto deep_done = load_fn<FInt>(dll, "sr_deep_done");
    auto wait_deep = load_fn<FWait>(dll, "sr_wait_deep");
    auto metricsfn = load_fn<FMetrics>(dll, "sr_get_metrics");
    auto destroy = load_fn<FDestroy>(dll, "sr_destroy");

    uint32_t version = api();
    SRHandle h = create(arena_bytes, core_bytes);
    if (!h) {
        std::cerr << "sr_create failed\n";
        FreeLibrary(dll);
        return 3;
    }

    auto* arena = static_cast<uint8_t*>(datafn(h));
    if (!arena) return 4;

    for (size_t p = 0; p < pages; ++p)
        fill_page(arena + p * PAGE_BYTES, p);

    ProcMem active_before_checkpoint = proc_mem();

    if (!checkpoint(h)) {
        std::cerr << "baseline checkpoint failed\n";
        return 5;
    }

    // A normal application keeps using the managed region after the baseline.
    // Only a sparse set changes; the SDK tracks those pages using write-watch.
    for (size_t i = 0; i < dirty_target; ++i) {
        size_t page = (i * 131ull + 17ull) % pages;
        uint8_t* p = arena + page * PAGE_BYTES;
        p[13] ^= (uint8_t)(0x5A ^ (i & 0xffu));
        p[2047] ^= (uint8_t)(0xA5 ^ ((i * 7u) & 0xffu));
    }

    const uint64_t expected_core = fnv1a64(arena, core_bytes);
    const uint64_t expected_full = fnv1a64(arena, arena_bytes);

    if (!dormant(h)) {
        std::cerr << "enter dormancy failed\n";
        return 6;
    }

    Sleep(50);
    ProcMem dormant_mem = proc_mem();

    if (!resume_core(h)) {
        std::cerr << "resume core failed\n";
        return 7;
    }

    uint64_t got_core = fnv1a64(arena, core_bytes);
    bool core_ok = got_core == expected_core;

    if (!start_deep(h)) {
        std::cerr << "start deep restore failed\n";
        return 8;
    }

    uint64_t core_passes = 0;
    uint64_t core_checksum = 0;

    while (deep_done(h) == 0) {
        uint64_t local = 0;
        for (size_t off = 0; off < core_bytes; off += PAGE_BYTES)
            local += arena[off];

        core_checksum ^= splitmix64(local + core_passes);
        ++core_passes;
        Sleep(0);
    }

    bool deep_ok = wait_deep(h, 30000) != 0;
    uint64_t got_full = fnv1a64(arena, arena_bytes);
    bool full_ok = deep_ok && got_full == expected_full;

    ProcMem restored_mem = proc_mem();

    SRMetrics m{};
    if (!metricsfn(h, &m)) return 9;

    const double packed_used_mb =
        (double)m.packed_used_bytes / (1024.0 * 1024.0);
    const double packed_commit_mb =
        (double)m.packed_committed_bytes / (1024.0 * 1024.0);
    const double raw_mb =
        (double)arena_bytes / (1024.0 * 1024.0);

    bool version_ok = (version >> 16) == 7;
    bool dirty_ok = m.dirty_pages > 0 &&
                    m.dirty_pages <= dirty_target + 16;
    bool compact_ok = packed_commit_mb <= packed_used_mb + 0.125;
    bool actual_reduction_ok = dormant_mem.private_mb < raw_mb * 0.50;
    bool latency_shape_ok =
        m.finalization_ms < m.baseline_ms &&
        m.core_restore_ms < m.deep_restore_ms;

    bool pass =
        version_ok &&
        dirty_ok &&
        compact_ok &&
        actual_reduction_ok &&
        latency_shape_ok &&
        core_ok &&
        full_ok &&
        core_passes > 0 &&
        m.deep_restore_ok == 1;

    std::ofstream out("windows_phase7.json", std::ios::binary);
    out << "{\n";
    out << "  \"phase\": \"Windows Phase 7 cooperative SDK boundary\",\n";
    out << "  \"api_version\": " << version << ",\n";
    out << "  \"runtime_linked_dll\": true,\n";
    out << "  \"arena_mb\": 256,\n";
    out << "  \"resume_core_mb\": 8,\n";
    out << "  \"dirty_pages\": " << m.dirty_pages << ",\n";
    out << "  \"baseline_payload_mb\": " << std::fixed << std::setprecision(3)
        << ((double)m.baseline_payload_bytes / (1024.0 * 1024.0)) << ",\n";
    out << "  \"packed_used_mb\": " << packed_used_mb << ",\n";
    out << "  \"packed_committed_mb\": " << packed_commit_mb << ",\n";
    out << "  \"active_private_before_checkpoint_mb\": "
        << active_before_checkpoint.private_mb << ",\n";
    out << "  \"dormant_process_private_mb\": "
        << dormant_mem.private_mb << ",\n";
    out << "  \"dormant_working_set_mb\": "
        << dormant_mem.ws_mb << ",\n";
    out << "  \"restored_process_private_mb\": "
        << restored_mem.private_mb << ",\n";
    out << "  \"baseline_ms\": " << m.baseline_ms << ",\n";
    out << "  \"finalization_ms\": " << m.finalization_ms << ",\n";
    out << "  \"decommit_ms\": " << m.decommit_ms << ",\n";
    out << "  \"core_restore_ms\": " << m.core_restore_ms << ",\n";
    out << "  \"deep_restore_ms\": " << m.deep_restore_ms << ",\n";
    out << "  \"core_passes_during_deep_restore\": " << core_passes << ",\n";
    out << "  \"low_memory_signal\": " << m.low_memory_signal << ",\n";
    out << "  \"core_hash_ok\": " << (core_ok ? "true" : "false") << ",\n";
    out << "  \"full_hash_ok\": " << (full_ok ? "true" : "false") << ",\n";
    out << "  \"actual_memory_reduction_ok\": "
        << (actual_reduction_ok ? "true" : "false") << ",\n";
    out << "  \"pass\": " << (pass ? "true" : "false") << "\n";
    out << "}\n";
    out.close();

    std::cout << "StateRAM Windows Phase 7 - cooperative SDK boundary\n";
    std::cout << " runtime-loaded DLL API version: 0x"
              << std::hex << version << std::dec << "\n";
    std::cout << " packed state: " << packed_used_mb
              << " MiB used / " << packed_commit_mb << " MiB committed\n";
    std::cout << " dormant whole-process private: "
              << dormant_mem.private_mb << " MiB\n";
    std::cout << " finalization=" << m.finalization_ms
              << " ms core_restore=" << m.core_restore_ms
              << " ms deep_restore=" << m.deep_restore_ms << " ms\n";
    std::cout << " host completed " << core_passes
              << " core-only passes while DLL restored deep state\n";
    std::cout << " core integrity=" << (core_ok ? "OK" : "FAIL")
              << " full integrity=" << (full_ok ? "OK" : "FAIL") << "\n";
    std::cout << " WINDOWS_STATERAM_PHASE7="
              << (pass ? "PASS" : "FAIL") << "\n";

    destroy(h);
    FreeLibrary(dll);
    return pass ? 0 : 10;
}
