#define _WIN32_WINNT 0x0602
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <memoryapi.h>
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
static constexpr uint64_t HOT_CAPTURE_INTERVAL_MS = 5000;
static constexpr uint64_t RECENT_FOREGROUND_MS = 5000;
static constexpr uint64_t RED_IDLE_TRIM_MS = 20000;
static constexpr uint64_t ORANGE_IDLE_TRIM_MS = 30000;
static constexpr uint64_t TRIM_COOLDOWN_MS = 30000;
static constexpr uint64_t STATE_EXPIRY_MS = 30000;
static constexpr uint64_t PER_PROCESS_HOTSET_CAP = 128ull * MiB;
static constexpr uint64_t GROUP_PREFETCH_CAP = 256ull * MiB;
static constexpr uint64_t MIN_TRIM_WORKING_SET = 160ull * MiB;
static constexpr uint64_t ORANGE_MIN_TRIM_WORKING_SET = 256ull * MiB;
static constexpr uint64_t ORANGE_TRIM_AVAILABLE = 640ull * MiB;
static constexpr size_t PREFETCH_BATCH_RANGES = 64;

static std::atomic<bool> g_stop{false};
static std::mutex g_log_mu;
static std::atomic<bool> g_prefetch_busy{false};
static std::atomic<uint64_t> g_prefetch_total_bytes{0};
static std::atomic<uint64_t> g_prefetch_calls{0};
static std::atomic<uint64_t> g_prefetch_failures{0};

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
    std::vector<HotRange> hot;
    uint64_t hot_bytes = 0;
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

struct WsPage {
    uintptr_t addr = 0;
    bool shared = false;
};

static bool capture_hotset(ProcState& st, uint64_t cap_bytes) {
    HANDLE h = open_process_for_observe(st.pid);
    if (!h) return false;

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    if (!GetProcessMemoryInfo(
            h,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc)))
    {
        CloseHandle(h);
        return false;
    }

    size_t estimated_pages =
        static_cast<size_t>(pmc.WorkingSetSize / PAGE_BYTES) + 16384;

    estimated_pages = std::max<size_t>(estimated_pages, 16384);

    size_t buffer_bytes =
        sizeof(PSAPI_WORKING_SET_INFORMATION) +
        estimated_pages * sizeof(PSAPI_WORKING_SET_BLOCK);

    std::vector<unsigned char> buffer(buffer_bytes);

    bool ok = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (QueryWorkingSet(
                h,
                buffer.data(),
                static_cast<DWORD>(buffer.size())))
        {
            ok = true;
            break;
        }

        const DWORD err = GetLastError();
        if (err != ERROR_BAD_LENGTH &&
            err != ERROR_INSUFFICIENT_BUFFER)
        {
            break;
        }

        buffer.resize(buffer.size() * 2);
    }

    CloseHandle(h);
    if (!ok) return false;

    auto* ws =
        reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buffer.data());

    std::vector<WsPage> private_pages;
    std::vector<WsPage> shared_pages;

    private_pages.reserve(
        static_cast<size_t>(ws->NumberOfEntries));

    for (ULONG_PTR i = 0; i < ws->NumberOfEntries; ++i) {
        const auto& block = ws->WorkingSetInfo[i];

        WsPage page{};
        page.addr =
            static_cast<uintptr_t>(block.VirtualPage) *
            static_cast<uintptr_t>(PAGE_BYTES);
        page.shared = block.Shared != 0;

        if (page.shared) shared_pages.push_back(page);
        else private_pages.push_back(page);
    }

    auto by_addr = [](const WsPage& a, const WsPage& b) {
        return a.addr < b.addr;
    };

    std::sort(private_pages.begin(), private_pages.end(), by_addr);
    std::sort(shared_pages.begin(), shared_pages.end(), by_addr);

    const size_t cap_pages =
        static_cast<size_t>(cap_bytes / PAGE_BYTES);

    std::vector<uintptr_t> selected;
    selected.reserve(std::min<size_t>(
        cap_pages,
        private_pages.size() + shared_pages.size()));

    for (const auto& page : private_pages) {
        if (selected.size() >= cap_pages) break;
        selected.push_back(page.addr);
    }

    for (const auto& page : shared_pages) {
        if (selected.size() >= cap_pages) break;
        selected.push_back(page.addr);
    }

    std::sort(selected.begin(), selected.end());
    selected.erase(
        std::unique(selected.begin(), selected.end()),
        selected.end());

    std::vector<HotRange> ranges;

    if (!selected.empty()) {
        uintptr_t start = selected[0];
        uintptr_t previous = selected[0];

        for (size_t i = 1; i < selected.size(); ++i) {
            if (selected[i] == previous + PAGE_BYTES) {
                previous = selected[i];
                continue;
            }

            ranges.push_back({
                start,
                static_cast<size_t>(
                    previous - start + PAGE_BYTES)
            });

            start = previous = selected[i];
        }

        ranges.push_back({
            start,
            static_cast<size_t>(
                previous - start + PAGE_BYTES)
        });
    }

    st.hot = std::move(ranges);
    st.hot_bytes =
        static_cast<uint64_t>(selected.size()) * PAGE_BYTES;
    st.last_capture = tick_ms();

    return true;
}

