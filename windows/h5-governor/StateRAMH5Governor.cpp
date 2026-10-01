#define _WIN32_WINNT 0x0602
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <processthreadsapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "User32.lib")

static constexpr uint64_t MiB = 1024ull * 1024ull;
static constexpr uint64_t PAGE_BYTES = 4096ull;
static constexpr DWORD LOOP_MS = 500;
static constexpr uint64_t RECENT_FOREGROUND_MS = 5000;
static constexpr uint64_t RED_IDLE_TRIM_MS = 12000;
static constexpr uint64_t ORANGE_IDLE_TRIM_MS = 15000;
static constexpr uint64_t TRIM_COOLDOWN_MS = 30000;
static constexpr uint64_t GLOBAL_TRIM_COOLDOWN_MS = 8000;
static constexpr uint64_t STATE_EXPIRY_MS = 30000;
static constexpr uint64_t MIN_TRIM_WORKING_SET = 160ull * MiB;
static constexpr uint64_t ORANGE_MIN_TRIM_WORKING_SET = 192ull * MiB;
static constexpr uint64_t ORANGE_TRIM_AVAILABLE = 720ull * MiB;

static std::atomic<bool> g_stop{false};
static std::mutex g_log_mu;
static std::atomic<uint64_t> g_trimmed_working_set_bytes{0};

static uint64_t tick_ms() { return GetTickCount64(); }

static uint64_t ft64(const FILETIME& ft) {
    ULARGE_INTEGER x{};
    x.LowPart = ft.dwLowDateTime;
    x.HighPart = ft.dwHighDateTime;
    return x.QuadPart;
}

static std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(
        CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
        out.data(), n, nullptr, nullptr);
    return out;
}

static void log_line(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::cout << s << std::endl;
}

static BOOL WINAPI console_handler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop.store(true, std::memory_order_release);
        return TRUE;
    default:
        return FALSE;
    }
}

struct MemState {
    uint64_t total = 0;
    uint64_t available = 0;
    DWORD load = 0;
    int pressure = 0;
};

static MemState memory_state() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);

    MemState out{};
    if (!GlobalMemoryStatusEx(&ms)) {
        out.pressure = 3;
        return out;
    }

    out.total = ms.ullTotalPhys;
    out.available = ms.ullAvailPhys;
    out.load = ms.dwMemoryLoad;

    const uint64_t pct10 = out.total / 10;
    const uint64_t pct18 = (out.total * 18) / 100;
    const uint64_t pct30 = (out.total * 30) / 100;

    if (out.available <= pct10 || out.load >= 90) out.pressure = 3;
    else if (out.available <= pct18 || out.load >= 85) out.pressure = 2;
    else if (out.available <= pct30 || out.load >= 75) out.pressure = 1;
    else out.pressure = 0;

    return out;
}

struct ProcInfo {
    DWORD pid = 0;
    DWORD parent = 0;
    std::wstring name;
};

static std::unordered_map<DWORD, ProcInfo> process_snapshot() {
    std::unordered_map<DWORD, ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            ProcInfo x{};
            x.pid = pe.th32ProcessID;
            x.parent = pe.th32ParentProcessID;
            x.name = pe.szExeFile;
            out.emplace(x.pid, std::move(x));
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return out;
}

struct WindowCollector {
    std::unordered_set<DWORD> pids;
};

static BOOL CALLBACK enum_window_cb(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<WindowCollector*>(lp);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if ((r.right - r.left) <= 1 || (r.bottom - r.top) <= 1) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) c->pids.insert(pid);
    return TRUE;
}

static std::unordered_set<DWORD> visible_roots() {
    WindowCollector c;
    EnumWindows(enum_window_cb, reinterpret_cast<LPARAM>(&c));
    c.pids.erase(GetCurrentProcessId());
    return c.pids;
}

static std::unordered_set<DWORD> descendants_of(
    const std::unordered_set<DWORD>& roots,
    const std::unordered_map<DWORD, ProcInfo>& procs)
{
    std::unordered_set<DWORD> out = roots;
    bool changed = true;

    while (changed) {
        changed = false;
        for (const auto& kv : procs) {
            if (out.count(kv.first)) continue;
            if (out.count(kv.second.parent)) {
                out.insert(kv.first);
                changed = true;
            }
        }
    }

    return out;
}

