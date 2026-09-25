#ifdef _WIN32
#define _WIN32_WINNT 0x0602
#define NOMINMAX
#include <windows.h>

#include "stateram_runtime_client.h"
#include "stateram_runtime_protocol.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

struct RuntimeClientImpl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    uint32_t pid = 0;
    uint32_t flags = 0;
};

static bool write_all(HANDLE h, const void* data, DWORD bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    DWORD remaining = bytes;

    while (remaining > 0) {
        DWORD wrote = 0;
        if (!WriteFile(h, p, remaining, &wrote, nullptr) || wrote == 0) {
            return false;
        }
        p += wrote;
        remaining -= wrote;
    }
    return true;
}

static bool read_all(HANDLE h, void* data, DWORD bytes) {
    uint8_t* p = static_cast<uint8_t*>(data);
    DWORD remaining = bytes;

    while (remaining > 0) {
        DWORD got = 0;
        if (!ReadFile(h, p, remaining, &got, nullptr) || got == 0) {
            return false;
        }
        p += got;
        remaining -= got;
    }
    return true;
}

SRR_API int srr_connect(
    const char* client_name,
    uint32_t flags,
    SRRuntimeClient* out_client
) {
    if (!client_name || !out_client) return -1;
    *out_client = nullptr;

    if (!WaitNamedPipeW(srr::PIPE_NAME, 5000)) {
        return -2;
    }

    HANDLE pipe = CreateFileW(
        srr::PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        return -3;
    }

    auto client = std::make_unique<RuntimeClientImpl>();
    client->pipe = pipe;
    client->pid = GetCurrentProcessId();
    client->flags = flags;

    srr::RegisterMessage reg{};
    reg.pid = client->pid;
    reg.flags = flags;
    std::strncpy(reg.name, client_name, sizeof(reg.name) - 1);

    if (!write_all(pipe, &reg, sizeof(reg))) {
        CloseHandle(pipe);
        return -4;
    }

    *out_client = client.release();
    return 0;
}

SRR_API int srr_sync(
    SRRuntimeClient client_handle,
    SRContext context,
    uint32_t flags,
    SRRuntimePolicy* out_policy
) {
    auto* client = static_cast<RuntimeClientImpl*>(client_handle);
    if (!client || client->pipe == INVALID_HANDLE_VALUE || !context) {
        return -1;
    }

    SRStats stats{};
    if (sr_get_stats(context, &stats) != SR_OK) {
        return -2;
    }

    srr::StatusMessage status{};
    status.pid = client->pid;
    status.logical_bytes = stats.logical_bytes;
    status.raw_bytes = stats.raw_resident_bytes;
    status.pinned_bytes = stats.pinned_bytes;
    status.overlay_bytes = stats.overlay_bytes;
    status.client_flags = flags;

    if (!write_all(client->pipe, &status, sizeof(status))) {
        return -3;
    }

    srr::PolicyMessage policy{};
    if (!read_all(client->pipe, &policy, sizeof(policy))) {
        return -4;
    }

    if (policy.magic != srr::MAGIC ||
        policy.version != srr::VERSION ||
        policy.type != srr::MSG_POLICY) {
        return -5;
    }

    if (stats.raw_resident_bytes > policy.recommended_raw_target) {
        (void)sr_trim(context, policy.recommended_raw_target);
    }

    if (out_policy) {
        out_policy->recommended_raw_target = policy.recommended_raw_target;
        out_policy->system_available_bytes = policy.system_available_bytes;
        out_policy->memory_load_percent = policy.memory_load_percent;
        out_policy->pressure_level = policy.pressure_level;
        out_policy->foreground = policy.foreground;
        out_policy->connected_clients = policy.connected_clients;
    }

    return 0;
}

SRR_API void srr_disconnect(SRRuntimeClient client_handle) {
    auto* client = static_cast<RuntimeClientImpl*>(client_handle);
    if (!client) return;

    if (client->pipe != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(client->pipe);
        CloseHandle(client->pipe);
        client->pipe = INVALID_HANDLE_VALUE;
    }

    delete client;
}

#else

#include "stateram_runtime_client.h"

SRR_API int srr_connect(const char*, uint32_t, SRRuntimeClient*) { return -1; }
SRR_API int srr_sync(SRRuntimeClient, SRContext, uint32_t, SRRuntimePolicy*) { return -1; }
SRR_API void srr_disconnect(SRRuntimeClient) {}

#endif
