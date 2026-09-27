#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <vector>
#include <string>
#include <algorithm>

static const wchar_t* kPortName = L"\\StateRAMH3BPort";
static const uint32_t kProtocolVersion = 2;
static const uint32_t kCommandQuery = 1;
static const SIZE_T kMiB = 1024ull * 1024ull;
static const SIZE_T kPressureChunk = 32ull * kMiB;
static const SIZE_T kTargetMiB = 96;
static const SIZE_T kMinPressureMiB = 512;
static const SIZE_T kMaxPressureMiB = 1280;
static const SIZE_T kEmergencyAvailMiB = 256;
static const SIZE_T kCommitReserveMiB = 1024;
static const DWORD kTargetReadyWaitMs = 180000;
static const DWORD kTargetGoWaitMs = 300000;
static const DWORD kTargetVerifyWaitMs = 300000;
static const int64_t kEvidenceShadowPages = 32;
static const int64_t kEvidencePagefileWrites = 8;

#pragma pack(push, 8)
struct H3B_COMMAND
{
    uint32_t Version;
    uint32_t Command;
};

struct H3B_COUNTERS
{
    uint32_t Version;
    uint32_t Size;

    int64_t PagefileReads;
    int64_t PagefileReadBytes;
    int64_t PagefileWrites;
    int64_t PagefileWriteBytes;
    int64_t PagingFileCreates;

    int64_t ShadowWritePages;
    int64_t ShadowReadPages;
    int64_t ShadowMatches;
    int64_t ShadowMismatches;
    int64_t ShadowUntracked;
    int64_t ShadowReplacements;
    int64_t ShadowBufferUnavailable;
    int64_t ShadowUnaligned;
    int64_t ShadowHighIrqlSkips;
    int64_t ShadowTableEntries;
    int64_t ShadowTableCapacity;
    int64_t ShadowPublishSkipped;
    int64_t ShadowVerifyInvalidated;
    int64_t HistoryExpired;
    int64_t HistoryRecordDrops;
    int64_t KnownPagefiles;
    int64_t HistoryCapacity;
    int64_t PagefileTableFull;
};
#pragma pack(pop)

static_assert(sizeof(H3B_COMMAND) == 8, "H3B command ABI drift");
static_assert(sizeof(H3B_COUNTERS) == 192, "H3B counter ABI drift");

typedef HRESULT (WINAPI *PFN_FILTER_CONNECT_COMMUNICATION_PORT)(
    LPCWSTR, DWORD, LPVOID, WORD, LPSECURITY_ATTRIBUTES, HANDLE*);

typedef HRESULT (WINAPI *PFN_FILTER_SEND_MESSAGE)(
    HANDLE, LPVOID, DWORD, LPVOID, DWORD, LPDWORD);

typedef BOOL (WINAPI *PFN_SET_PROCESS_INFORMATION_LOCAL)(
    HANDLE, int, LPVOID, DWORD);

struct MEMORY_PRIORITY_INFORMATION_LOCAL
{
    ULONG MemoryPriority;
};

struct MemSnapshot
{
    uint64_t totalPhysMiB;
    uint64_t availPhysMiB;
    uint64_t availCommitMiB;
};

static uint64_t SplitMix64(uint64_t& state)
{
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void FillRegion(void* base, SIZE_T bytes, uint64_t seed)
{
    uint64_t* words = static_cast<uint64_t*>(base);
    SIZE_T count = bytes / sizeof(uint64_t);
    uint64_t state = seed;

    for (SIZE_T i = 0; i < count; ++i)
    {
        words[i] = SplitMix64(state);
    }
}

static bool VerifyRegion(const void* base, SIZE_T bytes, uint64_t seed)
{
    const uint64_t* words = static_cast<const uint64_t*>(base);
    SIZE_T count = bytes / sizeof(uint64_t);
    uint64_t state = seed;

    for (SIZE_T i = 0; i < count; ++i)
    {
        if (words[i] != SplitMix64(state))
        {
            fwprintf(stderr, L"DATA_MISMATCH word=%llu\n",
                static_cast<unsigned long long>(i));
            return false;
        }
    }

    return true;
}

static bool GetMemorySnapshot(MemSnapshot& out)
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);

    if (!GlobalMemoryStatusEx(&ms))
    {
        return false;
    }

    out.totalPhysMiB = ms.ullTotalPhys / kMiB;
    out.availPhysMiB = ms.ullAvailPhys / kMiB;
    out.availCommitMiB = ms.ullAvailPageFile / kMiB;
    return true;
}