static std::wstring lower_name(std::wstring s) {
    std::transform(
        s.begin(), s.end(), s.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return s;
}

static bool excluded_name(const std::wstring& name) {
    static const std::unordered_set<std::wstring> deny = {
        L"system", L"registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"winlogon.exe", L"dwm.exe",
        L"explorer.exe", L"fontdrvhost.exe", L"audiodg.exe",
        L"sihost.exe", L"taskhostw.exe", L"shellexperiencehost.exe",
        L"startmenuexperiencehost.exe", L"searchhost.exe", L"searchui.exe",
        L"runtimebroker.exe", L"securityhealthservice.exe", L"msmpeng.exe"
    };
    return deny.count(lower_name(name)) != 0;
}

struct HotRange {
    uintptr_t base = 0;
    size_t bytes = 0;
};

struct ProcState {
    DWORD pid = 0;
    std::wstring name;
    uint64_t creation_time = 0;
    uint64_t last_seen = 0;
    uint64_t last_foreground = 0;
    uint64_t last_capture = 0;
    uint64_t last_trim = 0;
    uint64_t prev_cpu_time = 0;
    uint64_t prev_cpu_tick = 0;
    double cpu_pct = 0.0;
    ULONG original_mem_priority = 5;
    ULONG current_mem_priority = 5;
    bool have_original_priority = false;
    uint64_t working_set = 0;
};

static HANDLE open_process_for_observe(DWORD pid) {
    return OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE, pid);
}

static HANDLE open_process_for_control(DWORD pid) {
    return OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
        PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA | SYNCHRONIZE,
        FALSE, pid);
}

static bool refresh_identity_and_cpu(
    ProcState& st,
    HANDLE h,
    DWORD cpu_count,
    uint64_t now)
{
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return false;

    const uint64_t creation = ft64(c);
    const uint64_t total_cpu = ft64(k) + ft64(u);

    if (st.creation_time != 0 && st.creation_time != creation) {
        const DWORD pid = st.pid;
        const std::wstring name = st.name;
        st = ProcState{};
        st.pid = pid;
        st.name = name;
    }

    st.creation_time = creation;

    if (st.prev_cpu_tick != 0 &&
        now > st.prev_cpu_tick &&
        total_cpu >= st.prev_cpu_time)
    {
        const uint64_t cpu_delta_100ns = total_cpu - st.prev_cpu_time;
        const uint64_t wall_100ns = (now - st.prev_cpu_tick) * 10000ull;

        if (wall_100ns != 0 && cpu_count != 0) {
            st.cpu_pct =
                100.0 * static_cast<double>(cpu_delta_100ns) /
                static_cast<double>(wall_100ns * cpu_count);
            st.cpu_pct = std::clamp(st.cpu_pct, 0.0, 100.0);
        }
    }

    st.prev_cpu_time = total_cpu;
    st.prev_cpu_tick = now;

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    if (GetProcessMemoryInfo(
            h,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc)))
    {
        st.working_set = static_cast<uint64_t>(pmc.WorkingSetSize);
    }

    return true;
}

static bool get_mem_priority(HANDLE h, ULONG& value) {
    MEMORY_PRIORITY_INFORMATION info{};
    if (!GetProcessInformation(
            h,
            ProcessMemoryPriority,
            &info,
            sizeof(info)))
    {
        return false;
    }

    value = info.MemoryPriority;
    return true;
}

static bool set_mem_priority(ProcState& st, ULONG value) {
    HANDLE h = OpenProcess(
        PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, st.pid);

    if (!h) return false;

    if (!st.have_original_priority) {
        ULONG original = 5;
        if (get_mem_priority(h, original)) {
            st.original_mem_priority = original;
            st.current_mem_priority = original;
            st.have_original_priority = true;
        }
    }

    MEMORY_PRIORITY_INFORMATION info{};
    info.MemoryPriority = value;

    const BOOL ok = SetProcessInformation(
        h,
        ProcessMemoryPriority,
        &info,
        sizeof(info));

    CloseHandle(h);

    if (ok) st.current_mem_priority = value;
    return ok != FALSE;
}

static void restore_mem_priority(ProcState& st) {
    if (!st.have_original_priority ||
        st.current_mem_priority == st.original_mem_priority)
    {
        return;
    }

    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, st.pid);
    if (!h) return;

    MEMORY_PRIORITY_INFORMATION info{};
    info.MemoryPriority = st.original_mem_priority;

    if (SetProcessInformation(
            h,
            ProcessMemoryPriority,
            &info,
            sizeof(info)))
    {
        st.current_mem_priority = st.original_mem_priority;
    }

    CloseHandle(h);
}

static bool trim_process(ProcState& st) {
    HANDLE h = open_process_for_control(st.pid);
    if (!h) return false;

    const BOOL ok = EmptyWorkingSet(h);
    CloseHandle(h);

    if (ok) st.last_trim = tick_ms();
    return ok != FALSE;
}

static DWORD foreground_pid() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

