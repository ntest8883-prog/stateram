#include "platform.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

#ifdef _WIN32
#define _WIN32_WINNT 0x0602
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>
#pragma comment(lib, "Cabinet.lib")
#else
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <zlib.h>
#endif

namespace sr::platform {

static size_t page_round(size_t bytes) {
#ifdef _WIN32
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
#else
    const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
#endif
    return (bytes + page - 1) / page * page;
}

void* reserve_address_space(size_t bytes) {
    bytes = page_round(bytes);
#ifdef _WIN32
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(nullptr, bytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

bool commit_pages(void* address, size_t bytes) {
    bytes = page_round(bytes);
#ifdef _WIN32
    return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) == address;
#else
    return mprotect(address, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

bool decommit_pages(void* address, size_t bytes) {
    bytes = page_round(bytes);
#ifdef _WIN32
    return VirtualFree(address, bytes, MEM_DECOMMIT) != FALSE;
#else
    if (madvise(address, bytes, MADV_DONTNEED) != 0) {
        return false;
    }
    return mprotect(address, bytes, PROT_NONE) == 0;
#endif
}

void release_address_space(void* address, size_t bytes) {
    if (!address) return;
#ifdef _WIN32
    (void)bytes;
    VirtualFree(address, 0, MEM_RELEASE);
#else
    munmap(address, page_round(bytes));
#endif
}

#ifdef _WIN32
struct WinCodec {
    COMPRESSOR_HANDLE compressor = nullptr;
    DECOMPRESSOR_HANDLE decompressor = nullptr;

    WinCodec() {
        CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor);
        CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor);
    }

    ~WinCodec() {
        if (compressor) CloseCompressor(compressor);
        if (decompressor) CloseDecompressor(decompressor);
    }
};

static thread_local WinCodec codec;
#endif

bool compress_fast(
    const uint8_t* input,
    size_t input_bytes,
    Blob& out
) {
    out.bytes.clear();
    out.compressed = false;

    if (!input || input_bytes == 0) return false;

#ifdef _WIN32
    if (!codec.compressor) return false;

    SIZE_T needed = 0;
    BOOL first = Compress(
        codec.compressor,
        input,
        input_bytes,
        nullptr,
        0,
        &needed);

    if (first ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        needed == 0) {
        return false;
    }

    std::vector<uint8_t> tmp(needed);
    SIZE_T got = 0;

    if (!Compress(
            codec.compressor,
            input,
            input_bytes,
            tmp.data(),
            tmp.size(),
            &got)) {
        return false;
    }

    tmp.resize(got);

    if (got + 64 < input_bytes) {
        out.bytes = std::move(tmp);
        out.compressed = true;
    } else {
        out.bytes.assign(input, input + input_bytes);
        out.compressed = false;
    }
#else
    uLongf bound = compressBound(static_cast<uLong>(input_bytes));
    std::vector<uint8_t> tmp(bound);
    uLongf got = bound;

    int rc = compress2(
        tmp.data(),
        &got,
        input,
        static_cast<uLong>(input_bytes),
        Z_BEST_SPEED);

    if (rc != Z_OK) return false;
    tmp.resize(static_cast<size_t>(got));

    if (tmp.size() + 64 < input_bytes) {
        out.bytes = std::move(tmp);
        out.compressed = true;
    } else {
        out.bytes.assign(input, input + input_bytes);
        out.compressed = false;
    }
#endif
    return true;
}

bool decompress_fast(
    const Blob& blob,
    uint8_t* output,
    size_t output_bytes
) {
    if (!output || output_bytes == 0) return false;

    if (!blob.compressed) {
        if (blob.bytes.size() != output_bytes) return false;
        std::memcpy(output, blob.bytes.data(), output_bytes);
        return true;
    }

#ifdef _WIN32
    if (!codec.decompressor) return false;

    SIZE_T got = 0;
    if (!Decompress(
            codec.decompressor,
            blob.bytes.data(),
            blob.bytes.size(),
            output,
            output_bytes,
            &got)) {
        return false;
    }
    return got == output_bytes;
#else
    uLongf got = static_cast<uLongf>(output_bytes);
    int rc = uncompress(
        output,
        &got,
        blob.bytes.data(),
        static_cast<uLong>(blob.bytes.size()));
    return rc == Z_OK && got == output_bytes;
#endif
}

MemoryInfo memory_info() {
    MemoryInfo out{};
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        out.total_physical = ms.ullTotalPhys;
        out.available_physical = ms.ullAvailPhys;
        out.load_percent = ms.dwMemoryLoad;
    }
#else
    struct sysinfo si{};
    if (::sysinfo(&si) == 0) {
        const uint64_t unit = static_cast<uint64_t>(si.mem_unit);
        out.total_physical = static_cast<uint64_t>(si.totalram) * unit;
        out.available_physical =
            (static_cast<uint64_t>(si.freeram) +
             static_cast<uint64_t>(si.bufferram)) * unit;
        if (out.total_physical) {
            const uint64_t used = out.total_physical -
                std::min(out.total_physical, out.available_physical);
            out.load_percent = static_cast<uint32_t>(
                (100ull * used) / out.total_physical);
        }
    }
#endif
    return out;
}

double now_ms() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

uint64_t monotonic_tick_ns() {
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now().time_since_epoch()).count());
}

void begin_background_mode() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
}

void end_background_mode() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
#endif
}

void cooperative_yield(uint32_t sleep_ms) {
    if (sleep_ms == 0) {
        std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
}

} // namespace sr::platform
