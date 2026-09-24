#define _WIN32_WINNT 0x0602
#define NOMINMAX
#define STATERAM_SDK_BUILD
#include "stateram_sdk.h"

#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "Cabinet.lib")

static constexpr size_t PAGE_BYTES = 4096;
static constexpr size_t UNIT_BYTES = 256 * 1024;
static constexpr size_t PAGES_PER_UNIT = UNIT_BYTES / PAGE_BYTES;
static constexpr size_t COMMIT_CHUNK = 64 * 1024;
static constexpr uint32_t NO_DELTA = 0xffffffffu;

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

static size_t round_up(size_t x, size_t a) {
    return (x + a - 1) / a * a;
}

class PackedArena {
public:
    explicit PackedArena(size_t reserve_bytes) {
        reserve_bytes_ =
            round_up(reserve_bytes, COMMIT_CHUNK);

        base_ = static_cast<uint8_t*>(
            VirtualAlloc(
                nullptr,
                reserve_bytes_,
                MEM_RESERVE,
                PAGE_NOACCESS));

        if (!base_) {
            throw std::runtime_error(
                "pack reserve failed");
        }
    }

    ~PackedArena() {
        if (base_) {
            VirtualFree(base_, 0, MEM_RELEASE);
        }
    }

    uint64_t append(
        const void* data,
        size_t bytes
    ) {
        if (bytes == 0) {
            return static_cast<uint64_t>(used_);
        }

        if (used_ + bytes > reserve_bytes_) {
            throw std::runtime_error(
                "pack exhausted");
        }

        size_t need =
            round_up(
                used_ + bytes,
                COMMIT_CHUNK);

        if (need > committed_) {
            size_t add =
                need - committed_;

            void* want =
                base_ + committed_;

            void* got =
                VirtualAlloc(
                    want,
                    add,
                    MEM_COMMIT,
                    PAGE_READWRITE);

            if (got != want) {
                throw std::runtime_error(
                    "pack commit failed");
            }

            committed_ = need;
        }

        uint64_t off =
            static_cast<uint64_t>(
                used_);

        std::memcpy(
            base_ + used_,
            data,
            bytes);

        used_ += bytes;

        return off;
    }

    const uint8_t* at(
        uint64_t off
    ) const {
        if (off >= used_) {
            throw std::runtime_error(
                "bad pack offset");
        }

        return base_ +
               static_cast<size_t>(off);
    }

    size_t used() const {
        return used_;
    }

    size_t committed() const {
        return committed_;
    }

private:
    uint8_t* base_ = nullptr;
    size_t reserve_bytes_ = 0;
    size_t used_ = 0;
    size_t committed_ = 0;
};

struct UnitRef {
    uint64_t offset = 0;
    uint32_t length = 0;
    uint8_t compressed = 0;
    uint8_t pad[3]{};
};

struct Region {
    uint8_t* arena = nullptr;

    size_t bytes = 0;
    size_t core_bytes = 0;
    size_t units = 0;
    size_t pages = 0;

    COMPRESSOR_HANDLE compressor =
        nullptr;

    DECOMPRESSOR_HANDLE decompressor =
        nullptr;

    std::unique_ptr<PackedArena> pack;

    std::vector<UnitRef> refs;
    std::vector<uint32_t> delta_offsets;

    SRMetrics metrics{};

    HANDLE low_mem = nullptr;
    HANDLE deep_thread = nullptr;

    std::atomic<int> deep_done{0};
    std::atomic<int> deep_ok{0};
    std::atomic<uint32_t> state{
        SR_ACTIVE_NO_CAPSULE};
};

static void sync_metric_state(
    Region* r
) {
    r->metrics.lifecycle_state =
        r->state.load(
            std::memory_order_acquire);
}

static bool compress_append(
    Region* r,
    const uint8_t* raw,
    std::vector<uint8_t>& scratch,
    UnitRef& ref
) {
    SIZE_T needed = 0;

    BOOL first =
        Compress(
            r->compressor,
            raw,
            UNIT_BYTES,
            nullptr,
            0,
            &needed);

    if (first ||
        GetLastError() !=
            ERROR_INSUFFICIENT_BUFFER ||
        needed == 0) {
        return false;
    }

    scratch.resize(needed);

    SIZE_T got = 0;

    if (!Compress(
            r->compressor,
            raw,
            UNIT_BYTES,
            scratch.data(),
            scratch.size(),
            &got)) {
        return false;
    }

    if (got == 0 ||
        got >
            std::numeric_limits<
                uint32_t>::max()) {
        return false;
    }

    if (got < UNIT_BYTES) {
        ref.offset =
            r->pack->append(
                scratch.data(),
                got);

        ref.length =
            static_cast<uint32_t>(got);

        ref.compressed = 1;
    } else {
        ref.offset =
            r->pack->append(
                raw,
                UNIT_BYTES);

        ref.length =
            static_cast<uint32_t>(
                UNIT_BYTES);

        ref.compressed = 0;
    }

    return true;
}

