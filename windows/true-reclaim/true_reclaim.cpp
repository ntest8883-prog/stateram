#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#pragma comment(lib, "Cabinet.lib")
#pragma comment(lib, "Psapi.lib")

namespace {

enum : LONG {
    PAGE_RESIDENT = 0,
    PAGE_COMPRESSED = 1,
    PAGE_RESTORING = 2,
    PAGE_EVICTING = 3
};

struct PageMeta {
    volatile LONG state;
    void* compressed;
    SIZE_T compressed_size;
    std::uint64_t hash;
};

struct MemorySnapshot {
    SIZE_T working_set;
    SIZE_T private_usage;
    DWORD page_faults;
    ULONGLONG system_available;
};

BYTE* g_region = nullptr;
SIZE_T g_region_size = 0;
SIZE_T g_page_size = 0;
SIZE_T g_page_count = 0;
PageMeta* g_pages = nullptr;
COMPRESSOR_HANDLE g_compressor = nullptr;
DECOMPRESSOR_HANDLE g_decompressor = nullptr;

volatile LONG64 g_faults_handled = 0;
volatile LONG64 g_restores = 0;
volatile LONG64 g_hash_failures = 0;

std::uint64_t HashPage(const BYTE* data, SIZE_T size) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (SIZE_T i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

MemorySnapshot CaptureMemory() noexcept {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                         sizeof(pmc));

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    MemorySnapshot s{};
    s.working_set = pmc.WorkingSetSize;
    s.private_usage = pmc.PrivateUsage;
    s.page_faults = pmc.PageFaultCount;
    s.system_available = ms.ullAvailPhys;
    return s;
}

bool RestorePage(SIZE_T index) noexcept {
    PageMeta& meta = g_pages[index];

    if (InterlockedCompareExchange(&meta.state,
                                   PAGE_RESTORING,
                                   PAGE_COMPRESSED) != PAGE_COMPRESSED) {
        return meta.state == PAGE_RESIDENT;
    }

    BYTE* page = g_region + index * g_page_size;
    void* committed =
        VirtualAlloc(page, g_page_size, MEM_COMMIT, PAGE_READWRITE);

    if (committed != page) {
        InterlockedExchange(&meta.state, PAGE_COMPRESSED);
        return false;
    }

    SIZE_T restored = 0;
    if (!Decompress(g_decompressor,
                    meta.compressed,
                    meta.compressed_size,
                    page,
                    g_page_size,
                    &restored) ||
        restored != g_page_size) {
        VirtualFree(page, g_page_size, MEM_DECOMMIT);
        InterlockedExchange(&meta.state, PAGE_COMPRESSED);
        return false;
    }

    if (HashPage(page, g_page_size) != meta.hash) {
        InterlockedIncrement64(&g_hash_failures);
        VirtualFree(page, g_page_size, MEM_DECOMMIT);
        InterlockedExchange(&meta.state, PAGE_COMPRESSED);
        return false;
    }

    if (meta.compressed != nullptr) {
        HeapFree(GetProcessHeap(), 0, meta.compressed);
        meta.compressed = nullptr;
        meta.compressed_size = 0;
    }

    MemoryBarrier();
    InterlockedExchange(&meta.state, PAGE_RESIDENT);
    InterlockedIncrement64(&g_restores);
    return true;
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* info) noexcept {
    if (info == nullptr ||
        info->ExceptionRecord == nullptr ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        info->ExceptionRecord->NumberParameters < 2 ||
        g_region == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const ULONG_PTR fault =
        static_cast<ULONG_PTR>(info->ExceptionRecord->ExceptionInformation[1]);
    const ULONG_PTR begin = reinterpret_cast<ULONG_PTR>(g_region);
    const ULONG_PTR end = begin + g_region_size;

    if (fault < begin || fault >= end) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const SIZE_T index = static_cast<SIZE_T>((fault - begin) / g_page_size);
    if (index >= g_page_count || g_pages[index].state != PAGE_COMPRESSED) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (!RestorePage(index)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    InterlockedIncrement64(&g_faults_handled);
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool CompressAndDecommit(SIZE_T index,
                         SIZE_T* compressed_bytes,
                         bool* skipped_incompressible) noexcept {
    PageMeta& meta = g_pages[index];

    if (InterlockedCompareExchange(&meta.state,
                                   PAGE_EVICTING,
                                   PAGE_RESIDENT) != PAGE_RESIDENT) {
        return false;
    }

    BYTE* page = g_region + index * g_page_size;
    const std::uint64_t hash = HashPage(page, g_page_size);

    SIZE_T needed = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL probe =
        Compress(g_compressor,
                 page,
                 g_page_size,
                 nullptr,
                 0,
                 &needed);

    if (!probe && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        InterlockedExchange(&meta.state, PAGE_RESIDENT);
        return false;
    }

    if (needed == 0) {
        InterlockedExchange(&meta.state, PAGE_RESIDENT);
        return false;
    }

    // The zero-sized probe returns the buffer size required to guarantee
    // compression succeeds, not the final compressed payload size. Allocate
    // that buffer first, then decide whether the actual result is worth
    // decommitting the source page.
    void* buffer = HeapAlloc(GetProcessHeap(), 0, needed);
    if (buffer == nullptr) {
        InterlockedExchange(&meta.state, PAGE_RESIDENT);
        return false;
    }

    SIZE_T actual = 0;
    if (!Compress(g_compressor,
                  page,
                  g_page_size,
                  buffer,
                  needed,
                  &actual) ||
        actual == 0 ||
        actual >= (g_page_size - 128)) {
        HeapFree(GetProcessHeap(), 0, buffer);
        *skipped_incompressible = true;
        InterlockedExchange(&meta.state, PAGE_RESIDENT);
        return true;
    }

    meta.compressed = buffer;
    meta.compressed_size = actual;
    meta.hash = hash;
    MemoryBarrier();

    if (!VirtualFree(page, g_page_size, MEM_DECOMMIT)) {
        HeapFree(GetProcessHeap(), 0, buffer);
        meta.compressed = nullptr;
        meta.compressed_size = 0;
        InterlockedExchange(&meta.state, PAGE_RESIDENT);
        return false;
    }

    InterlockedExchange(&meta.state, PAGE_COMPRESSED);
    *compressed_bytes += actual;
    return true;
}

void FillPage(BYTE* page, SIZE_T index) noexcept {
    switch (index & 3U) {
    case 0:
        std::memset(page, 0, g_page_size);
        break;

    case 1:
        for (SIZE_T i = 0; i < g_page_size; ++i) {
            page[i] = static_cast<BYTE>(((i / 64) + index) & 0xFFU);
        }
        break;

    case 2: {
        static constexpr char text[] =
            "StateRAM real reclamation test: structured application-like "
            "content with repeated fields, strings, zeros and small deltas. ";
        const SIZE_T n = sizeof(text) - 1;
        for (SIZE_T i = 0; i < g_page_size; ++i) {
            page[i] = static_cast<BYTE>(text[(i + index) % n]);
        }
        break;
    }

    default: {
        std::uint64_t x =
            0x9E3779B97F4A7C15ULL ^
            (static_cast<std::uint64_t>(index) * 0xD1B54A32D192ED03ULL);
        for (SIZE_T i = 0; i < g_page_size; ++i) {
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            x *= 2685821657736338717ULL;
            page[i] = static_cast<BYTE>(x >> 56);
        }
        break;
    }
    }
}

bool IsReservedPage(SIZE_T index) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    BYTE* page = g_region + index * g_page_size;
    if (VirtualQuery(page, &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    return mbi.State == MEM_RESERVE;
}

SIZE_T ParseSizeArg(int argc,
                    char** argv,
                    const char* name,
                    SIZE_T default_value) noexcept {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            const unsigned long long value =
                std::strtoull(argv[i + 1], nullptr, 10);
            if (value > 0) {
                return static_cast<SIZE_T>(value);
            }
        }
    }
    return default_value;
}

void PrintMiB(const char* key, ULONGLONG bytes) noexcept {
    std::printf("%s=%.3f MiB\n",
                key,
                static_cast<double>(bytes) / (1024.0 * 1024.0));
}

} // namespace

int main(int argc, char** argv) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    g_page_size = si.dwPageSize;

    const SIZE_T logical_mib = ParseSizeArg(argc, argv, "--mib", 128);
    const SIZE_T requested_samples = ParseSizeArg(argc, argv, "--sample", 256);

    if (g_page_size == 0 ||
        logical_mib > 4096ULL ||
        requested_samples > 1000000ULL) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }

