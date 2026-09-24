#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #ifdef STATERAM_SDK_BUILD
    #define SR_API extern "C" __declspec(dllexport)
  #else
    #define SR_API extern "C"
  #endif
#else
  #define SR_API extern "C"
#endif

enum SRState : uint32_t {
    SR_ACTIVE_NO_CAPSULE = 0,
    SR_ACTIVE_BASELINE   = 1,
    SR_DORMANT           = 2,
    SR_CORE_RESUMED      = 3,
    SR_DEEP_RESTORING    = 4,
    SR_ACTIVE_CAPSULE    = 5,
    SR_BASELINE_BUILDING  = 6
};

struct SRMetrics {
    uint64_t arena_bytes;
    uint64_t core_bytes;

    uint64_t baseline_payload_bytes;
    uint64_t packed_used_bytes;
    uint64_t packed_committed_bytes;
    uint64_t dirty_pages;

    uint64_t baseline_epochs;
    uint64_t baseline_attempts;
    uint64_t baseline_cancellations;
    uint64_t baseline_pressure_aborts;
    uint64_t baseline_slices;
    uint64_t baseline_cpu_backoffs;
    uint64_t capsule_releases;
    uint64_t capsule_rearms;
    uint64_t deep_slices;
    uint64_t deep_yields;
    uint64_t deep_pressure_yields;

    double baseline_ms;
    double baseline_wall_ms;
    double baseline_work_ms;
    double finalization_ms;
    double decommit_ms;
    double core_restore_ms;
    double deep_restore_ms;
    double capsule_release_ms;

    int low_memory_signal;
    int deep_restore_ok;
    int background_mode_entered;
    int deep_background_mode_entered;
    uint32_t lifecycle_state;
};

typedef void* SRHandle;

SR_API uint32_t sr_api_version();
SR_API SRHandle sr_create(uint64_t bytes, uint64_t core_bytes);
SR_API void* sr_data(SRHandle handle);

SR_API int sr_begin_background_baseline(
    SRHandle handle,
    uint32_t units_per_slice,
    uint32_t base_sleep_ms);
SR_API int sr_baseline_done(SRHandle handle);
SR_API uint32_t sr_baseline_progress_permille(SRHandle handle);
SR_API int sr_wait_baseline(SRHandle handle, uint32_t timeout_ms);
SR_API int sr_cancel_background_baseline(SRHandle handle);

/* Compatibility helper: builds one baseline and waits for it. */
SR_API int sr_checkpoint_baseline(SRHandle handle);

SR_API int sr_enter_dormant(SRHandle handle);
SR_API int sr_resume_core(SRHandle handle);
SR_API int sr_start_deep_restore(SRHandle handle);
SR_API int sr_deep_done(SRHandle handle);
SR_API int sr_wait_deep(SRHandle handle, uint32_t timeout_ms);

/* Releases a prepared or obsolete capsule while the region is fully active. */
SR_API int sr_release_capsule(SRHandle handle);

/* Keep an existing restored capsule and begin tracking fresh writes so
   the same state can be made dormant again without rebuilding baseline. */
SR_API int sr_rearm_existing_capsule(SRHandle handle);

SR_API int sr_get_metrics(SRHandle handle, SRMetrics* out);
SR_API void sr_destroy(SRHandle handle);
