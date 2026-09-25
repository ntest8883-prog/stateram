#pragma once
#include <cstdint>

#include "stateram_core.h"

#ifdef _WIN32
  #ifdef STATERAM_RUNTIME_CLIENT_BUILD
    #define SRR_API extern "C" __declspec(dllexport)
  #else
    #define SRR_API extern "C" __declspec(dllimport)
  #endif
#else
  #define SRR_API extern "C"
#endif

typedef void* SRRuntimeClient;

struct SRRuntimePolicy {
    uint64_t recommended_raw_target;
    uint64_t system_available_bytes;
    uint32_t memory_load_percent;
    uint32_t pressure_level;
    uint32_t foreground;
    uint32_t connected_clients;
};

enum SRRuntimeClientFlags : uint32_t {
    SRR_CLIENT_NONE = 0,
    SRR_CLIENT_INTERACTIVE_HINT = 1u << 0
};

SRR_API int srr_connect(
    const char* client_name,
    uint32_t flags,
    SRRuntimeClient* out_client);

SRR_API int srr_sync(
    SRRuntimeClient client,
    SRContext context,
    uint32_t flags,
    SRRuntimePolicy* out_policy);

SRR_API void srr_disconnect(SRRuntimeClient client);
