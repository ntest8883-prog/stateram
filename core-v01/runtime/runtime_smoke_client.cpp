#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include "stateram_core.h"
#include "stateram_runtime_client.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static constexpr uint64_t MB = 1024ull * 1024ull;

struct RecipeState {
    uint64_t salt = 0;
};

static uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static int recipe(
    SRObjectId,
    uint64_t offset,
    uint64_t length,
    void* destination,
    void* user
) {
    auto* st = static_cast<RecipeState*>(user);
    auto* p = static_cast<uint8_t*>(destination);
    if (!st || !p) return 0;

    for (uint64_t i = 0; i < length; ++i) {
        const uint64_t absolute = offset + i;
        p[i] = static_cast<uint8_t>(
            mix64(st->salt ^ (absolute / 32)) >>
            ((absolute & 7ull) * 8));
    }

    return 1;
}

int main(int argc, char** argv) {
    std::string name = "client";
    bool interactive = false;
    int loops = 20;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--interactive") == 0) {
            interactive = true;
        } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (std::strcmp(argv[i], "--loops") == 0 && i + 1 < argc) {
            loops = std::max(1, std::atoi(argv[++i]));
        }
    }

    SRContextConfig cfg{};
    cfg.target_raw_bytes = 384ull * MB;
    cfg.hard_raw_bytes = 448ull * MB;
    cfg.foreground_burst_bytes = 32ull * MB;
    cfg.comfortable_available_bytes = 128ull * MB;
    cfg.emergency_available_bytes = 64ull * MB;
    cfg.background_period_ms = 1000;
    cfg.prewarm_segments_per_hint = 1;
    cfg.overlay_consolidate_pages = 16;

    SRContext ctx = nullptr;
    if (sr_context_create(&cfg, &ctx) != SR_OK) return 10;

    RecipeState state{};
    state.salt = interactive
        ? 0x1111222233334444ull
        : 0xaaaabbbbccccddddull;

    SRObjectDescriptor d{};
    d.name = "runtime-smoke-state";
    d.logical_size = 512ull * MB;
    d.semantic_class = SR_SEM_REBUILDABLE_DERIVED;
    d.backing_kind = SR_BACKING_RECIPE;
    d.rebuild = recipe;
    d.user = &state;

    SRObjectId id = 0;
    if (sr_register_object(ctx, &d, nullptr, 0, &id) != SR_OK) {
        sr_context_destroy(ctx);
        return 11;
    }

    /* Materialize 384 MiB so runtime policy can visibly reclaim a background client. */
    for (uint64_t off = 0; off < 384ull * MB; off += 8ull * MB) {
        SRLease lease = nullptr;
        if (sr_acquire(
                ctx,
                id,
                off,
                8ull * MB,
                SR_ACCESS_READ,
                SR_LATENCY_NORMAL,
                &lease) != SR_OK) {
            sr_context_destroy(ctx);
            return 12;
        }
        volatile uint8_t sink =
            static_cast<uint8_t*>(sr_lease_data(lease))[0];
        (void)sink;
        sr_release(lease);
    }

    SRRuntimeClient runtime = nullptr;
    const uint32_t flags =
        interactive
            ? SRR_CLIENT_INTERACTIVE_HINT
            : SRR_CLIENT_NONE;

    if (srr_connect(name.c_str(), flags, &runtime) != 0) {
        sr_context_destroy(ctx);
        return 13;
    }

    uint64_t min_target = UINT64_MAX;
    uint64_t max_target = 0;
    uint64_t min_raw = UINT64_MAX;
    uint64_t max_raw = 0;
    uint32_t max_clients = 0;
    uint32_t foreground_seen = 0;

    for (int i = 0; i < loops; ++i) {
        SRRuntimePolicy p{};
        if (srr_sync(runtime, ctx, flags, &p) != 0) {
            srr_disconnect(runtime);
            sr_context_destroy(ctx);
            return 14;
        }

        SRStats s{};
        sr_get_stats(ctx, &s);

        min_target = std::min(min_target, p.recommended_raw_target);
        max_target = std::max(max_target, p.recommended_raw_target);
        min_raw = std::min(min_raw, s.raw_resident_bytes);
        max_raw = std::max(max_raw, s.raw_resident_bytes);
        max_clients = std::max(max_clients, p.connected_clients);
        foreground_seen = std::max(foreground_seen, p.foreground);

        std::this_thread::sleep_for(
            std::chrono::milliseconds(200));
    }

    const std::string out_name =
        "runtime_smoke_" + name + ".json";

    std::ofstream out(out_name, std::ios::binary);
    out
        << "{\n"
        << "  \"name\": \"" << name << "\",\n"
        << "  \"interactive_hint\": "
        << (interactive ? "true" : "false") << ",\n"
        << "  \"min_target_mb\": "
        << (min_target / static_cast<double>(MB)) << ",\n"
        << "  \"max_target_mb\": "
        << (max_target / static_cast<double>(MB)) << ",\n"
        << "  \"min_raw_mb\": "
        << (min_raw / static_cast<double>(MB)) << ",\n"
        << "  \"max_raw_mb\": "
        << (max_raw / static_cast<double>(MB)) << ",\n"
        << "  \"max_connected_clients\": "
        << max_clients << ",\n"
        << "  \"foreground_seen\": "
        << foreground_seen << "\n"
        << "}\n";

    std::cout
        << "RUNTIME_SMOKE_CLIENT=" << name
        << " min_target_mb="
        << (min_target / static_cast<double>(MB))
        << " min_raw_mb="
        << (min_raw / static_cast<double>(MB))
        << " max_clients=" << max_clients
        << " foreground_seen=" << foreground_seen
        << "\n";

    srr_disconnect(runtime);
    sr_context_destroy(ctx);
    return 0;
}
#else
int main() { return 1; }
#endif
