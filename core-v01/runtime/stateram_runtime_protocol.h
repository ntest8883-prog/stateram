#pragma once
#include <cstdint>

namespace srr {

static constexpr uint32_t MAGIC = 0x53525232u; /* SRR2 */
static constexpr uint32_t VERSION = 0x00020000u;
static constexpr wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\StateRAM-v02";

enum MessageType : uint32_t {
    MSG_REGISTER = 1,
    MSG_STATUS = 2,
    MSG_POLICY = 3
};

enum ClientFlags : uint32_t {
    CLIENT_NONE = 0,
    CLIENT_INTERACTIVE_HINT = 1u << 0
};

#pragma pack(push, 1)

struct RegisterMessage {
    uint32_t magic = MAGIC;
    uint32_t version = VERSION;
    uint32_t type = MSG_REGISTER;
    uint32_t pid = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
    char name[64]{};
};

struct StatusMessage {
    uint32_t magic = MAGIC;
    uint32_t version = VERSION;
    uint32_t type = MSG_STATUS;
    uint32_t pid = 0;

    uint64_t logical_bytes = 0;
    uint64_t raw_bytes = 0;
    uint64_t pinned_bytes = 0;
    uint64_t overlay_bytes = 0;

    uint32_t client_flags = 0;
    uint32_t reserved = 0;
};

struct PolicyMessage {
    uint32_t magic = MAGIC;
    uint32_t version = VERSION;
    uint32_t type = MSG_POLICY;
    uint32_t pid = 0;

    uint64_t recommended_raw_target = 0;
    uint64_t system_available_bytes = 0;

    uint32_t memory_load_percent = 0;
    uint32_t pressure_level = 0;
    uint32_t foreground = 0;
    uint32_t connected_clients = 0;
};

#pragma pack(pop)

} // namespace srr
