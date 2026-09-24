#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #ifdef STATERAM_CORE_BUILD
    #define SR_API extern "C" __declspec(dllexport)
  #else
    #define SR_API extern "C" __declspec(dllimport)
  #endif
#else
  #define SR_API extern "C"
#endif

/* StateRAM Core v0.1 public cooperative API.
   The application must access managed bytes through leases. */

typedef void* SRContext;
typedef void* SRLease;
typedef uint64_t SRObjectId;

enum SRResult : int32_t {
    SR_OK = 0,
    SR_ERR_INVALID = -1,
    SR_ERR_NOMEM = -2,
    SR_ERR_NOT_FOUND = -3,
    SR_ERR_BACKING = -4,
    SR_ERR_CORRUPTION = -5,
    SR_ERR_BUSY = -6,
    SR_ERR_BUDGET = -7,
    SR_ERR_INTERNAL = -8
};

enum SRSemanticClass : uint32_t {
    SR_SEM_CRITICAL_INTERACTIVE = 0,
    SR_SEM_USER_DOCUMENT = 1,
    SR_SEM_DERIVED_INDEX = 2,
    SR_SEM_HISTORY = 3,
    SR_SEM_CACHE = 4,
    SR_SEM_FILE_BACKED_ASSET = 5,
    SR_SEM_REBUILDABLE_DERIVED = 6,
    SR_SEM_SHARED_IMMUTABLE = 7
};

enum SRBackingKind : uint32_t {
    SR_BACKING_NONE = 0,
    SR_BACKING_RECIPE = 1,
    SR_BACKING_FILE = 2,
    SR_BACKING_DISCARDABLE_RECIPE = 3
};

enum SRAccessMode : uint32_t {
    SR_ACCESS_READ = 1,
    SR_ACCESS_WRITE = 2
};

enum SRLatencyClass : uint32_t {
    SR_LATENCY_INTERACTIVE = 0,
    SR_LATENCY_NORMAL = 1,
    SR_LATENCY_BACKGROUND = 2
};

enum SRRepresentation : uint32_t {
    SR_REP_RAW = 0,
    SR_REP_COMPRESSED = 1,
    SR_REP_RECIPE = 2,
    SR_REP_RECIPE_DELTA = 3,
    SR_REP_FILE_REF = 4,
    SR_REP_FILE_DELTA = 5,
    SR_REP_DISCARDABLE = 6,
    SR_REP_NONE = 7
};

enum SRObjectFlags : uint32_t {
    /* When initial_data is supplied together with a recipe/file backing,
       this asserts that the initial bytes equal the backing at generation 0. */
    SR_OBJECT_INITIAL_MATCHES_BACKING = 1u << 0,
    /* Protect the critical prefix from ordinary background dormancy unless
       the system is under emergency memory pressure. */
    SR_OBJECT_PROTECT_CRITICAL_PREFIX = 1u << 1
};

typedef int (*SRRebuildCallback)(
    SRObjectId object_id,
    uint64_t offset,
    uint64_t length,
    void* destination,
    void* user);

typedef int (*SRBackingReadCallback)(
    SRObjectId object_id,
    uint64_t offset,
    uint64_t length,
    void* destination,
    void* user);

struct SRContextConfig {
    uint64_t target_raw_bytes;
    uint64_t hard_raw_bytes;
    uint64_t foreground_burst_bytes;

    uint64_t comfortable_available_bytes;
    uint64_t emergency_available_bytes;

    uint32_t background_period_ms;
    uint32_t prewarm_segments_per_hint;
    uint32_t overlay_consolidate_pages;
    uint32_t reserved;
};

struct SRObjectDescriptor {
    const char* name;
    uint64_t logical_size;
    uint64_t critical_prefix_bytes;

    uint32_t semantic_class;
    uint32_t backing_kind;
    uint32_t flags;
    uint32_t reserved;

    SRRebuildCallback rebuild;
    SRBackingReadCallback backing_read;
    void* user;
};

struct SRStats {
    uint64_t object_count;
    uint64_t logical_bytes;

    uint64_t raw_resident_bytes;
    uint64_t compressed_bytes;
    uint64_t overlay_bytes;
    uint64_t pinned_bytes;

    uint64_t promotions;
    uint64_t promotion_failures;
    uint64_t streaming_evictions;
    uint64_t eviction_aborts_generation_changed;
    uint64_t write_generations;

    uint64_t foreground_acquires;
    uint64_t background_prewarms;
    uint64_t background_compaction_passes;
    uint64_t background_pressure_passes;

    uint64_t available_physical_bytes;
    uint32_t memory_load_percent;
    uint32_t pressure_level; /* 0 green, 1 yellow, 2 orange, 3 red */

    double last_promotion_ms;
    double max_promotion_ms;
    double last_compaction_ms;
};

struct SRObjectStats {
    SRObjectId object_id;
    uint64_t logical_size;
    uint64_t raw_resident_bytes;
    uint64_t compressed_bytes;
    uint64_t overlay_bytes;
    uint64_t generation;
    uint64_t acquire_count;
    uint64_t promotion_count;
    uint64_t eviction_count;
    uint32_t semantic_class;
    uint32_t cold_hint;
};

SR_API uint32_t sr_core_api_version();

SR_API int sr_context_create(
    const SRContextConfig* config,
    SRContext* out_context);

SR_API void sr_context_destroy(SRContext context);

SR_API int sr_register_object(
    SRContext context,
    const SRObjectDescriptor* descriptor,
    const void* initial_data,
    uint64_t initial_data_bytes,
    SRObjectId* out_object_id);

SR_API int sr_unregister_object(
    SRContext context,
    SRObjectId object_id);

/* Acquire a contiguous application view. The returned pointer remains valid
   until sr_release(). A WRITE lease is conservatively journaled at page
   granularity on release, so acknowledged writes survive later dormancy. */
SR_API int sr_acquire(
    SRContext context,
    SRObjectId object_id,
    uint64_t offset,
    uint64_t length,
    uint32_t access_mode,
    uint32_t latency_class,
    SRLease* out_lease);

SR_API void* sr_lease_data(SRLease lease);
SR_API uint64_t sr_lease_length(SRLease lease);

/* Optional hint for future optimization. Core v0.1 remains correct even if
   the application never calls this function. */
SR_API int sr_mark_dirty(
    SRLease lease,
    uint64_t relative_offset,
    uint64_t length);

SR_API int sr_release(SRLease lease);

SR_API int sr_hint_cold(
    SRContext context,
    SRObjectId object_id);

SR_API int sr_hint_hot(
    SRContext context,
    SRObjectId object_id);

SR_API int sr_hint_next(
    SRContext context,
    const SRObjectId* object_ids,
    size_t count);

/* Streaming compaction request. This does not create a second full copy of
   an object; eligible segments are encoded/published/decommitted one at a time. */
SR_API int sr_trim(
    SRContext context,
    uint64_t target_raw_bytes);

SR_API int sr_get_stats(
    SRContext context,
    SRStats* out_stats);

SR_API int sr_get_object_stats(
    SRContext context,
    SRObjectId object_id,
    SRObjectStats* out_stats);

/* Returns the current logical generation of the object. */
SR_API uint64_t sr_object_generation(
    SRContext context,
    SRObjectId object_id);

/* Debug/introspection helper. The representation reported is the dominant
   representation of the segment at the instant of the call. */
SR_API int sr_segment_representation(
    SRContext context,
    SRObjectId object_id,
    uint64_t segment_index,
    uint32_t* out_representation);