static bool QueryH3B(H3B_COUNTERS& out)
{
    HMODULE fltlib = LoadLibraryW(L"fltlib.dll");
    if (!fltlib)
    {
        fwprintf(stderr, L"QUERY_ERROR LoadLibrary(fltlib.dll)=%lu\n", GetLastError());
        return false;
    }

    auto connect = reinterpret_cast<PFN_FILTER_CONNECT_COMMUNICATION_PORT>(
        GetProcAddress(fltlib, "FilterConnectCommunicationPort"));
    auto send = reinterpret_cast<PFN_FILTER_SEND_MESSAGE>(
        GetProcAddress(fltlib, "FilterSendMessage"));

    if (!connect || !send)
    {
        fwprintf(stderr, L"QUERY_ERROR fltlib exports missing\n");
        FreeLibrary(fltlib);
        return false;
    }

    HANDLE port = INVALID_HANDLE_VALUE;
    HRESULT hr = connect(kPortName, 0, nullptr, 0, nullptr, &port);
    if (FAILED(hr))
    {
        fwprintf(stderr, L"QUERY_ERROR connect=0x%08lX\n",
            static_cast<unsigned long>(hr));
        FreeLibrary(fltlib);
        return false;
    }

    H3B_COMMAND command = { kProtocolVersion, kCommandQuery };
    ZeroMemory(&out, sizeof(out));
    DWORD returned = 0;

    hr = send(
        port,
        &command,
        sizeof(command),
        &out,
        sizeof(out),
        &returned);

    CloseHandle(port);
    FreeLibrary(fltlib);

    if (FAILED(hr))
    {
        fwprintf(stderr, L"QUERY_ERROR send=0x%08lX\n",
            static_cast<unsigned long>(hr));
        return false;
    }

    if ((returned < sizeof(out)) ||
        (out.Version != kProtocolVersion) ||
        (out.Size != sizeof(out)))
    {
        fwprintf(stderr,
            L"QUERY_ERROR protocol returned=%lu version=%lu size=%lu expected=%zu\n",
            returned,
            static_cast<unsigned long>(out.Version),
            static_cast<unsigned long>(out.Size),
            sizeof(out));
        return false;
    }

    return true;
}

static void PrintDelta(const H3B_COUNTERS& before, const H3B_COUNTERS& after)
{
    wprintf(L"H3B_DELTA PagefileWrites=%lld PagefileReads=%lld "
            L"ShadowWritePages=%lld ShadowReadPages=%lld "
            L"ShadowMatches=%lld ShadowMismatches=%lld "
            L"ShadowUntracked=%lld HistoryExpired=%lld "
            L"HistoryRecordDrops=%lld PublishSkipped=%lld "
            L"VerifyInvalidated=%lld\n",
        after.PagefileWrites - before.PagefileWrites,
        after.PagefileReads - before.PagefileReads,
        after.ShadowWritePages - before.ShadowWritePages,
        after.ShadowReadPages - before.ShadowReadPages,
        after.ShadowMatches - before.ShadowMatches,
        after.ShadowMismatches - before.ShadowMismatches,
        after.ShadowUntracked - before.ShadowUntracked,
        after.HistoryExpired - before.HistoryExpired,
        after.HistoryRecordDrops - before.HistoryRecordDrops,
        after.ShadowPublishSkipped - before.ShadowPublishSkipped,
        after.ShadowVerifyInvalidated - before.ShadowVerifyInvalidated);
}

static std::wstring EventName(DWORD parentPid, const wchar_t* suffix)
{
    wchar_t buffer[128] = {};
    _snwprintf_s(
        buffer,
        _countof(buffer),
        _TRUNCATE,
        L"Local\\StateRAMH3BPressure_%lu_%s",
        static_cast<unsigned long>(parentPid),
        suffix);
    return buffer;
}

