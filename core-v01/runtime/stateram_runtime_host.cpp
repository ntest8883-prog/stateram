#ifdef _WIN32
#define _WIN32_WINNT 0x0602
#define NOMINMAX
#include <windows.h>

#include "stateram_runtime_protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

static constexpr uint64_t MB = 1024ull * 1024ull;
static constexpr uint64_t EMERGENCY_RESERVE = 640ull * MB;

/*
 * Test-only pressure override used by disposable CI browser validation.
 * Normal runs leave this at -1 and always use real Windows memory pressure.
 */
static int g_test_pressure_override = -1;

struct ClientState {
    uint32_t pid = 0;
    uint32_t flags = 0;
    std::string name;
    uint64_t logical_bytes = 0;
    uint64_t raw_bytes = 0;
    uint64_t pinned_bytes = 0;
    uint64_t overlay_bytes = 0;
    uint64_t last_seen_tick = 0;
};

struct HostState {
    std::mutex mu;
    std::unordered_map<uint32_t, ClientState> clients;
    std::atomic<bool> stop{false};
};

static uint64_t tick_ms() {
    return GetTickCount64();
}

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

static uint32_t foreground_pid() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return static_cast<uint32_t>(pid);
}

struct SystemState {
    uint64_t available = 0;
    uint32_t load = 0;
    uint32_t pressure = 0;
    uint64_t global_raw_budget = 128ull * MB;
};

static SystemState system_state() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);

    SystemState out{};
    if (!GlobalMemoryStatusEx(&ms)) {
        out.pressure = 3;
        return out;
    }

    out.available = ms.ullAvailPhys;
    out.load = ms.dwMemoryLoad;

    if (ms.ullAvailPhys <= 640ull * MB || ms.dwMemoryLoad >= 88) {
        out.pressure = 3;
    } else if (ms.ullAvailPhys <= 1024ull * MB || ms.dwMemoryLoad >= 80) {
        out.pressure = 2;
    } else if (ms.ullAvailPhys <= 1536ull * MB || ms.dwMemoryLoad >= 72) {
        out.pressure = 1;
    } else {
        out.pressure = 0;
    }

    uint64_t headroom_budget = 0;
    if (ms.ullAvailPhys > EMERGENCY_RESERVE) {
        headroom_budget = ms.ullAvailPhys - EMERGENCY_RESERVE;
    }

    if (g_test_pressure_override >= 0) {
        out.pressure = static_cast<uint32_t>(
            std::clamp(g_test_pressure_override, 0, 3));
    }

    switch (out.pressure) {
        case 3:
            out.global_raw_budget = 128ull * MB;
            break;
        case 2:
            out.global_raw_budget = 256ull * MB;
            break;
        case 1:
            out.global_raw_budget = std::min<uint64_t>(
                512ull * MB,
                std::max<uint64_t>(128ull * MB, headroom_budget));
            break;
        default:
            out.global_raw_budget = std::min<uint64_t>(
                768ull * MB,
                std::max<uint64_t>(256ull * MB, headroom_budget / 2));
            break;
    }

    return out;
}

static size_t active_client_count_locked(
    const HostState& host,
    uint64_t now
) {
    size_t n = 0;
    for (const auto& kv : host.clients) {
        if (now - kv.second.last_seen_tick <= 5000) {
            ++n;
        }
    }
    return std::max<size_t>(1, n);
}

static bool any_interactive_locked(
    const HostState& host,
    uint32_t os_foreground,
    uint64_t now
) {
    for (const auto& kv : host.clients) {
        const auto& c = kv.second;
        if (now - c.last_seen_tick > 5000) continue;

        if (c.pid == os_foreground ||
            (c.flags & srr::CLIENT_INTERACTIVE_HINT) != 0) {
            return true;
        }
    }
    return false;
}

