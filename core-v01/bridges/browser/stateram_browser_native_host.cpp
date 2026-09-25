#ifdef _WIN32
#define _WIN32_WINNT 0x0602
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include "stateram_core.h"
#include "stateram_runtime_client.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

static constexpr uint64_t MB = 1024ull * 1024ull;
static constexpr uint32_t MAX_NATIVE_MESSAGE = 64u * 1024u;

struct BrowserStatus {
    uint32_t reclaimable = 0;
    uint64_t oldest_idle_seconds = 0;
};

struct BrowserPolicy {
    uint32_t discard = 0;
    uint64_t minimum_idle_seconds = std::numeric_limits<uint64_t>::max();
};

static bool read_exact(FILE* f, void* dst, size_t n) {
    auto* p = static_cast<unsigned char*>(dst);
    size_t done = 0;

    while (done < n) {
        const size_t got = std::fread(p + done, 1, n - done, f);
        if (got == 0) return false;
        done += got;
    }

    return true;
}

static bool write_exact(FILE* f, const void* src, size_t n) {
    const auto* p = static_cast<const unsigned char*>(src);
    size_t done = 0;

    while (done < n) {
        const size_t wrote = std::fwrite(p + done, 1, n - done, f);
        if (wrote == 0) return false;
        done += wrote;
    }

    return true;
}

static bool read_native_message(std::string& out) {
    uint32_t length = 0;

    if (!read_exact(stdin, &length, sizeof(length))) {
        return false;
    }

    if (length == 0 || length > MAX_NATIVE_MESSAGE) {
        return false;
    }

    out.resize(length);
    return read_exact(stdin, out.data(), length);
}

static bool write_native_message(const std::string& message) {
    if (message.size() > MAX_NATIVE_MESSAGE) return false;

    const uint32_t length =
        static_cast<uint32_t>(message.size());

    if (!write_exact(stdout, &length, sizeof(length))) {
        return false;
    }

    if (!write_exact(stdout, message.data(), message.size())) {
        return false;
    }

    std::fflush(stdout);
    return true;
}

static bool parse_uint64(
    const std::string& json,
    const char* key,
    uint64_t& value
) {
    const std::string needle =
        std::string("\"") + key + "\"";

    size_t p = json.find(needle);
    if (p == std::string::npos) return false;

    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;

    ++p;
    while (p < json.size() &&
           (json[p] == ' ' || json[p] == '\t' ||
            json[p] == '\r' || json[p] == '\n')) {
        ++p;
    }

    if (p >= json.size() || json[p] < '0' || json[p] > '9') {
        return false;
    }

    uint64_t out = 0;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
        const uint64_t digit =
            static_cast<uint64_t>(json[p] - '0');

        if (out >
            (std::numeric_limits<uint64_t>::max() - digit) / 10ull) {
            return false;
        }

        out = out * 10ull + digit;
        ++p;
    }

    value = out;
    return true;
}

static BrowserStatus parse_status(const std::string& json) {
    BrowserStatus s{};

    uint64_t reclaimable = 0;
    uint64_t oldest = 0;

    (void)parse_uint64(
        json,
        "reclaimable",
        reclaimable);

    (void)parse_uint64(
        json,
        "oldestIdleSeconds",
        oldest);

    s.reclaimable =
        static_cast<uint32_t>(
            std::min<uint64_t>(
                reclaimable,
                10000ull));

    s.oldest_idle_seconds = oldest;
    return s;
}

static BrowserPolicy browser_policy(
    uint32_t pressure,
    const BrowserStatus& status
) {
    BrowserPolicy out{};

    if (status.reclaimable == 0) {
        return out;
    }

    switch (pressure) {
        case 0:
            out.discard = 0;
            out.minimum_idle_seconds =
                std::numeric_limits<uint64_t>::max();
            break;

        case 1:
            out.minimum_idle_seconds = 15ull * 60ull;
            if (status.oldest_idle_seconds >= out.minimum_idle_seconds) {
                out.discard = std::min<uint32_t>(
                    1u,
                    status.reclaimable);
            }
            break;

        case 2:
            out.minimum_idle_seconds = 5ull * 60ull;
            if (status.oldest_idle_seconds >= out.minimum_idle_seconds) {
                out.discard = std::min<uint32_t>(
                    2u,
                    status.reclaimable);
            }
            break;

        default:
            out.minimum_idle_seconds = 60ull;
            if (status.oldest_idle_seconds >= out.minimum_idle_seconds) {
                out.discard = std::min<uint32_t>(
                    4u,
                    status.reclaimable);
            }
            break;
    }

    return out;
}