static int TargetMode(DWORD parentPid, SIZE_T targetMiB)
{
    std::wstring readyName = EventName(parentPid, L"ready");
    std::wstring goName = EventName(parentPid, L"go");
    std::wstring doneName = EventName(parentPid, L"done");

    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    HANDLE go = OpenEventW(SYNCHRONIZE, FALSE, goName.c_str());
    HANDLE done = OpenEventW(EVENT_MODIFY_STATE, FALSE, doneName.c_str());

    if (!ready || !go || !done)
    {
        fwprintf(stderr, L"TARGET_ERROR open events=%lu\n", GetLastError());
        if (ready) CloseHandle(ready);
        if (go) CloseHandle(go);
        if (done) CloseHandle(done);
        return 20;
    }

    const SIZE_T bytes = targetMiB * kMiB;
    const uint64_t seed = 0x535441544552414Dull ^ parentPid;

    /*
     * Set the child memory priority BEFORE committing/filling its target.
     * This avoids depending on whether a later priority change is applied
     * retroactively to pages that were already faulted in.
     */
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto setProcessInformation =
        reinterpret_cast<PFN_SET_PROCESS_INFORMATION_LOCAL>(
            GetProcAddress(kernel32, "SetProcessInformation"));

    if (setProcessInformation)
    {
        MEMORY_PRIORITY_INFORMATION_LOCAL mpi = {};
        mpi.MemoryPriority = 1;
        if (setProcessInformation(
                GetCurrentProcess(),
                0,
                &mpi,
                sizeof(mpi)))
        {
            wprintf(L"TARGET_MEMORY_PRIORITY=VERY_LOW\n");
        }
        else
        {
            wprintf(L"TARGET_MEMORY_PRIORITY=UNCHANGED error=%lu\n", GetLastError());
        }
    }
    else
    {
        wprintf(L"TARGET_MEMORY_PRIORITY=API_UNAVAILABLE\n");
    }

    void* target = VirtualAlloc(
        nullptr,
        bytes,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);

    if (!target)
    {
        fwprintf(stderr, L"TARGET_ERROR VirtualAlloc(%zu MiB)=%lu\n",
            targetMiB, GetLastError());
        CloseHandle(ready);
        CloseHandle(go);
        CloseHandle(done);
        return 21;
    }

    wprintf(L"TARGET_ALLOCATED=%zu MiB\n", targetMiB);
    FillRegion(target, bytes, seed);

    if (!SetProcessWorkingSetSize(
            GetCurrentProcess(),
            static_cast<SIZE_T>(-1),
            static_cast<SIZE_T>(-1)))
    {
        fwprintf(stderr, L"TARGET_ERROR working-set trim=%lu\n", GetLastError());
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready);
        CloseHandle(go);
        CloseHandle(done);
        return 22;
    }

    wprintf(L"TARGET_TRIMMED=YES\n");
    SetEvent(ready);

    DWORD wait = WaitForSingleObject(go, kTargetGoWaitMs);
    if (wait != WAIT_OBJECT_0)
    {
        fwprintf(stderr, L"TARGET_ERROR wait-go=%lu\n", wait);
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready);
        CloseHandle(go);
        CloseHandle(done);
        return 23;
    }

    wprintf(L"TARGET_VERIFY_BEGIN\n");
    bool valid = VerifyRegion(target, bytes, seed);
    wprintf(L"TARGET_VERIFY=%s\n", valid ? L"PASS" : L"FAIL");

    SetEvent(done);
    VirtualFree(target, 0, MEM_RELEASE);
    CloseHandle(ready);
    CloseHandle(go);
    CloseHandle(done);

    return valid ? 0 : 24;
}