    g_region_size = logical_mib * 1024ULL * 1024ULL;
    g_region_size =
        ((g_region_size + g_page_size - 1) / g_page_size) * g_page_size;
    g_page_count = g_region_size / g_page_size;

    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,
                          nullptr,
                          &g_compressor) ||
        !CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,
                            nullptr,
                            &g_decompressor)) {
        std::fprintf(stderr,
                     "compression API initialization failed: %lu\n",
                     GetLastError());
        return 3;
    }

    g_pages = new (std::nothrow) PageMeta[g_page_count]();
    std::vector<std::uint64_t> expected(g_page_count);

    if (g_pages == nullptr || expected.size() != g_page_count) {
        std::fprintf(stderr, "metadata allocation failed\n");
        return 4;
    }

    g_region = static_cast<BYTE*>(
        VirtualAlloc(nullptr, g_region_size, MEM_RESERVE, PAGE_NOACCESS));
    if (g_region == nullptr ||
        VirtualAlloc(g_region,
                     g_region_size,
                     MEM_COMMIT,
                     PAGE_READWRITE) != g_region) {
        std::fprintf(stderr,
                     "managed region allocation failed: %lu\n",
                     GetLastError());
        return 5;
    }

    PVOID veh = AddVectoredExceptionHandler(1, VectoredHandler);
    if (veh == nullptr) {
        std::fprintf(stderr, "AddVectoredExceptionHandler failed\n");
        return 6;
    }

    for (SIZE_T i = 0; i < g_page_count; ++i) {
        BYTE* page = g_region + i * g_page_size;
        FillPage(page, i);
        expected[i] = HashPage(page, g_page_size);
        g_pages[i].state = PAGE_RESIDENT;
    }

    Sleep(50);
    const MemorySnapshot before = CaptureMemory();

    SIZE_T payload_bytes = 0;
    SIZE_T compressed_pages = 0;
    SIZE_T incompressible_pages = 0;
    bool ok = true;

    std::vector<SIZE_T> compressed_indices;
    compressed_indices.reserve(g_page_count);

    for (SIZE_T i = 0; i < g_page_count; ++i) {
        bool skipped = false;
        const SIZE_T before_bytes = payload_bytes;

        if (!CompressAndDecommit(i, &payload_bytes, &skipped)) {
            ok = false;
            break;
        }

        if (skipped) {
            ++incompressible_pages;
        } else if (payload_bytes > before_bytes) {
            ++compressed_pages;
            compressed_indices.push_back(i);
        }
    }

    SIZE_T reserved_verified = 0;
    if (ok) {
        for (SIZE_T index : compressed_indices) {
            if (IsReservedPage(index)) {
                ++reserved_verified;
            }
        }
    }

    Sleep(50);
    const MemorySnapshot after_evict = CaptureMemory();

    const SIZE_T sample_count =
        std::min(requested_samples, compressed_indices.size());

    for (SIZE_T s = 0; ok && s < sample_count; ++s) {
        const SIZE_T pos =
            (s * compressed_indices.size()) /
            std::max<SIZE_T>(sample_count, 1);
        const SIZE_T index =
            compressed_indices[std::min(pos, compressed_indices.size() - 1)];

        volatile BYTE v = g_region[index * g_page_size];
        (void)v;

        if (HashPage(g_region + index * g_page_size, g_page_size) !=
            expected[index]) {
            ok = false;
            break;
        }

        const SIZE_T offset =
            ((index * 977U) + 1379U) & (g_page_size - 1U);
        g_region[index * g_page_size + offset] ^= 0x5AU;
        expected[index] =
            HashPage(g_region + index * g_page_size, g_page_size);
    }

    SIZE_T second_payload = 0;
    for (SIZE_T s = 0; ok && s < sample_count; ++s) {
        const SIZE_T pos =
            (s * compressed_indices.size()) /
            std::max<SIZE_T>(sample_count, 1);
        const SIZE_T index =
            compressed_indices[std::min(pos, compressed_indices.size() - 1)];

        bool skipped = false;
        if (!CompressAndDecommit(index, &second_payload, &skipped) ||
            skipped) {
            ok = false;
            break;
        }

        volatile BYTE v = g_region[index * g_page_size];
        (void)v;

        if (HashPage(g_region + index * g_page_size, g_page_size) !=
            expected[index]) {
            ok = false;
            break;
        }
    }

    const MemorySnapshot after_restore = CaptureMemory();

    const ULONGLONG evicted_bytes =
        static_cast<ULONGLONG>(compressed_pages) * g_page_size;
    const ULONGLONG metadata_bytes =
        static_cast<ULONGLONG>(g_page_count) * sizeof(PageMeta);
    const LONGLONG model_net_saved =
        static_cast<LONGLONG>(evicted_bytes) -
        static_cast<LONGLONG>(payload_bytes) -
        static_cast<LONGLONG>(metadata_bytes);
    const LONGLONG private_drop =
        static_cast<LONGLONG>(before.private_usage) -
        static_cast<LONGLONG>(after_evict.private_usage);

    std::printf("StateRAM Windows true-reclamation subtest\n");
    std::printf("profile=mixed-synthetic-selective\n");
    std::printf("page_size=%llu\n",
                static_cast<unsigned long long>(g_page_size));
    std::printf("logical_pages=%llu\n",
                static_cast<unsigned long long>(g_page_count));
    std::printf("compressed_decommitted_pages=%llu\n",
                static_cast<unsigned long long>(compressed_pages));
    std::printf("incompressible_pages_kept_resident=%llu\n",
                static_cast<unsigned long long>(incompressible_pages));
    std::printf("reserved_state_verified_pages=%llu\n",
                static_cast<unsigned long long>(reserved_verified));
    std::printf("sample_pages_double_cycle=%llu\n",
                static_cast<unsigned long long>(sample_count));
    std::printf("veh_faults_handled=%lld\n",
                static_cast<long long>(g_faults_handled));
    std::printf("restores=%lld\n",
                static_cast<long long>(g_restores));
    std::printf("hash_failures=%lld\n",
                static_cast<long long>(g_hash_failures));

    PrintMiB("logical_size", g_region_size);
    PrintMiB("evicted_original_bytes", evicted_bytes);
    PrintMiB("compressed_payload_bytes", payload_bytes);
    PrintMiB("metadata_bytes", metadata_bytes);

    std::printf("payload_model_net_saved=%.3f MiB\n",
                static_cast<double>(model_net_saved) / (1024.0 * 1024.0));

    PrintMiB("private_usage_before", before.private_usage);
    PrintMiB("private_usage_after_evict", after_evict.private_usage);
    PrintMiB("private_usage_after_restore_sample", after_restore.private_usage);
    std::printf("measured_private_usage_drop=%.3f MiB\n",
                static_cast<double>(private_drop) / (1024.0 * 1024.0));

    PrintMiB("working_set_before", before.working_set);
    PrintMiB("working_set_after_evict", after_evict.working_set);
    PrintMiB("working_set_after_restore_sample", after_restore.working_set);
    PrintMiB("system_available_before", before.system_available);
    PrintMiB("system_available_after_evict", after_evict.system_available);

    if (compressed_pages == 0 ||
        reserved_verified != compressed_pages ||
        payload_bytes >= evicted_bytes ||
        model_net_saved <= 0 ||
        sample_count == 0 ||
        g_faults_handled < static_cast<LONG64>(sample_count * 2) ||
        g_restores < static_cast<LONG64>(sample_count * 2) ||
        g_hash_failures != 0) {
        ok = false;
    }

    std::printf("RESULT=%s\n", ok ? "PASS" : "FAIL");

    RemoveVectoredExceptionHandler(veh);

    for (SIZE_T i = 0; i < g_page_count; ++i) {
        if (g_pages[i].state == PAGE_COMPRESSED) {
            RestorePage(i);
        }
        if (g_pages[i].compressed != nullptr) {
            HeapFree(GetProcessHeap(), 0, g_pages[i].compressed);
            g_pages[i].compressed = nullptr;
        }
    }

    VirtualFree(g_region, 0, MEM_RELEASE);
    delete[] g_pages;
    CloseCompressor(g_compressor);
    CloseDecompressor(g_decompressor);

    return ok ? 0 : 1;
}
