#include "stateram_core.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

static uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static void deterministic_fill(uint64_t offset, uint64_t length, void* dst, uint64_t salt) {
    auto* p = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < length; ++i) {
        const uint64_t absolute = offset + i;
        p[i] = static_cast<uint8_t>(mix64((absolute / 32) ^ salt) >> ((absolute & 7) * 8));
    }
}

static int recipe_cb(
    SRObjectId,
    uint64_t offset,
    uint64_t length,
    void* destination,
    void* user
) {
    uint64_t salt = *static_cast<uint64_t*>(user);
    deterministic_fill(offset, length, destination, salt);
    return 1;
}

static bool expect_recipe_bytes(const uint8_t* data, uint64_t offset, uint64_t length, uint64_t salt) {
    std::vector<uint8_t> expected(static_cast<size_t>(length));
    deterministic_fill(offset, length, expected.data(), salt);
    return std::memcmp(data, expected.data(), static_cast<size_t>(length)) == 0;
}

static void fail(const char* what) {
    std::cerr << "SELF-CHECK FAIL: " << what << "\n";
    std::exit(2);
}

int main() {
    constexpr uint64_t MB = 1024ull * 1024ull;

    SRContextConfig cfg{};
    cfg.target_raw_bytes = 12 * MB;
    cfg.hard_raw_bytes = 20 * MB;
    cfg.foreground_burst_bytes = 8 * MB;
    cfg.comfortable_available_bytes = 64 * MB;
    cfg.emergency_available_bytes = 32 * MB;
    cfg.background_period_ms = 10;
    cfg.prewarm_segments_per_hint = 2;
    cfg.overlay_consolidate_pages = 16;

    SRContext ctx = nullptr;
    if (sr_context_create(&cfg, &ctx) != SR_OK) fail("context create");

    uint64_t salt = 0x123456789abcdef0ull;
    SRObjectDescriptor recipe{};
    recipe.name = "derived_search_index";
    recipe.logical_size = 64 * MB;
    recipe.critical_prefix_bytes = 2 * MB;
    recipe.semantic_class = SR_SEM_DERIVED_INDEX;
    recipe.backing_kind = SR_BACKING_RECIPE;
    recipe.flags = SR_OBJECT_PROTECT_CRITICAL_PREFIX;
    recipe.rebuild = recipe_cb;
    recipe.user = &salt;

    SRObjectId recipe_id = 0;
    if (sr_register_object(ctx, &recipe, nullptr, 0, &recipe_id) != SR_OK) {
        fail("register recipe object");
    }

    const uint64_t request_offset = 17 * MB + 1234;
    const uint64_t request_length = 700 * 1024;

    SRLease read1 = nullptr;
    if (sr_acquire(ctx, recipe_id, request_offset, request_length,
                   SR_ACCESS_READ, SR_LATENCY_INTERACTIVE, &read1) != SR_OK) {
        fail("first acquire");
    }

    auto* p1 = static_cast<uint8_t*>(sr_lease_data(read1));
    if (!p1 || !expect_recipe_bytes(p1, request_offset, request_length, salt)) {
        fail("recipe reconstruction bytes");
    }
    if (sr_release(read1) != SR_OK) fail("first release");

    const uint64_t write_offset = 18 * MB + 4096;
    const uint64_t write_length = 8192;
    SRLease write = nullptr;
    if (sr_acquire(ctx, recipe_id, write_offset, write_length,
                   SR_ACCESS_WRITE, SR_LATENCY_INTERACTIVE, &write) != SR_OK) {
        fail("write acquire");
    }

    auto* wp = static_cast<uint8_t*>(sr_lease_data(write));
    if (!wp) fail("write pointer");
    for (uint64_t i = 0; i < write_length; ++i) {
        wp[i] ^= static_cast<uint8_t>(0xA5u + (i & 7));
    }
    if (sr_release(write) != SR_OK) fail("write release");

    if (sr_object_generation(ctx, recipe_id) == 0) fail("generation did not advance");

    sr_hint_cold(ctx, recipe_id);
    (void)sr_trim(ctx, 2 * MB);

    SRLease verify = nullptr;
    if (sr_acquire(ctx, recipe_id, write_offset, write_length,
                   SR_ACCESS_READ, SR_LATENCY_INTERACTIVE, &verify) != SR_OK) {
        fail("overlay reacquire");
    }

    auto* vp = static_cast<uint8_t*>(sr_lease_data(verify));
    std::vector<uint8_t> baseline(write_length);
    deterministic_fill(write_offset, write_length, baseline.data(), salt);
    for (uint64_t i = 0; i < write_length; ++i) {
        baseline[static_cast<size_t>(i)] ^= static_cast<uint8_t>(0xA5u + (i & 7));
    }
    if (std::memcmp(vp, baseline.data(), static_cast<size_t>(write_length)) != 0) {
        fail("recipe+delta overlay did not survive dormancy");
    }
    if (sr_release(verify) != SR_OK) fail("overlay verify release");

    std::vector<uint8_t> raw(8 * MB);
    for (size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<uint8_t>((i / 4096) & 0x0f);
    }

    SRObjectDescriptor raw_desc{};
    raw_desc.name = "editable_document_history";
    raw_desc.logical_size = raw.size();
    raw_desc.critical_prefix_bytes = 512 * 1024;
    raw_desc.semantic_class = SR_SEM_HISTORY;
    raw_desc.backing_kind = SR_BACKING_NONE;

    SRObjectId raw_id = 0;
    if (sr_register_object(ctx, &raw_desc, raw.data(), raw.size(), &raw_id) != SR_OK) {
        fail("register raw object");
    }

    sr_hint_cold(ctx, raw_id);
    if (sr_trim(ctx, 4 * MB) != SR_OK) {
        fail("streaming trim could not reach target");
    }

    SRLease raw_verify = nullptr;
    if (sr_acquire(ctx, raw_id, 3 * MB, 256 * 1024,
                   SR_ACCESS_READ, SR_LATENCY_INTERACTIVE, &raw_verify) != SR_OK) {
        fail("compressed reacquire");
    }
    if (std::memcmp(sr_lease_data(raw_verify), raw.data() + 3 * MB, 256 * 1024) != 0) {
        fail("compressed exact restore mismatch");
    }
    sr_release(raw_verify);

    SRStats stats{};
    if (sr_get_stats(ctx, &stats) != SR_OK) fail("stats");

    std::cout
        << "STATERAM_CORE_V01_SELF_CHECK=PASS\n"
        << "logical_mb=" << (stats.logical_bytes / MB) << "\n"
        << "raw_resident_mb=" << (stats.raw_resident_bytes / MB) << "\n"
        << "compressed_mb=" << (stats.compressed_bytes / MB) << "\n"
        << "overlay_kb=" << (stats.overlay_bytes / 1024) << "\n"
        << "promotions=" << stats.promotions << "\n"
        << "streaming_evictions=" << stats.streaming_evictions << "\n"
        << "write_generations=" << stats.write_generations << "\n";

    sr_context_destroy(ctx);
    return 0;
}
