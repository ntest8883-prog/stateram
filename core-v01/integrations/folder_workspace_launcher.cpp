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
static constexpr uint64_t PROCESS_CAP = 768ull * MB;
static constexpr uint64_t START_MIN_AVAILABLE = 1024ull * MB;
static constexpr uint64_t ABORT_MIN_AVAILABLE = 640ull * MB;
static constexpr DWORD START_MAX_LOAD = 80;
static constexpr DWORD ABORT_LOAD = 88;

static bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return true;

    TOKEN_ELEVATION e{};
    DWORD bytes = 0;
    const BOOL ok = GetTokenInformation(
        token, TokenElevation, &e, sizeof(e), &bytes);

    CloseHandle(token);
    return !ok || e.TokenIsElevated != 0;
}

static bool memory_status(MEMORYSTATUSEX& ms) {
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) != FALSE;
}

static std::wstring exe_dir() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(
        nullptr, buf.data(), static_cast<DWORD>(buf.size()));

    if (n == 0 || n >= buf.size()) return L".";

    std::wstring p(buf.data(), n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

static std::wstring quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

int wmain(int argc, wchar_t** argv) {
    std::wcout
        << L"StateRAM Folder Workspace — Safe Launcher\n\n";

    if (argc < 2) {
        std::wcout
            << L"Usage:\n"
            << L"  StateRAM_FolderWorkspace_Safe.exe <folder>\n";
        return 1;
    }

    if (elevated()) {
        std::wcout << L"SAFETY STOP: run normally, not as Administrator.\n";
        return 2;
    }

    MEMORYSTATUSEX start{};
    if (!memory_status(start)) return 3;

    if (start.ullAvailPhys < START_MIN_AVAILABLE ||
        start.dwMemoryLoad > START_MAX_LOAD) {
        std::wcout
            << L"SAFETY STOP: close some applications/tabs and try again.\n";
        return 4;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return 5;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    lim.BasicLimitInformation.ActiveProcessLimit = 1;
    lim.ProcessMemoryLimit = static_cast<SIZE_T>(PROCESS_CAP);

    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &lim,
            sizeof(lim))) {
        CloseHandle(job);
        return 6;
    }

    const std::wstring child =
        exe_dir() + L"\\StateRAM_FolderWorkspace.exe";

    std::wstring cmd =
        quote(child) + L" " + quote(argv[1]);

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
            CREATE_SUSPENDED | CREATE_NEW_CONSOLE,
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

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::wcout
        << L"Workspace started inside a 768 MiB process-memory safety envelope.\n"
        << L"Closing this launcher also closes the workspace.\n";

    bool abort = false;

    for (;;) {
        const DWORD w = WaitForSingleObject(pi.hProcess, 500);

        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_TIMEOUT) {
            abort = true;
            break;
        }

        MEMORYSTATUSEX ms{};
        BOOL low = FALSE;

        if (!memory_status(ms) ||
            !QueryMemoryResourceNotification(low_mem, &low) ||
            low ||
            ms.ullAvailPhys < ABORT_MIN_AVAILABLE ||
            ms.dwMemoryLoad >= ABORT_LOAD) {
            abort = true;
            break;
        }
    }

    if (abort) {
        std::wcout
            << L"SAFETY STOP: system memory pressure crossed the guard threshold.\n";
        TerminateJobObject(job, 92);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(low_mem);
    CloseHandle(pi.hProcess);
    CloseHandle(job);

    return abort ? 50 : static_cast<int>(exit_code);
}
#else
int main() { return 1; }
#endif