static int SelfTest()
{
    if ((sizeof(H3B_COMMAND) != 8) || (sizeof(H3B_COUNTERS) != 192))
    {
        fwprintf(stderr, L"SELFTEST=FAIL ABI\n");
        return 2;
    }

    const SIZE_T bytes = 4 * kMiB;
    void* memory = VirtualAlloc(
        nullptr,
        bytes,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);

    if (!memory)
    {
        fwprintf(stderr, L"SELFTEST=FAIL allocation\n");
        return 3;
    }

    const uint64_t seed = 0x123456789ABCDEF0ull;
    FillRegion(memory, bytes, seed);

    if (!VerifyRegion(memory, bytes, seed))
    {
        VirtualFree(memory, 0, MEM_RELEASE);
        fwprintf(stderr, L"SELFTEST=FAIL verify\n");
        return 4;
    }

    static_cast<uint8_t*>(memory)[4096] ^= 0x5A;
    if (VerifyRegion(memory, bytes, seed))
    {
        VirtualFree(memory, 0, MEM_RELEASE);
        fwprintf(stderr, L"SELFTEST=FAIL corruption-detection\n");
        return 5;
    }

    FillRegion(memory, bytes, seed);
    VirtualFree(memory, 0, MEM_RELEASE);

    MemSnapshot ms = {};
    if (!GetMemorySnapshot(ms) || ms.totalPhysMiB == 0)
    {
        fwprintf(stderr, L"SELFTEST=FAIL memory-status\n");
        return 6;
    }

    wprintf(L"SELFTEST=PASS command=%zu counters=%zu totalPhys=%llu MiB\n",
        sizeof(H3B_COMMAND),
        sizeof(H3B_COUNTERS),
        static_cast<unsigned long long>(ms.totalPhysMiB));
    return 0;
}

