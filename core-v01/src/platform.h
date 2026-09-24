#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sr::platform {

struct MemoryInfo {
    uint64_t total_physical = 0;
    uint64_t available_physical = 0;
    uint32_t load_percent = 0;
};

struct Blob {
    std::vector<uint8_t> bytes;
    bool compressed = false;
};

void* reserve_address_space(size_t bytes);
bool commit_pages(void* address, size_t bytes);
bool decommit_pages(void* address, size_t bytes);
void release_address_space(void* address, size_t bytes);

bool compress_fast(
    const uint8_t* input,
    size_t input_bytes,
    Blob& out);

bool decompress_fast(
    const Blob& blob,
    uint8_t* output,
    size_t output_bytes);

MemoryInfo memory_info();

double now_ms();
uint64_t monotonic_tick_ns();

void begin_background_mode();
void end_background_mode();
void cooperative_yield(uint32_t sleep_ms);

} // namespace sr::platform