static bool build_unit(
    Region* r,
    size_t u,
    std::vector<uint8_t>& raw
) {
    if (!r->pack) return false;

    raw.assign(
        UNIT_BYTES,
        0);

    const UnitRef& ref =
        r->refs[u];

    if (ref.compressed) {
        SIZE_T got = 0;

        if (!Decompress(
                r->decompressor,
                r->pack->at(
                    ref.offset),
                ref.length,
                raw.data(),
                raw.size(),
                &got)) {
            return false;
        }

        if (got != UNIT_BYTES) {
            return false;
        }
    } else {
        if (ref.length !=
            UNIT_BYTES) {
            return false;
        }

        std::memcpy(
            raw.data(),
            r->pack->at(
                ref.offset),
            UNIT_BYTES);
    }

    const size_t first_page =
        u * PAGES_PER_UNIT;

    for (size_t p = 0;
         p < PAGES_PER_UNIT;
         ++p) {
        size_t page =
            first_page + p;

        uint32_t off =
            r->delta_offsets[page];

        if (off != NO_DELTA) {
            std::memcpy(
                raw.data() +
                    p * PAGE_BYTES,
                r->pack->at(off),
                PAGE_BYTES);
        }
    }

    return true;
}

static bool restore_range(
    Region* r,
    size_t first,
    size_t end
) {
    std::vector<uint8_t> raw(
        UNIT_BYTES);

    for (size_t u = first;
         u < end;
         ++u) {
        if (!build_unit(
                r,
                u,
                raw)) {
            return false;
        }

        uint8_t* target =
            r->arena +
            u * UNIT_BYTES;

        void* p =
            VirtualAlloc(
                target,
                UNIT_BYTES,
                MEM_COMMIT,
                PAGE_READWRITE);

        if (p != target) {
            return false;
        }

        std::memcpy(
            target,
            raw.data(),
            UNIT_BYTES);
    }

    return true;
}

static DWORD WINAPI deep_thread_proc(
    LPVOID arg
) {
    Region* r =
        static_cast<Region*>(arg);

    double t0 =
        now_ms();

    size_t core_units =
        r->core_bytes /
        UNIT_BYTES;

    bool ok =
        restore_range(
            r,
            core_units,
            r->units);

    r->metrics.deep_restore_ms =
        now_ms() - t0;

    r->deep_ok.store(
        ok ? 1 : 0,
        std::memory_order_release);

    r->metrics.deep_restore_ok =
        ok ? 1 : 0;

    if (ok) {
        r->state.store(
            SR_ACTIVE_CAPSULE,
            std::memory_order_release);
    }

    sync_metric_state(r);

    r->deep_done.store(
        1,
        std::memory_order_release);

    return ok ? 0 : 1;
}

SR_API uint32_t sr_api_version() {
    return 0x00080001u;
}

SR_API SRHandle sr_create(
    uint64_t bytes64,
    uint64_t core64
) {
    try {
        if (bytes64 == 0 ||
            core64 == 0 ||
            bytes64 % UNIT_BYTES != 0 ||
            core64 % UNIT_BYTES != 0 ||
            core64 >= bytes64 ||
            bytes64 >
                static_cast<uint64_t>(
                    SIZE_MAX)) {
            return nullptr;
        }

        std::unique_ptr<Region>
            r(new Region{});

        r->bytes =
            static_cast<size_t>(
                bytes64);

        r->core_bytes =
            static_cast<size_t>(
                core64);

        r->units =
            r->bytes /
            UNIT_BYTES;

        r->pages =
            r->bytes /
            PAGE_BYTES;

        r->arena =
            static_cast<uint8_t*>(
                VirtualAlloc(
                    nullptr,
                    r->bytes,
                    MEM_RESERVE |
                    MEM_COMMIT |
                    MEM_WRITE_WATCH,
                    PAGE_READWRITE));

        if (!r->arena) {
            return nullptr;
        }

        DWORD alg =
            COMPRESS_ALGORITHM_XPRESS |
            COMPRESS_RAW;

        if (!CreateCompressor(
                alg,
                nullptr,
                &r->compressor)) {
            return nullptr;
        }

        if (!CreateDecompressor(
                alg,
                nullptr,
                &r->decompressor)) {
            return nullptr;
        }

        r->refs.resize(
            r->units);

        r->delta_offsets.assign(
            r->pages,
            NO_DELTA);

        r->low_mem =
            CreateMemoryResourceNotification(
                LowMemoryResourceNotification);

        r->metrics.arena_bytes =
            r->bytes;

        r->metrics.core_bytes =
            r->core_bytes;

        sync_metric_state(
            r.get());

        return r.release();

    } catch (...) {
        return nullptr;
    }
}