static std::vector<WIN32_MEMORY_RANGE_ENTRY>
validate_prefetch_ranges(
    HANDLE h,
    const std::vector<HotRange>& hot,
    uint64_t cap_bytes,
    uint64_t& selected_bytes)
{
    std::vector<WIN32_MEMORY_RANGE_ENTRY> out;
    selected_bytes = 0;

    for (const auto& range : hot) {
        uintptr_t cursor = range.base;
        const uintptr_t end = range.base + range.bytes;

        while (cursor < end &&
               selected_bytes < cap_bytes)
        {
            MEMORY_BASIC_INFORMATION mbi{};

            if (VirtualQueryEx(
                    h,
                    reinterpret_cast<LPCVOID>(cursor),
                    &mbi,
                    sizeof(mbi)) != sizeof(mbi))
            {
                break;
            }

            const uintptr_t mbi_begin =
                reinterpret_cast<uintptr_t>(mbi.BaseAddress);

            const uintptr_t mbi_end =
                mbi_begin + mbi.RegionSize;

            const uintptr_t region_begin =
                std::max<uintptr_t>(cursor, mbi_begin);

            const uintptr_t region_end =
                std::min<uintptr_t>(end, mbi_end);

            if (region_end <= region_begin) break;

            const bool accessible =
                mbi.State == MEM_COMMIT &&
                (mbi.Protect & PAGE_GUARD) == 0 &&
                (mbi.Protect & PAGE_NOACCESS) == 0;

            if (accessible) {
                size_t bytes =
                    static_cast<size_t>(
                        region_end - region_begin);

                const uint64_t remaining =
                    cap_bytes - selected_bytes;

                if (bytes > remaining) {
                    bytes = static_cast<size_t>(
                        remaining - (remaining % PAGE_BYTES));
                }

                if (bytes >= PAGE_BYTES) {
                    WIN32_MEMORY_RANGE_ENTRY item{};
                    item.VirtualAddress =
                        reinterpret_cast<PVOID>(region_begin);
                    item.NumberOfBytes = bytes;
                    out.push_back(item);
                    selected_bytes += bytes;
                }
            }

            cursor = region_end;
        }

        if (selected_bytes >= cap_bytes) break;
    }

    return out;
}

struct PrefetchItem {
    DWORD pid = 0;
    std::wstring name;
    std::vector<HotRange> hot;
};

