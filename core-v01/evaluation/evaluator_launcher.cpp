#ifdef _WIN32
#define _WIN32_WINNT 0x0602
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static constexpr uint64_t MB = 1024ull * 1024ull;
static constexpr uint64_t PROCESS_COMMIT_CAP = 1280ull * MB;
static constexpr uint64_t START_MIN_AVAILABLE = 1024ull * MB;
static constexpr uint64_t ABORT_MIN_AVAILABLE = 640ull * MB;
static constexpr DWORD START_MAX_LOAD = 80;
static constexpr DWORD ABORT_LOAD = 88;
static constexpr ULONGLONG TIMEOUT_MS = 20ull * 60ull * 1000ull;

static uint64_t ft64(const FILETIME& ft) {
    ULARGE_INTEGER x{};
    x.LowPart = ft.dwLowDateTime;
    x.HighPart = ft.dwHighDateTime;
    return x.QuadPart;
}

static bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return true;

    TOKEN_ELEVATION e{};
    DWORD bytes = 0;
    const BOOL ok = GetTokenInformation(
        token,
        TokenElevation,
        &e,
        sizeof(e),
        &bytes);

    CloseHandle(token);
    return !ok || e.TokenIsElevated != 0;
}

static bool mem_status(MEMORYSTATUSEX& ms) {
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) != FALSE;
}

static std::wstring exe_dir() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(
        nullptr,
        buf.data(),
        static_cast<DWORD>(buf.size()));

    if (n == 0 || n >= buf.size()) return L".";

    std::wstring path(buf.data(), n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos
        ? L"."
        : path.substr(0, slash);
}

struct CpuSample {
    bool init = false;
    uint64_t proc100ns = 0;
    ULONGLONG tick = 0;
};

static double child_cpu_pct(
    HANDLE process,
    CpuSample& s,
    DWORD logical
) {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(
            process,
            &create,
            &exit,
            &kernel,
            &user)) {
        return -1.0;
    }

    const uint64_t proc =
        ft64(kernel) + ft64(user);
    const ULONGLONG tick = GetTickCount64();

    if (!s.init) {
        s.init = true;
        s.proc100ns = proc;
        s.tick = tick;
        return 0.0;
    }

    const uint64_t dp = proc - s.proc100ns;
    const ULONGLONG dt = tick - s.tick;

    s.proc100ns = proc;
    s.tick = tick;

    if (dt == 0 || logical == 0) return 0.0;

    const double proc_ms =
        static_cast<double>(dp) / 10000.0;

    return 100.0 * proc_ms /
        (static_cast<double>(dt) * logical);
}

struct SysSample {
    bool init = false;
    uint64_t idle = 0;
    uint64_t kernel = 0;
    uint64_t user = 0;
};

static double system_busy(SysSample& s) {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return -1.0;

    const uint64_t ni = ft64(idle);
    const uint64_t nk = ft64(kernel);
    const uint64_t nu = ft64(user);

    if (!s.init) {
        s.init = true;
        s.idle = ni;
        s.kernel = nk;
        s.user = nu;
        return 0.0;
    }

    const uint64_t di = ni - s.idle;
    const uint64_t dk = nk - s.kernel;
    const uint64_t du = nu - s.user;

    s.idle = ni;
    s.kernel = nk;
    s.user = nu;

    const uint64_t total = dk + du;
    if (total == 0 || di > total) return 0.0;

    return 100.0 *
        static_cast<double>(total - di) /
        static_cast<double>(total);
}

static void write_guard(
    const char* status,
    const char* reason,
    double start_mb,
    double min_mb,
    uint64_t peak_job_mb,
    double peak_child_cpu,
    double peak_system_busy,
    DWORD exit_code
) {
    std::ofstream out(
        "stateram_4to8_guard.json",
        std::ios::binary);

    out
        << "{\n"
        << "  \"status\": \"" << status << "\",\n"
        << "  \"reason\": \"" << reason << "\",\n"
        << "  \"start_available_mb\": "
        << std::fixed << std::setprecision(1)
        << start_mb << ",\n"
        << "  \"minimum_available_mb\": "
        << min_mb << ",\n"
        << "  \"peak_job_process_memory_mb\": "
        << peak_job_mb << ",\n"
        << "  \"peak_child_cpu_pct_total_machine\": "
        << peak_child_cpu << ",\n"
        << "  \"peak_system_busy_pct\": "
        << peak_system_busy << ",\n"
        << "  \"child_exit_code\": "
        << exit_code << "\n"
        << "}\n";
}