static int ParentMode()
{
    H3B_COUNTERS before = {};
    if (!QueryH3B(before))
    {
        fwprintf(stderr, L"RESULT=ABORT reason=H3B_QUERY_FAILED\n");
        return 30;
    }

    if (before.ShadowMismatches != 0)
    {
        fwprintf(stderr,
            L"RESULT=ABORT reason=BASELINE_MISMATCHES value=%lld\n",
            before.ShadowMismatches);
        return 31;
    }

    if (before.KnownPagefiles <= 0 || before.PagingFileCreates <= 0)
    {
        fwprintf(stderr,
            L"RESULT=ABORT reason=PAGEFILES_NOT_OBSERVED known=%lld creates=%lld\n",
            before.KnownPagefiles,
            before.PagingFileCreates);
        return 32;
    }

    if (before.PagefileTableFull != 0)
    {
        fwprintf(stderr,
            L"RESULT=ABORT reason=PAGEFILE_TABLE_FULL value=%lld\n",
            before.PagefileTableFull);
        return 33;
    }

    MemSnapshot initial = {};
    if (!GetMemorySnapshot(initial))
    {
        fwprintf(stderr, L"RESULT=ABORT reason=MEMORY_STATUS_FAILED\n");
        return 34;
    }

    wprintf(L"BASELINE knownPagefiles=%lld creates=%lld shadowWrites=%lld "
            L"shadowReads=%lld matches=%lld mismatches=%lld "
            L"historyDrops=%lld\n",
        before.KnownPagefiles,
        before.PagingFileCreates,
        before.ShadowWritePages,
        before.ShadowReadPages,
        before.ShadowMatches,
        before.ShadowMismatches,
        before.HistoryRecordDrops);

    wprintf(L"MEMORY_INITIAL total=%llu MiB available=%llu MiB commitAvailable=%llu MiB\n",
        static_cast<unsigned long long>(initial.totalPhysMiB),
        static_cast<unsigned long long>(initial.availPhysMiB),
        static_cast<unsigned long long>(initial.availCommitMiB));

    if (initial.totalPhysMiB < 3000)
    {
        fwprintf(stderr,
            L"RESULT=ABORT reason=UNEXPECTED_SMALL_PHYSICAL_MEMORY total=%llu MiB\n",
            static_cast<unsigned long long>(initial.totalPhysMiB));
        return 35;
    }

    DWORD parentPid = GetCurrentProcessId();
    std::wstring readyName = EventName(parentPid, L"ready");
    std::wstring goName = EventName(parentPid, L"go");
    std::wstring doneName = EventName(parentPid, L"done");

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, goName.c_str());
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, doneName.c_str());

    if (!ready || !go || !done)
    {
        fwprintf(stderr, L"RESULT=ABORT reason=CREATE_EVENTS error=%lu\n", GetLastError());
        if (ready) CloseHandle(ready);
        if (go) CloseHandle(go);
        if (done) CloseHandle(done);
        return 36;
    }

    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, _countof(exe)))
    {
        fwprintf(stderr, L"RESULT=ABORT reason=GET_EXE error=%lu\n", GetLastError());
        CloseHandle(ready);
        CloseHandle(go);
        CloseHandle(done);
        return 37;
    }

    wchar_t commandLine[2 * MAX_PATH] = {};
    _snwprintf_s(
        commandLine,
        _countof(commandLine),
        _TRUNCATE,
        L"\"%s\" --target %lu %zu",
        exe,
        static_cast<unsigned long>(parentPid),
        kTargetMiB);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(
            nullptr,
            commandLine,
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi))
    {
        fwprintf(stderr, L"RESULT=ABORT reason=CREATE_TARGET error=%lu\n", GetLastError());
        CloseHandle(ready);
        CloseHandle(go);
        CloseHandle(done);
        return 38;
    }

    int result = 39;
    std::vector<void*> pressure;

    do
    {
        DWORD wait = WaitForSingleObject(ready, kTargetReadyWaitMs);
        if (wait != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"RESULT=ABORT reason=TARGET_READY_TIMEOUT wait=%lu\n", wait);
            break;
        }

        MemSnapshot afterTarget = {};
        if (!GetMemorySnapshot(afterTarget))
        {
            fwprintf(stderr, L"RESULT=ABORT reason=MEMORY_STATUS_AFTER_TARGET\n");
            break;
        }

        uint64_t desiredPressureMiB =
            afterTarget.availPhysMiB + 256;

        desiredPressureMiB = std::max<uint64_t>(
            desiredPressureMiB,
            kMinPressureMiB);
        desiredPressureMiB = std::min<uint64_t>(
            desiredPressureMiB,
            kMaxPressureMiB);

        if (afterTarget.availCommitMiB <= kCommitReserveMiB + 64)
        {
            fwprintf(stderr,
                L"RESULT=ABORT reason=LOW_COMMIT_HEADROOM available=%llu MiB\n",
                static_cast<unsigned long long>(afterTarget.availCommitMiB));
            break;
        }

        desiredPressureMiB = std::min<uint64_t>(
            desiredPressureMiB,
            afterTarget.availCommitMiB - kCommitReserveMiB);

        wprintf(L"PRESSURE_PLAN target=%zu MiB desiredPressure=%llu MiB "
                L"availableBeforePressure=%llu MiB\n",
            kTargetMiB,
            static_cast<unsigned long long>(desiredPressureMiB),
            static_cast<unsigned long long>(afterTarget.availPhysMiB));

        uint64_t allocatedMiB = 0;
        uint64_t blockNumber = 0;
        bool stopForEvidence = false;
        bool stopForMismatch = false;
        H3B_COUNTERS mid = before;

        /*
         * Do not blindly allocate all pressure and then inspect the verifier.
         * Check it every 64 MiB once meaningful pressure exists.  This lets us
         * trigger the target read while fresh write history is still available,
         * instead of discovering history-ring expiry after the fact.
         */
        while (allocatedMiB + (kPressureChunk / kMiB) <= desiredPressureMiB)
        {
            MemSnapshot now = {};
            if (!GetMemorySnapshot(now))
            {
                fwprintf(stderr, L"PRESSURE_STOP reason=MEMORY_STATUS\n");
                break;
            }

            if ((now.availPhysMiB <= kEmergencyAvailMiB) &&
                (allocatedMiB >= 256))
            {
                wprintf(L"PRESSURE_STOP reason=PHYSICAL_SAFETY available=%llu MiB\n",
                    static_cast<unsigned long long>(now.availPhysMiB));
                break;
            }

            if (now.availCommitMiB <= kCommitReserveMiB + (kPressureChunk / kMiB))
            {
                wprintf(L"PRESSURE_STOP reason=COMMIT_SAFETY availableCommit=%llu MiB\n",
                    static_cast<unsigned long long>(now.availCommitMiB));
                break;
            }

            void* block = VirtualAlloc(
                nullptr,
                kPressureChunk,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE);

            if (!block)
            {
                wprintf(L"PRESSURE_STOP reason=VIRTUALALLOC error=%lu\n", GetLastError());
                break;
            }

            FillRegion(
                block,
                kPressureChunk,
                0x5052455353555245ull ^ parentPid ^ blockNumber);

            pressure.push_back(block);
            allocatedMiB += kPressureChunk / kMiB;
            ++blockNumber;

            if ((allocatedMiB >= 256) &&
                ((allocatedMiB % 64) == 0))
            {
                H3B_COUNTERS probe = {};
                if (!QueryH3B(probe))
                {
                    fwprintf(stderr, L"PRESSURE_STOP reason=H3B_QUERY_FAILED\n");
                    break;
                }

                mid = probe;

                if (probe.ShadowMismatches != before.ShadowMismatches)
                {
                    stopForMismatch = true;
                    break;
                }

                const int64_t writeDelta =
                    probe.PagefileWrites - before.PagefileWrites;
                const int64_t shadowDelta =
                    probe.ShadowWritePages - before.ShadowWritePages;
                const int64_t readDelta =
                    probe.ShadowReadPages - before.ShadowReadPages;
                const int64_t matchDelta =
                    probe.ShadowMatches - before.ShadowMatches;
                const int64_t historyDropDelta =
                    probe.HistoryRecordDrops - before.HistoryRecordDrops;

                if ((readDelta > 0) && (matchDelta > 0))
                {
                    wprintf(L"PRESSURE_STOP reason=VERIFIER_ALREADY_MATCHED "
                            L"allocated=%llu MiB\n",
                        static_cast<unsigned long long>(allocatedMiB));
                    stopForEvidence = true;
                    break;
                }

                if (historyDropDelta > 0)
                {
                    wprintf(L"PRESSURE_STOP reason=HISTORY_PRESSURE "
                            L"drops=%lld allocated=%llu MiB\n",
                        historyDropDelta,
                        static_cast<unsigned long long>(allocatedMiB));
                    stopForEvidence = true;
                    break;
                }

                if ((writeDelta >= kEvidencePagefileWrites) &&
                    (shadowDelta >= kEvidenceShadowPages))
                {
                    wprintf(L"PRESSURE_STOP reason=FRESH_PAGEFILE_EVIDENCE "
                            L"writes=%lld shadowPages=%lld allocated=%llu MiB\n",
                        writeDelta,
                        shadowDelta,
                        static_cast<unsigned long long>(allocatedMiB));
                    stopForEvidence = true;
                    break;
                }
            }
        }

        MemSnapshot pressurePeak = {};
        GetMemorySnapshot(pressurePeak);

        wprintf(L"PRESSURE_ALLOCATED=%llu MiB availableAtPeak=%llu MiB\n",
            static_cast<unsigned long long>(allocatedMiB),
            static_cast<unsigned long long>(pressurePeak.availPhysMiB));

        if (stopForMismatch)
        {
            fwprintf(stderr, L"RESULT=STOP_MISMATCH phase=pressure\n");
            result = 40;
            break;
        }

        if (allocatedMiB < 256)
        {
            fwprintf(stderr,
                L"RESULT=ABORT reason=INSUFFICIENT_PRESSURE allocated=%llu MiB\n",
                static_cast<unsigned long long>(allocatedMiB));
            break;
        }

        /*
         * One short settling interval is enough.  A four-second blind wait can
         * create extra pagefile writes and evict the very history we need.
         */
        if (!stopForEvidence)
        {
            Sleep(750);
        }

        if (!QueryH3B(mid))
        {
            fwprintf(stderr, L"RESULT=ABORT reason=MID_QUERY_FAILED\n");
            break;
        }

        PrintDelta(before, mid);

        if (mid.ShadowMismatches != before.ShadowMismatches)
        {
            fwprintf(stderr,
                L"RESULT=STOP_MISMATCH phase=before-read delta=%lld\n",
                mid.ShadowMismatches - before.ShadowMismatches);
            result = 40;
            break;
        }

        if (mid.HistoryRecordDrops != before.HistoryRecordDrops)
        {
            wprintf(L"NOTICE history-ring-drops-before-read=%lld; "
                    L"triggering target read immediately\n",
                mid.HistoryRecordDrops - before.HistoryRecordDrops);
        }

        SetEvent(go);

        wait = WaitForSingleObject(done, kTargetVerifyWaitMs);
        if (wait != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"RESULT=ABORT reason=TARGET_VERIFY_TIMEOUT wait=%lu\n", wait);
            break;
        }

        WaitForSingleObject(pi.hProcess, 30000);

        DWORD childCode = STILL_ACTIVE;
        GetExitCodeProcess(pi.hProcess, &childCode);

        if (childCode != 0)
        {
            fwprintf(stderr, L"RESULT=ABORT reason=TARGET_VERIFY_FAILED code=%lu\n",
                static_cast<unsigned long>(childCode));
            result = 41;
            break;
        }

        H3B_COUNTERS after = {};
        if (!QueryH3B(after))
        {
            fwprintf(stderr, L"RESULT=ABORT reason=FINAL_QUERY_FAILED\n");
            break;
        }

        PrintDelta(before, after);

        if (after.ShadowMismatches != before.ShadowMismatches)
        {
            fwprintf(stderr,
                L"RESULT=STOP_MISMATCH delta=%lld\n",
                after.ShadowMismatches - before.ShadowMismatches);
            result = 42;
            break;
        }

        int64_t readDelta = after.ShadowReadPages - before.ShadowReadPages;
        int64_t matchDelta = after.ShadowMatches - before.ShadowMatches;

        if ((readDelta > 0) && (matchDelta > 0))
        {
            wprintf(L"RESULT=PASS shadowReadPagesDelta=%lld shadowMatchesDelta=%lld\n",
                readDelta, matchDelta);
            result = 0;
        }
        else
        {
            wprintf(L"RESULT=NO_TRACKED_READ shadowReadPagesDelta=%lld "
                    L"shadowMatchesDelta=%lld historyExpiredDelta=%lld "
                    L"historyDropsDelta=%lld\n",
                readDelta,
                matchDelta,
                after.HistoryExpired - before.HistoryExpired,
                after.HistoryRecordDrops - before.HistoryRecordDrops);
            result = 43;
        }
    }
    while (false);

    SetEvent(go);

    for (void* block : pressure)
    {
        VirtualFree(block, 0, MEM_RELEASE);
    }

    DWORD childWait = WaitForSingleObject(pi.hProcess, 5000);
    if (childWait != WAIT_OBJECT_0)
    {
        TerminateProcess(pi.hProcess, 99);
        WaitForSingleObject(pi.hProcess, 2000);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(ready);
    CloseHandle(go);
    CloseHandle(done);

    MemSnapshot finalMem = {};
    if (GetMemorySnapshot(finalMem))
    {
        wprintf(L"MEMORY_FINAL available=%llu MiB commitAvailable=%llu MiB\n",
            static_cast<unsigned long long>(finalMem.availPhysMiB),
            static_cast<unsigned long long>(finalMem.availCommitMiB));
    }

    return result;
}

int wmain(int argc, wchar_t** argv)
{
    if ((argc == 2) && (_wcsicmp(argv[1], L"--selftest") == 0))
    {
        return SelfTest();
    }

    if ((argc == 4) && (_wcsicmp(argv[1], L"--target") == 0))
    {
        wchar_t* end1 = nullptr;
        wchar_t* end2 = nullptr;

        unsigned long parentPid = wcstoul(argv[2], &end1, 10);
        unsigned long long targetMiB = _wcstoui64(argv[3], &end2, 10);

        if (!end1 || *end1 != L'\0' ||
            !end2 || *end2 != L'\0' ||
            parentPid == 0 ||
            targetMiB < 16 ||
            targetMiB > 512)
        {
            fwprintf(stderr, L"TARGET_ERROR invalid arguments\n");
            return 10;
        }

        return TargetMode(
            static_cast<DWORD>(parentPid),
            static_cast<SIZE_T>(targetMiB));
    }

    if (argc != 1)
    {
        fwprintf(stderr, L"Usage: StateRAMH3BPressure.exe [--selftest]\n");
        return 1;
    }

    return ParentMode();
}
