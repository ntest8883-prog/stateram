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

struct SRMetrics {
    uint64_t arena_bytes;
    uint64_t core_bytes;
    uint64_t baseline_payload_bytes;
    uint64_t packed_used_bytes;
    uint64_t packed_committed_bytes;
    uint64_t dirty_pages;
    uint64_t core_spin_safe; // reserved for host-side interpretation
    double baseline_ms;
    double finalization_ms;
    double decommit_ms;
    double core_restore_ms;
    double deep_restore_ms;
    int low_memory_signal;
    int deep_restore_ok;
};

typedef void* SRHandle;

SR_API uint32_t sr_api_version();
SR_API SRHandle sr_create(uint64_t bytes, uint64_t core_bytes);
SR_API void* sr_data(SRHandle handle);
SR_API int sr_checkpoint_baseline(SRHandle handle);
SR_API int sr_enter_dormant(SRHandle handle);
SR_API int sr_resume_core(SRHandle handle);
SR_API int sr_start_deep_restore(SRHandle handle);
SR_API int sr_deep_done(SRHandle handle);
SR_API int sr_wait_deep(SRHandle handle, uint32_t timeout_ms);
SR_API int sr_get_metrics(SRHandle handle, SRMetrics* out);
SR_API void sr_destroy(SRHandle handle);