static void launch_prefetch(
    std::vector<PrefetchItem> items,
    uint64_t cap_bytes)
{
    if (items.empty() || cap_bytes < PAGE_BYTES) return;

    bool expected = false;
    if (!g_prefetch_busy.compare_exchange_strong(
            expected, true))
    {
        return;
    }

    std::thread(
        [items = std::move(items), cap_bytes]() mutable
        {
            const uint64_t start = tick_ms();

            uint64_t remaining = cap_bytes;
            uint64_t total_selected = 0;
            uint64_t successes = 0;
            uint64_t failures = 0;

            for (const auto& item : items) {
                if (remaining < PAGE_BYTES) break;

                HANDLE h =
                    open_process_for_observe(item.pid);

                if (!h) {
                    ++failures;

                    std::ostringstream fail;
                    fail
                        << "PREFETCH_FAIL pid=" << item.pid
                        << " name=" << narrow(item.name)
                        << " stage=OPEN_PROCESS"
                        << " error=" << GetLastError();

                    log_line(fail.str());
                    continue;
                }

                uint64_t selected = 0;

                auto ranges =
                    validate_prefetch_ranges(
                        h,
                        item.hot,
                        remaining,
                        selected);

                if (ranges.empty() || selected == 0) {
                    CloseHandle(h);
                    continue;
                }

                uint64_t process_selected = 0;
                bool any_success = false;
                bool process_failed = false;

                for (size_t base = 0;
                     base < ranges.size();
                     base += PREFETCH_BATCH_RANGES)
                {
                    const size_t count =
                        std::min<size_t>(
                            PREFETCH_BATCH_RANGES,
                            ranges.size() - base);

                    uint64_t batch_bytes = 0;

                    for (size_t j = 0; j < count; ++j) {
                        batch_bytes +=
                            ranges[base + j].NumberOfBytes;
                    }

                    SetLastError(ERROR_SUCCESS);

                    if (PrefetchVirtualMemory(
                            h,
                            static_cast<ULONG_PTR>(count),
                            ranges.data() + base,
                            0))
                    {
                        any_success = true;
                        process_selected += batch_bytes;
                    }
                    else {
                        const DWORD err = GetLastError();
                        process_failed = true;

                        std::ostringstream fail;
                        fail
                            << "PREFETCH_FAIL pid=" << item.pid
                            << " name=" << narrow(item.name)
                            << " stage=API"
                            << " error=" << err
                            << " ranges=" << count
                            << " batchMiB="
                            << std::fixed
                            << std::setprecision(1)
                            << (static_cast<double>(
                                    batch_bytes) / MiB);

                        log_line(fail.str());
                        break;
                    }
                }

                if (any_success) {
                    ++successes;
                    total_selected += process_selected;

                    if (process_selected < remaining) {
                        remaining -= process_selected;
                    }
                    else {
                        remaining = 0;
                    }
                }

                if (process_failed) {
                    ++failures;
                }

                CloseHandle(h);
            }

            g_prefetch_calls.fetch_add(
                successes,
                std::memory_order_relaxed);

            g_prefetch_failures.fetch_add(
                failures,
                std::memory_order_relaxed);

            g_prefetch_total_bytes.fetch_add(
                total_selected,
                std::memory_order_relaxed);

            std::ostringstream oss;
            oss
                << "PREFETCH_END selectedMiB="
                << std::fixed << std::setprecision(1)
                << (static_cast<double>(
                        total_selected) / MiB)
                << " successProcesses=" << successes
                << " failures=" << failures
                << " elapsedMs="
                << (tick_ms() - start);

            log_line(oss.str());

            g_prefetch_busy.store(
                false,
                std::memory_order_release);
        }).detach();
}

static bool trim_process(ProcState& st) {
    HANDLE h = open_process_for_control(st.pid);
    if (!h) return false;

    const BOOL ok = EmptyWorkingSet(h);
    CloseHandle(h);

    if (ok) st.last_trim = tick_ms();
    return ok != FALSE;
}

