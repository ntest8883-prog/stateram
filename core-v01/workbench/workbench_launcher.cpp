#ifdef _WIN32
#define _WIN32_WINNT 0x0602
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

static constexpr uint64_t MB = 1024ull * 1024ull;
static constexpr uint64_t PROCESS_COMMIT_CAP = 1280ull * MB;
static constexpr uint64_t START_MIN_AVAILABLE = 1024ull * MB;
static constexpr uint64_t ABORT_MIN_AVAILABLE = 640ull * MB;
static constexpr DWORD START_MAX_MEMORY_LOAD = 80;
static constexpr DWORD ABORT_MEMORY_LOAD = 88;

static uint64_t filetime64(const FILETIME& ft) {
    ULARGE_INTEGER x{};
    x.LowPart = ft.dwLowDateTime;
    x.HighPart = ft.dwHighDateTime;
    return x.QuadPart;
}

static bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return true;
    TOKEN_ELEVATION e{};
    DWORD bytes = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &bytes);
    CloseHandle(token);
    return !ok || e.TokenIsElevated != 0;
}

static bool memory_status(MEMORYSTATUSEX& ms) {
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) != FALSE;
}

static std::wstring directory_of_this_exe() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return L".";
    std::wstring p(buf.data(), n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

struct CpuSample {
    bool initialized = false;
    uint64_t proc100ns = 0;
    ULONGLONG tick = 0;
};

static double child_cpu_pct(HANDLE process, CpuSample& s, DWORD logical) {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &create, &exit, &kernel, &user)) return -1.0;
    uint64_t nowp = filetime64(kernel) + filetime64(user);
    ULONGLONG nowt = GetTickCount64();
    if (!s.initialized) {
        s.initialized = true;
        s.proc100ns = nowp;
        s.tick = nowt;
        return 0.0;
    }
    uint64_t dp = nowp - s.proc100ns;
    ULONGLONG dt = nowt - s.tick;
    s.proc100ns = nowp;
    s.tick = nowt;
    if (dt == 0 || logical == 0) return 0.0;
    double proc_ms = static_cast<double>(dp) / 10000.0;
    return 100.0 * proc_ms / (static_cast<double>(dt) * logical);
}

struct SystemSample {
    bool initialized = false;
    uint64_t idle = 0;
    uint64_t kernel = 0;
    uint64_t user = 0;
};

static double system_busy(SystemSample& s) {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return -1.0;
    uint64_t ni = filetime64(idle);
    uint64_t nk = filetime64(kernel);
    uint64_t nu = filetime64(user);
    if (!s.initialized) {
        s.initialized = true;
        s.idle = ni;
        s.kernel = nk;
        s.user = nu;
        return 0.0;
    }
    uint64_t di = ni - s.idle;
    uint64_t dk = nk - s.kernel;
    uint64_t du = nu - s.user;
    s.idle = ni;
    s.kernel = nk;
    s.user = nu;
    uint64_t total = dk + du;
    if (total == 0 || di > total) return 0.0;
    return 100.0 * static_cast<double>(total - di) / static_cast<double>(total);
}

int wmain() {
    std::wcout << L"StateRAM Workbench Safe Launcher — Core v0.1 4→8\n\n";

    if (elevated()) {
        std::wcout << L"SAFETY STOP: do not run StateRAM as Administrator.\n";
        return 2;
    }

    MEMORYSTATUSEX start{};
    if (!memory_status(start)) return 3;

    const double start_mb = static_cast<double>(start.ullAvailPhys) / MB;
    std::wcout << L"Available RAM: " << start_mb << L" MiB\n"
               << L"Memory load:  " << start.dwMemoryLoad << L"%\n";

    if (start.ullAvailPhys < START_MIN_AVAILABLE ||
        start.dwMemoryLoad > START_MAX_MEMORY_LOAD) {
        std::wcout << L"SAFETY STOP: close some applications/tabs and try again.\n";
        return 4;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return 5;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(PROCESS_COMMIT_CAP);

    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        CloseHandle(job);
        return 6;
    }

    const std::wstring child = directory_of_this_exe() + L"\\StateRAM_Workbench.exe";
    std::wstring cmd = L"\"" + child + L"\" --guarded-child";
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &si, &pi)) {
        std::wcout << L"Could not start StateRAM_Workbench.exe\n";
        CloseHandle(job);
        return 7;
    }

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 90);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return 8;
    }

    HANDLE low_mem = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    if (!low_mem) {
        TerminateJobObject(job, 91);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return 9;
    }

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    DWORD logical = std::max<DWORD>(1, sys.dwNumberOfProcessors);

    std::wcout << L"\nSafety supervisor active:\n"
               << L"  hard child commit cap: 1280 MiB\n"
               << L"  no whole-process CPU hard throttle\n"
               << L"  abort below 640 MiB available RAM or >=88% load\n"
               << L"  abort only after sustained abnormal CPU pressure\n"
               << L"  closing this launcher also closes the isolated Workbench\n\n";

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    CpuSample child_cpu{};
    SystemSample system_cpu{};
    int child_over70 = 0;
    int system_overload = 0;

    bool safety_abort = false;
    std::wstring reason;

    for (;;) {
        DWORD w = WaitForSingleObject(pi.hProcess, 250);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_TIMEOUT) {
            safety_abort = true;
            reason = L"worker wait failure";
            break;
        }

        MEMORYSTATUSEX ms{};
        if (!memory_status(ms)) {
            safety_abort = true;
            reason = L"memory status failure";
            break;
        }

        BOOL low = FALSE;
        if (!QueryMemoryResourceNotification(low_mem, &low)) {
            safety_abort = true;
            reason = L"low-memory monitor failure";
            break;
        }

        if (low || ms.ullAvailPhys < ABORT_MIN_AVAILABLE ||
            ms.dwMemoryLoad >= ABORT_MEMORY_LOAD) {
            safety_abort = true;
            reason = L"system memory safety threshold";
            break;
        }

        const double child_pct = child_cpu_pct(pi.hProcess, child_cpu, logical);
        const double system_pct = system_busy(system_cpu);

        if (child_pct > 70.0) ++child_over70;
        else child_over70 = 0;

        if (system_pct > 95.0 && child_pct > 25.0) ++system_overload;
        else system_overload = 0;

        if (child_over70 >= 8) {
            safety_abort = true;
            reason = L"sustained child CPU pressure";
            break;
        }
        if (system_overload >= 8) {
            safety_abort = true;
            reason = L"sustained system CPU pressure";
            break;
        }
    }

    if (safety_abort) {
        std::wcout << L"\nSAFETY STOP: " << reason << L"\n"
                   << L"Stopping only the isolated StateRAM Workbench.\n";
        TerminateJobObject(job, 92);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(low_mem);
    CloseHandle(pi.hProcess);
    CloseHandle(job);

    return safety_abort ? 50 : static_cast<int>(exit_code);
}
#else
int main() { return 0; }
#endif