static int policy_selfcheck() {
    struct Case {
        uint32_t pressure;
        uint32_t reclaimable;
        uint64_t oldest;
        uint32_t expected;
    };

    const Case cases[] = {
        {0, 10, 3600, 0},
        {1, 10, 899, 0},
        {1, 10, 900, 1},
        {2, 10, 299, 0},
        {2, 10, 300, 2},
        {3, 10, 59, 0},
        {3, 10, 60, 4},
        {3, 2, 3600, 2},
        {3, 0, 3600, 0}
    };

    for (const auto& c : cases) {
        BrowserStatus s{};
        s.reclaimable = c.reclaimable;
        s.oldest_idle_seconds = c.oldest;

        const BrowserPolicy p =
            browser_policy(c.pressure, s);

        if (p.discard != c.expected) {
            std::cerr
                << "policy_selfcheck_fail pressure="
                << c.pressure
                << " expected="
                << c.expected
                << " got="
                << p.discard
                << "\n";
            return 20;
        }
    }

    std::cout
        << "STATERAM_BROWSER_BRIDGE_POLICY_SELFCHECK=PASS\n";

    return 0;
}

static std::string response_json(
    bool ok,
    const char* reason,
    const SRRuntimePolicy* runtime,
    const BrowserPolicy* browser
) {
    std::string out = "{";

    out += "\"ok\":";
    out += ok ? "true" : "false";

    out += ",\"reason\":\"";
    out += reason ? reason : "";
    out += "\"";

    if (runtime) {
        out += ",\"pressure\":";
        out += std::to_string(runtime->pressure_level);

        out += ",\"availableMb\":";
        out += std::to_string(
            runtime->system_available_bytes / MB);

        out += ",\"memoryLoad\":";
        out += std::to_string(
            runtime->memory_load_percent);
    } else {
        out += ",\"pressure\":0";
        out += ",\"availableMb\":0";
        out += ",\"memoryLoad\":0";
    }

    if (browser) {
        out += ",\"discard\":";
        out += std::to_string(browser->discard);

        out += ",\"minimumIdleSeconds\":";
        if (browser->minimum_idle_seconds ==
            std::numeric_limits<uint64_t>::max()) {
            out += "4294967295";
        } else {
            out += std::to_string(
                browser->minimum_idle_seconds);
        }
    } else {
        out += ",\"discard\":0";
        out += ",\"minimumIdleSeconds\":4294967295";
    }

    out += "}";
    return out;
}

int main(int argc, char** argv) {
    if (argc >= 2 &&
        std::strcmp(argv[1], "--policy-selfcheck") == 0) {
        return policy_selfcheck();
    }

    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    SRContextConfig cfg{};
    cfg.target_raw_bytes = 32ull * MB;
    cfg.hard_raw_bytes = 64ull * MB;
    cfg.foreground_burst_bytes = 8ull * MB;
    cfg.comfortable_available_bytes = 1024ull * MB;
    cfg.emergency_available_bytes = 640ull * MB;
    cfg.background_period_ms = 1000;
    cfg.prewarm_segments_per_hint = 0;
    cfg.overlay_consolidate_pages = 8;

    SRContext context = nullptr;
    if (sr_context_create(&cfg, &context) != SR_OK) {
        (void)write_native_message(
            response_json(
                false,
                "core_init_failed",
                nullptr,
                nullptr));
        return 2;
    }

    SRRuntimeClient runtime = nullptr;

    const int connect_rc =
        srr_connect(
            "browser-bridge",
            SRR_CLIENT_NONE,
            &runtime);

    if (connect_rc != 0) {
        (void)write_native_message(
            response_json(
                false,
                "runtime_unavailable",
                nullptr,
                nullptr));

        sr_context_destroy(context);
        return 0;
    }

    std::string message;

    while (read_native_message(message)) {
        const BrowserStatus status =
            parse_status(message);

        SRRuntimePolicy runtime_policy{};

        const int sync_rc =
            srr_sync(
                runtime,
                context,
                SRR_CLIENT_NONE,
                &runtime_policy);

        if (sync_rc != 0) {
            (void)write_native_message(
                response_json(
                    false,
                    "runtime_sync_failed",
                    nullptr,
                    nullptr));
            break;
        }

        const BrowserPolicy browser =
            browser_policy(
                runtime_policy.pressure_level,
                status);

        if (!write_native_message(
                response_json(
                    true,
                    "ok",
                    &runtime_policy,
                    &browser))) {
            break;
        }
    }

    srr_disconnect(runtime);
    sr_context_destroy(context);
    return 0;
}

#else
int main() { return 1; }
#endif