int wmain() {
    std::wcout
        << L"StateRAM Core v0.1 — guarded 4-to-8 evaluation\n\n"
        << L"This runs the agreed A/B/C evaluation once.\n"
        << L"It does not change the pagefile, registry, boot settings, drivers, or other processes.\n\n";

    if (is_elevated()) {
        std::wcout << L"SAFETY STOP: run normally, not as Administrator.\n";
        return 2;
    }

    MEMORYSTATUSEX start{};
    if (!mem_status(start)) return 3;

    const double start_mb =
        static_cast<double>(start.ullAvailPhys) / MB;

    if (start.ullAvailPhys < START_MIN_AVAILABLE ||
        start.dwMemoryLoad > START_MAX_LOAD) {
        write_guard(
            "ABORTED_BEFORE_START",
            "insufficient_memory_headroom",
            start_mb,
            start_mb,
            0,
            0.0,
            0.0,
            0);

        std::wcout
            << L"SAFETY STOP: close some applications/tabs and try again later.\n";
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
    limits.ProcessMemoryLimit =
        static_cast<SIZE_T>(PROCESS_COMMIT_CAP);

    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))) {
        CloseHandle(job);
        return 6;
    }

    const std::wstring child =
        exe_dir() + L"\\StateRAM_4to8_Evaluator.exe";

    std::wstring cmd =
        L"\"" + child + L"\" --local-evaluation";

    std::vector<wchar_t> mutable_cmd(
        cmd.begin(),
        cmd.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(
            nullptr,
            mutable_cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED | NORMAL_PRIORITY_CLASS,
            nullptr,
            nullptr,
            &si,
            &pi)) {
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

    HANDLE low_mem =
        CreateMemoryResourceNotification(
            LowMemoryResourceNotification);

    if (!low_mem) {
        TerminateJobObject(job, 91);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return 9;
    }

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const DWORD logical =
        std::max<DWORD>(
            1,
            sys.dwNumberOfProcessors);

    std::wcout
        << L"Safety envelope:\n"
        << L"  start only with >=1024 MiB available RAM and <=80% load\n"
        << L"  hard evaluator-process commit cap: 1280 MiB\n"
        << L"  abort below 640 MiB available RAM or >=88% load\n"
        << L"  sustained CPU watchdog, but no whole-process hard CPU throttle\n"
        << L"  20 minute hard timeout\n\n";

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    CpuSample child_cpu{};
    SysSample sys_cpu{};

    int child_over70 = 0;
    int system_overload = 0;

    double min_available_mb = start_mb;
    double peak_child_cpu = 0.0;
    double peak_system_busy = 0.0;

    bool abort = false;
    const char* reason = "none";
    const ULONGLONG started = GetTickCount64();

    for (;;) {
        const DWORD w =
            WaitForSingleObject(
                pi.hProcess,
                250);

        if (w == WAIT_OBJECT_0) break;

        if (w != WAIT_TIMEOUT) {
            abort = true;
            reason = "worker_wait_failure";
            break;
        }

        MEMORYSTATUSEX ms{};
        if (!mem_status(ms)) {
            abort = true;
            reason = "memory_status_failure";
            break;
        }

        min_available_mb =
            std::min(
                min_available_mb,
                static_cast<double>(ms.ullAvailPhys) / MB);

        BOOL low = FALSE;
        if (!QueryMemoryResourceNotification(
                low_mem,
                &low)) {
            abort = true;
            reason = "low_memory_monitor_failure";
            break;
        }

        if (low ||
            ms.ullAvailPhys < ABORT_MIN_AVAILABLE ||
            ms.dwMemoryLoad >= ABORT_LOAD) {
            abort = true;
            reason = "system_memory_safety_threshold";
            break;
        }

        const double child_pct =
            child_cpu_pct(
                pi.hProcess,
                child_cpu,
                logical);

        const double busy =
            system_busy(sys_cpu);

        peak_child_cpu =
            std::max(
                peak_child_cpu,
                std::max(0.0, child_pct));

        peak_system_busy =
            std::max(
                peak_system_busy,
                std::max(0.0, busy));

        child_over70 =
            child_pct > 70.0
                ? child_over70 + 1
                : 0;

        system_overload =
            busy > 95.0 && child_pct > 25.0
                ? system_overload + 1
                : 0;

        if (child_over70 >= 8) {
            abort = true;
            reason = "sustained_child_cpu_pressure";
            break;
        }

        if (system_overload >= 8) {
            abort = true;
            reason = "sustained_system_cpu_pressure";
            break;
        }

        if (GetTickCount64() - started >= TIMEOUT_MS) {
            abort = true;
            reason = "timeout";
            break;
        }
    }

    if (abort) {
        std::wcout
            << L"\nSAFETY STOP triggered. Only the isolated evaluator is being stopped.\n";

        TerminateJobObject(job, 92);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD exit_code = 999;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    QueryInformationJobObject(
        job,
        JobObjectExtendedLimitInformation,
        &info,
        sizeof(info),
        nullptr);

    const uint64_t peak_job_mb =
        static_cast<uint64_t>(
            info.PeakProcessMemoryUsed / MB);

    write_guard(
        abort
            ? "ABORTED_BY_SAFETY_WATCHDOG"
            : (exit_code == 0
                ? "COMPLETED_PASS"
                : "COMPLETED_EVALUATION_FAIL"),
        reason,
        start_mb,
        min_available_mb,
        peak_job_mb,
        peak_child_cpu,
        peak_system_busy,
        exit_code);

    if (!abort && exit_code == 0) {
        std::wcout
            << L"\nEvaluation completed. Send ChatGPT these files:\n"
            << L"  stateram_4to8_evaluation.json\n"
            << L"  stateram_4to8_interactions.csv\n"
            << L"  stateram_4to8_guard.json\n";
    }

    CloseHandle(low_mem);
    CloseHandle(pi.hProcess);
    CloseHandle(job);

    return abort
        ? 50
        : static_cast<int>(exit_code);
}
#else
int main() { return 1; }
#endif