static uint64_t choose_prefetch_cap(
    const MemState& mem)
{
    if (mem.available < 320ull * MiB) return 0;
    if (mem.available < 600ull * MiB) return 64ull * MiB;
    if (mem.available < 1000ull * MiB) return 128ull * MiB;
    if (mem.available < 1600ull * MiB) return 192ull * MiB;
    return GROUP_PREFETCH_CAP;
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
    auto* p1 = GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"),
        "PrefetchVirtualMemory");

    auto* p2 = GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"),
        "SetProcessInformation");

    if (!p1 || !p2) {
        std::cout
            << "SELFTEST=FAIL reason=API_MISSING\n";
        return 2;
    }

    ULONG priority = 0;

    if (!get_mem_priority(
            GetCurrentProcess(),
            priority))
    {
        std::cout
            << "SELFTEST=FAIL reason=GET_MEMORY_PRIORITY"
            << " error=" << GetLastError()
            << "\n";
        return 3;
    }

    const SIZE_T test_bytes = 1ull * MiB;
    void* test_region = VirtualAlloc(
        nullptr,
        test_bytes,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);

    if (!test_region) {
        std::cout
            << "SELFTEST=FAIL reason=TEST_ALLOC"
            << " error=" << GetLastError()
            << "\n";
        return 4;
    }

    volatile unsigned char* test =
        static_cast<volatile unsigned char*>(
            test_region);

    for (SIZE_T off = 0;
         off < test_bytes;
         off += PAGE_BYTES)
    {
        test[off] =
            static_cast<unsigned char>(
                (off / PAGE_BYTES) & 0xFF);
    }

    WIN32_MEMORY_RANGE_ENTRY self_range{};
    self_range.VirtualAddress = test_region;
    self_range.NumberOfBytes = test_bytes;

    SetLastError(ERROR_SUCCESS);

    const BOOL prefetch_self_ok =
        PrefetchVirtualMemory(
            GetCurrentProcess(),
            1,
            &self_range,
            0);

    const DWORD prefetch_self_error =
        prefetch_self_ok
            ? ERROR_SUCCESS
            : GetLastError();

    VirtualFree(
        test_region,
        0,
        MEM_RELEASE);

    if (!prefetch_self_ok) {
        std::cout
            << "SELFTEST=FAIL reason=PREFETCH_SELF"
            << " error=" << prefetch_self_error
            << "\n";
        return 5;
    }

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);

    if (!GlobalMemoryStatusEx(&ms)) {
        std::cout
            << "SELFTEST=FAIL reason=MEMORY_STATUS\n";
        return 6;
    }

    std::cout
        << "SELFTEST=PASS"
        << " prefetch=YES"
        << " prefetchSelf=PASS"
        << " memoryPriority=YES"
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

    std::cout
        << "StateRAM H5.1 Adaptive Governor\n"
        << "mode="
        << (dry_run ? "DRY_RUN" : "ACTIVE")
        << "\n"
        << "policy="
        << "foreground-hotset+batched-prefetch+idle-priority+proactive-trim"
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

        if (fg_root != 0) {
            uint64_t group_captured = 0;

            for (DWORD pid : fg_group) {
                auto it = states.find(pid);
                if (it == states.end()) continue;

                auto& st = it->second;

                if (now - st.last_capture <
                    HOT_CAPTURE_INTERVAL_MS)
                {
                    continue;
                }

                const uint64_t left =
                    GROUP_PREFETCH_CAP -
                    std::min<uint64_t>(
                        GROUP_PREFETCH_CAP,
                        group_captured);

                const uint64_t cap =
                    std::min<uint64_t>(
                        PER_PROCESS_HOTSET_CAP,
                        left);

                if (cap < PAGE_BYTES) break;

                if (capture_hotset(st, cap)) {
                    group_captured +=
                        st.hot_bytes;
                }
            }
        }

        if (fg_root != 0 &&
            fg_root != last_fg_root)
        {
            std::vector<PrefetchItem> items;
            uint64_t remembered = 0;

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

                if (!st.hot.empty()) {
                    items.push_back({
                        pid,
                        st.name,
                        st.hot
                    });

                    remembered +=
                        st.hot_bytes;
                }
            }

            const uint64_t cap =
                choose_prefetch_cap(mem);

            std::ostringstream oss;

            oss
                << "FOREGROUND_SWITCH pid="
                << fg_root
                << " rememberedMiB="
                << std::fixed
                << std::setprecision(1)
                << (static_cast<double>(
                        remembered) / MiB)
                << " prefetchCapMiB="
                << (cap / MiB)
                << " pressure="
                << mem.pressure;

            log_line(oss.str());

            if (!dry_run && cap != 0) {
                launch_prefetch(
                    std::move(items),
                    cap);
            }

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

            const bool critical_trim_candidate =
                mem.pressure == 3 &&
                mem.available <= critical_floor &&
                (now - st.last_foreground) >=
                    RED_IDLE_TRIM_MS &&
                (now - st.last_trim) >=
                    TRIM_COOLDOWN_MS &&
                st.cpu_pct < 0.5 &&
                st.working_set >=
                    MIN_TRIM_WORKING_SET;

            const bool proactive_trim_candidate =
                mem.pressure == 2 &&
                mem.available <=
                    ORANGE_TRIM_AVAILABLE &&
                (now - st.last_foreground) >=
                    ORANGE_IDLE_TRIM_MS &&
                (now - st.last_trim) >=
                    TRIM_COOLDOWN_MS &&
                st.cpu_pct < 0.5 &&
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
            if (now -
                    trim_candidate->last_capture >
                2000)
            {
                capture_hotset(
                    *trim_candidate,
                    PER_PROCESS_HOTSET_CAP);
            }

            const uint64_t ws_before =
                trim_candidate->working_set;

            if (trim_process(*trim_candidate)) {
                ++trims;

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
                    << " rememberedMiB="
                    << (static_cast<double>(
                            trim_candidate->hot_bytes) /
                        MiB)
                    << " availableMiB="
                    << (mem.available / MiB);

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
            uint64_t captured = 0;
            size_t active_states = 0;

            for (const auto& kv : states) {
                if (now - kv.second.last_seen <=
                    STATE_EXPIRY_MS)
                {
                    captured +=
                        kv.second.hot_bytes;
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
                << " rememberedMiB="
                << std::fixed
                << std::setprecision(1)
                << (static_cast<double>(
                        captured) / MiB)
                << " prefetchMiB="
                << (static_cast<double>(
                        g_prefetch_total_bytes.load()) /
                    MiB)
                << " prefetchCalls="
                << g_prefetch_calls.load()
                << " prefetchFailures="
                << g_prefetch_failures.load()
                << " priorityChanges="
                << priority_changes
                << " trims="
                << trims;

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