static srr::PolicyMessage make_policy(
    HostState& host,
    const srr::StatusMessage& status
) {
    const SystemState sys = system_state();
    const uint32_t os_fg = foreground_pid();
    const uint64_t now = tick_ms();

    srr::PolicyMessage out{};
    out.pid = status.pid;
    out.system_available_bytes = sys.available;
    out.memory_load_percent = sys.load;
    out.pressure_level = sys.pressure;

    std::lock_guard<std::mutex> g(host.mu);

    auto& client = host.clients[status.pid];
    client.pid = status.pid;
    client.flags = status.client_flags;
    client.logical_bytes = status.logical_bytes;
    client.raw_bytes = status.raw_bytes;
    client.pinned_bytes = status.pinned_bytes;
    client.overlay_bytes = status.overlay_bytes;
    client.last_seen_tick = now;

    const size_t n = active_client_count_locked(host, now);
    out.connected_clients = static_cast<uint32_t>(n);

    const bool interactive =
        status.pid == os_fg ||
        (status.client_flags & srr::CLIENT_INTERACTIVE_HINT) != 0;

    out.foreground = interactive ? 1u : 0u;

    if (n <= 1) {
        out.recommended_raw_target = sys.global_raw_budget;
        return out;
    }

    const bool have_interactive =
        any_interactive_locked(host, os_fg, now);

    if (!have_interactive) {
        out.recommended_raw_target =
            std::max<uint64_t>(
                32ull * MB,
                sys.global_raw_budget / n);
        return out;
    }

    if (interactive) {
        out.recommended_raw_target =
            std::max<uint64_t>(
                128ull * MB,
                (sys.global_raw_budget * 60ull) / 100ull);
    } else {
        const uint64_t remainder =
            (sys.global_raw_budget * 40ull) / 100ull;

        const size_t background_count =
            std::max<size_t>(1, n - 1);

        out.recommended_raw_target =
            std::max<uint64_t>(
                32ull * MB,
                remainder / background_count);
    }

    return out;
}

static void client_thread(
    HostState* host,
    HANDLE pipe
) {
    srr::RegisterMessage reg{};

    if (!read_all(pipe, &reg, sizeof(reg)) ||
        reg.magic != srr::MAGIC ||
        reg.version != srr::VERSION ||
        reg.type != srr::MSG_REGISTER ||
        reg.pid == 0) {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }

    {
        std::lock_guard<std::mutex> g(host->mu);
        ClientState c{};
        c.pid = reg.pid;
        c.flags = reg.flags;
        c.name = reg.name;
        c.last_seen_tick = tick_ms();
        host->clients[reg.pid] = std::move(c);
    }

    std::cout
        << "client_connected pid=" << reg.pid
        << " name=" << reg.name
        << "\n";

    while (!host->stop.load(std::memory_order_acquire)) {
        srr::StatusMessage status{};

        if (!read_all(pipe, &status, sizeof(status))) {
            break;
        }

        if (status.magic != srr::MAGIC ||
            status.version != srr::VERSION ||
            status.type != srr::MSG_STATUS ||
            status.pid != reg.pid) {
            break;
        }

        const srr::PolicyMessage policy =
            make_policy(*host, status);

        if (!write_all(pipe, &policy, sizeof(policy))) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> g(host->mu);
        host->clients.erase(reg.pid);
    }

    std::cout
        << "client_disconnected pid=" << reg.pid
        << "\n";

    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

static HANDLE create_pipe() {
    return CreateNamedPipeW(
        srr::PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        4096,
        4096,
        0,
        nullptr);
}

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i] ? argv[i] : L"";

        const std::wstring prefix = L"--test-pressure=";
        if (arg.rfind(prefix, 0) == 0) {
            const std::wstring value = arg.substr(prefix.size());
            if (value.size() != 1 ||
                value[0] < L'0' ||
                value[0] > L'3') {
                std::wcerr
                    << L"Invalid --test-pressure value; expected 0..3\n";
                return 3;
            }

            g_test_pressure_override =
                static_cast<int>(value[0] - L'0');
        } else {
            std::wcerr
                << L"Unknown argument: " << arg << L"\n";
            return 3;
        }
    }

    std::wcout
        << L"StateRAM Runtime Host v0.2\n"
        << L"Per-user coordination runtime. No driver. No injection.\n";

    if (g_test_pressure_override >= 0) {
        std::wcout
            << L"TEST_ONLY_PRESSURE_OVERRIDE="
            << g_test_pressure_override
            << L"\n";
    }

    std::wcout
        << L"Waiting for StateRAM-enabled applications...\n";

    HostState host;

    for (;;) {
        HANDLE pipe = create_pipe();

        if (pipe == INVALID_HANDLE_VALUE) {
            std::cerr
                << "CreateNamedPipe failed: "
                << GetLastError()
                << "\n";
            return 2;
        }

        const BOOL connected =
            ConnectNamedPipe(pipe, nullptr)
                ? TRUE
                : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        std::thread(
            client_thread,
            &host,
            pipe).detach();
    }
}

#else
int main() { return 1; }
#endif