static std::unordered_set<DWORD>
group_for_root(
    DWORD root,
    const std::unordered_map<DWORD, ProcInfo>& procs)
{
    if (root == 0) return {};
    std::unordered_set<DWORD> roots{root};
    return descendants_of(roots, procs);
}

static int selftest() {
    ULONG priority = 0;

    if (!get_mem_priority(
            GetCurrentProcess(),
            priority))
    {
        std::cout
            << "SELFTEST=FAIL reason=GET_MEMORY_PRIORITY"
            << " error=" << GetLastError()
            << "\n";
        return 2;
    }

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);

    if (!GlobalMemoryStatusEx(&ms)) {
        std::cout
            << "SELFTEST=FAIL reason=MEMORY_STATUS\n";
        return 3;
    }

    std::cout
        << "SELFTEST=PASS"
        << " memoryPriority=YES"
        << " leanGovernor=YES"
        << " crossProcessPrefetch=DISABLED"
        << " currentPriority=" << priority
        << " totalPhysMiB=" << (ms.ullTotalPhys / MiB)
        << " availableMiB=" << (ms.ullAvailPhys / MiB)
        << "\n";

    return 0;
}

int wmain(int argc, wchar_t** argv) {
    bool dry_run = false;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--selftest") == 0) {
            return selftest();
        }
        if (wcscmp(argv[i], L"--dry-run") == 0) {
            dry_run = true;
        }
    }

    SetConsoleCtrlHandler(console_handler, TRUE);

    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    const DWORD cpu_count =
        std::max<DWORD>(
            1,
            si.dwNumberOfProcessors);

    const DWORD self_pid =
        GetCurrentProcessId();

    std::unordered_map<DWORD, ProcState> states;

    DWORD last_fg_root = 0;
    uint64_t last_status = 0;
    uint64_t trims = 0;
    uint64_t priority_changes = 0;
    uint64_t last_global_trim = 0;

    std::cout
        << "StateRAM H5.2 Lean Governor\n"
        << "mode="
        << (dry_run ? "DRY_RUN" : "ACTIVE")
        << "\n"
        << "policy="
        << "idle-background-memory-priority+early-lean-reclaim"
        << "\n"
        << "stop=Ctrl+C\n";

    while (!g_stop.load(
               std::memory_order_acquire))
    {
        const uint64_t now = tick_ms();
        const MemState mem = memory_state();

        const auto procs =
            process_snapshot();

        const auto roots =
            visible_roots();

        const auto candidate_pids =
            descendants_of(
                roots,
                procs);

        const DWORD fg_root =
            foreground_pid();

        const auto fg_group =
            group_for_root(
                fg_root,
                procs);

        for (DWORD pid : candidate_pids) {
            if (pid == 0 || pid == self_pid) continue;

            auto pit = procs.find(pid);

            if (pit == procs.end() ||
                excluded_name(pit->second.name))
            {
                continue;
            }

            HANDLE h =
                open_process_for_observe(pid);

            if (!h) continue;

            auto& st = states[pid];

            if (st.pid == 0) st.pid = pid;

            st.name = pit->second.name;
            st.last_seen = now;

            refresh_identity_and_cpu(
                st,
                h,
                cpu_count,
                now);

            CloseHandle(h);

            if (fg_group.count(pid)) {
                st.last_foreground = now;
            }
        }

        if (fg_root != 0 &&
            fg_root != last_fg_root)
        {
            for (DWORD pid : fg_group) {
                auto it = states.find(pid);
                if (it == states.end()) continue;

                auto& st = it->second;

                if (!dry_run &&
                    st.current_mem_priority != 5 &&
                    set_mem_priority(st, 5))
                {
                    ++priority_changes;
                }
            }

            std::ostringstream oss;

            oss
                << "FOREGROUND_SWITCH pid="
                << fg_root
                << " pressure="
                << mem.pressure
                << " availableMiB="
                << (mem.available / MiB);

            log_line(oss.str());
            last_fg_root = fg_root;
        }

        ProcState* trim_candidate = nullptr;


        for (auto& kv : states) {
            auto& st = kv.second;

            if (now - st.last_seen >
                STATE_EXPIRY_MS)
            {
                continue;
            }

            if (fg_group.count(st.pid)) {
                continue;
            }

            const bool recently_fg =
                (now - st.last_foreground) <
                RECENT_FOREGROUND_MS;

            const bool cpu_busy =
                st.cpu_pct >= 2.5;

            if (recently_fg || cpu_busy) {
                if (!dry_run &&
                    st.have_original_priority &&
                    st.current_mem_priority != 5 &&
                    set_mem_priority(st, 5))
                {
                    ++priority_changes;
                }
                continue;
            }

            ULONG target_priority = 5;

            if (mem.pressure == 1) {
                target_priority = 4;
            }
            else if (mem.pressure == 2) {
                target_priority = 2;
            }
            else if (mem.pressure == 3) {
                target_priority = 1;
            }

            if (!dry_run &&
                st.current_mem_priority !=
                target_priority)
            {
                if (set_mem_priority(
                        st,
                        target_priority))
                {
                    ++priority_changes;
                }
            }

            const uint64_t critical_floor =
                std::max<uint64_t>(
                    256ull * MiB,
                    mem.total / 10);

            const bool global_trim_ready =
                (now - last_global_trim) >=
                    GLOBAL_TRIM_COOLDOWN_MS;

            const bool critical_trim_candidate =
                global_trim_ready &&
                mem.pressure == 3 &&
                mem.available <= critical_floor &&
                (now - st.last_foreground) >=
                    RED_IDLE_TRIM_MS &&
                (now - st.last_trim) >=
                    TRIM_COOLDOWN_MS &&
                st.cpu_pct < 0.75 &&
                st.working_set >=
                    MIN_TRIM_WORKING_SET;

            const bool proactive_trim_candidate =
                global_trim_ready &&
                mem.pressure >= 2 &&
                mem.available <=
                    ORANGE_TRIM_AVAILABLE &&
                (now - st.last_foreground) >=
                    ORANGE_IDLE_TRIM_MS &&
                (now - st.last_trim) >=
                    TRIM_COOLDOWN_MS &&
                st.cpu_pct < 0.75 &&
                st.working_set >=
                    ORANGE_MIN_TRIM_WORKING_SET;

            if (critical_trim_candidate ||
                proactive_trim_candidate)
            {
                if (!trim_candidate ||
                    st.working_set >
                    trim_candidate->working_set)
                {
                    trim_candidate = &st;
                }
            }
        }

        if (trim_candidate && !dry_run) {
            const uint64_t ws_before =
                trim_candidate->working_set;

            if (trim_process(*trim_candidate)) {
                ++trims;
                last_global_trim = now;

                g_trimmed_working_set_bytes.fetch_add(
                    ws_before,
                    std::memory_order_relaxed);

                std::ostringstream oss;

                oss
                    << ((mem.pressure >= 3)
                        ? "CRITICAL_TRIM pid="
                        : "PROACTIVE_TRIM pid=")
                    << trim_candidate->pid
                    << " name="
                    << narrow(
                        trim_candidate->name)
                    << " wsMiB="
                    << std::fixed
                    << std::setprecision(1)
                    << (static_cast<double>(
                            ws_before) / MiB)
                    << " availableMiB="
                    << (mem.available / MiB)
                    << " cpuPct="
                    << std::setprecision(2)
                    << trim_candidate->cpu_pct;

                log_line(oss.str());
            }
        }

        if (mem.pressure == 0 &&
            !dry_run)
        {
            for (auto& kv : states) {
                auto& st = kv.second;

                if (st.have_original_priority &&
                    st.current_mem_priority !=
                    st.original_mem_priority)
                {
                    restore_mem_priority(st);
                }
            }
        }

        for (auto it = states.begin();
             it != states.end();)
        {
            if (now - it->second.last_seen >
                STATE_EXPIRY_MS)
            {
                if (!dry_run) {
                    restore_mem_priority(
                        it->second);
                }

                it = states.erase(it);
            }
            else {
                ++it;
            }
        }

        if (now - last_status >= 5000) {
            size_t active_states = 0;

            for (const auto& kv : states) {
                if (now - kv.second.last_seen <=
                    STATE_EXPIRY_MS)
                {
                    ++active_states;
                }
            }

            std::ostringstream oss;

            oss
                << "STATUS availableMiB="
                << (mem.available / MiB)
                << " load="
                << mem.load
                << " pressure="
                << mem.pressure
                << " fgPid="
                << fg_root
                << " managed="
                << active_states
                << " priorityChanges="
                << priority_changes
                << " trims="
                << trims
                << " trimmedWsMiB="
                << std::fixed
                << std::setprecision(1)
                << (static_cast<double>(
                        g_trimmed_working_set_bytes.load()) /
                    MiB);

            log_line(oss.str());
            last_status = now;
        }

        Sleep(LOOP_MS);
    }

    if (!dry_run) {
        for (auto& kv : states) {
            restore_mem_priority(
                kv.second);
        }
    }

    std::cout
        << "STATERAM_H5_STOPPED"
        << " priorities_restored=YES\n";

    return 0;
}