SR_API void* sr_data(
    SRHandle handle
) {
    Region* r =
        static_cast<Region*>(
            handle);

    return r
        ? r->arena
        : nullptr;
}

SR_API int sr_checkpoint_baseline(
    SRHandle handle
) {
    Region* r =
        static_cast<Region*>(
            handle);

    if (!r || !r->arena) {
        return 0;
    }

    if (r->deep_thread) {
        return 0;
    }

    if (r->state.load() !=
            SR_ACTIVE_NO_CAPSULE) {
        return 0;
    }

    try {
        r->pack.reset(
            new PackedArena(
                r->bytes +
                r->bytes));

        std::fill(
            r->delta_offsets.begin(),
            r->delta_offsets.end(),
            NO_DELTA);

        std::fill(
            r->refs.begin(),
            r->refs.end(),
            UnitRef{});

        std::vector<uint8_t> scratch;
        scratch.reserve(
            UNIT_BYTES);

        double t0 =
            now_ms();

        for (size_t u = 0;
             u < r->units;
             ++u) {
            if (!compress_append(
                    r,
                    r->arena +
                        u * UNIT_BYTES,
                    scratch,
                    r->refs[u])) {
                return 0;
            }
        }

        r->metrics.baseline_ms =
            now_ms() - t0;

        r->metrics.baseline_payload_bytes =
            r->pack->used();

        r->metrics.packed_used_bytes =
            r->pack->used();

        r->metrics.packed_committed_bytes =
            r->pack->committed();

        r->metrics.baseline_epochs += 1;

        if (ResetWriteWatch(
                r->arena,
                r->bytes) != 0) {
            return 0;
        }

        r->state.store(
            SR_ACTIVE_BASELINE,
            std::memory_order_release);

        sync_metric_state(r);

        return 1;

    } catch (...) {
        return 0;
    }
}

SR_API int sr_enter_dormant(
    SRHandle handle
) {
    Region* r =
        static_cast<Region*>(
            handle);

    if (!r ||
        !r->arena ||
        !r->pack) {
        return 0;
    }

    if (r->deep_thread) {
        return 0;
    }

    if (r->state.load() !=
            SR_ACTIVE_BASELINE) {
        return 0;
    }

    try {
        double t0 =
            now_ms();

        std::vector<void*> pages(
            r->pages);

        ULONG_PTR count =
            pages.size();

        DWORD granularity = 0;

        UINT wr =
            GetWriteWatch(
                WRITE_WATCH_FLAG_RESET,
                r->arena,
                r->bytes,
                pages.data(),
                &count,
                &granularity);

        if (wr != 0 ||
            granularity !=
                PAGE_BYTES) {
            return 0;
        }

        std::array<
            uint8_t,
            PAGE_BYTES> tmp{};

        for (ULONG_PTR i = 0;
             i < count;
             ++i) {
            uintptr_t addr =
                reinterpret_cast<
                    uintptr_t>(
                        pages[i]);

            uintptr_t base =
                reinterpret_cast<
                    uintptr_t>(
                        r->arena);

            size_t page_index =
                (addr - base) /
                PAGE_BYTES;

            if (page_index >=
                r->pages) {
                return 0;
            }

            std::memcpy(
                tmp.data(),
                r->arena +
                    page_index *
                    PAGE_BYTES,
                PAGE_BYTES);

            uint64_t off =
                r->pack->append(
                    tmp.data(),
                    PAGE_BYTES);

            if (off >
                std::numeric_limits<
                    uint32_t>::max()) {
                return 0;
            }

            r->delta_offsets[
                page_index] =
                    static_cast<
                        uint32_t>(off);
        }

        r->metrics.dirty_pages =
            static_cast<uint64_t>(
                count);

        r->metrics.finalization_ms =
            now_ms() - t0;

        r->metrics.packed_used_bytes =
            r->pack->used();

        r->metrics.packed_committed_bytes =
            r->pack->committed();

        BOOL low = FALSE;

        if (r->low_mem) {
            QueryMemoryResourceNotification(
                r->low_mem,
                &low);
        }

        r->metrics.low_memory_signal =
            low ? 1 : 0;

        double d0 =
            now_ms();

        BOOL ok =
            VirtualFree(
                r->arena,
                r->bytes,
                MEM_DECOMMIT);

        r->metrics.decommit_ms =
            now_ms() - d0;

        if (!ok) {
            return 0;
        }

        r->state.store(
            SR_DORMANT,
            std::memory_order_release);

        sync_metric_state(r);

        return 1;

    } catch (...) {
        return 0;
    }
}

SR_API int sr_resume_core(
    SRHandle handle
) {
    Region* r =
        static_cast<Region*>(
            handle);

    if (!r ||
        !r->arena ||
        r->state.load() !=
            SR_DORMANT) {
        return 0;
    }

    double t0 =
        now_ms();

    bool ok =
        restore_range(
            r,
            0,
            r->core_bytes /
                UNIT_BYTES);

    r->metrics.core_restore_ms =
        now_ms() - t0;

    if (ok) {
        r->state.store(
            SR_CORE_RESUMED,
            std::memory_order_release);

        sync_metric_state(r);
    }

    return ok ? 1 : 0;
}

SR_API int sr_start_deep_restore(
    SRHandle handle
) {
    Region* r =
        static_cast<Region*>(
            handle);

    if (!r ||
        r->deep_thread ||
        r->state.load() !=
            SR_CORE_RESUMED) {
        return 0;
    }

    r->deep_done.store(0);
    r->deep_ok.store(0);

    r->state.store(
        SR_DEEP_RESTORING,
        std::memory_order_release);

    sync_metric_state(r);

    r->deep_thread =
        CreateThread(
            nullptr,
            0,
            deep_thread_proc,
            r,
            0,
            nullptr);

    if (!r->deep_thread) {
        r->state.store(
            SR_CORE_RESUMED,
            std::memory_order_release);

        sync_metric_state(r);

        return 0;
    }

    return 1;
}

SR_API int sr_deep_done(
    SRHandle handle
) {
    Region* r =
        static_cast<Region*>(
            handle);

    if (!r) return -1;

    return r->deep_done.load(
        std::memory_order_acquire);
}

SR_API int sr_wait_deep(
    SRHandle handle,
    uint32_t timeout_ms
) {
    Region* r =
        static_cast<Region*>(
            handle);

    if (!r ||
        !r->deep_thread) {
        return 0;
    }

    DWORD w =
        WaitForSingleObject(
            r->deep_thread,
            timeout_ms);

    if (w != WAIT_OBJECT_0) {
        return 0;
    }

    CloseHandle(
        r->deep_thread);

    r->deep_thread = nullptr;

    bool ok =
        r->deep_ok.load(
            std::memory_order_acquire) != 0;

    if (ok) {
        r->state.store(
            SR_ACTIVE_CAPSULE,
            std::memory_order_release);

        sync_metric_state(r);
    }

    return ok ? 1 : 0;
}

SR_API int sr_release_capsule(
    SRHandle handle
) {
    Region* r =
        static_cast<Region*>(
            handle);

    if (!r ||
        r->deep_thread) {
        return 0;
    }

    uint32_t state =
        r->state.load(
            std::memory_order_acquire);

    if (state !=
            SR_ACTIVE_CAPSULE &&
        state !=
            SR_ACTIVE_BASELINE) {
        return 0;
    }

    double t0 =
        now_ms();

    r->pack.reset();

    std::fill(
        r->refs.begin(),
        r->refs.end(),
        UnitRef{});

    std::fill(
        r->delta_offsets.begin(),
        r->delta_offsets.end(),
        NO_DELTA);

    r->metrics.packed_used_bytes = 0;
    r->metrics.packed_committed_bytes = 0;
    r->metrics.capsule_releases += 1;
    r->metrics.capsule_release_ms =
        now_ms() - t0;

    r->state.store(
        SR_ACTIVE_NO_CAPSULE,
        std::memory_order_release);

    sync_metric_state(r);

    return 1;
}

SR_API int sr_get_metrics(
    SRHandle handle,
    SRMetrics* out
) {
    Region* r =
        static_cast<Region*>(
            handle);

    if (!r || !out) {
        return 0;
    }

    sync_metric_state(r);

    *out = r->metrics;

    return 1;
}

SR_API void sr_destroy(
    SRHandle handle
) {
    Region* r =
        static_cast<Region*>(
            handle);

    if (!r) return;

    if (r->deep_thread) {
        WaitForSingleObject(
            r->deep_thread,
            INFINITE);

        CloseHandle(
            r->deep_thread);

        r->deep_thread = nullptr;
    }

    if (r->low_mem) {
        CloseHandle(
            r->low_mem);
    }

    if (r->decompressor) {
        CloseDecompressor(
            r->decompressor);
    }

    if (r->compressor) {
        CloseCompressor(
            r->compressor);
    }

    if (r->arena) {
        VirtualFree(
            r->arena,
            0,
            MEM_RELEASE);
    }

    delete r;
}
