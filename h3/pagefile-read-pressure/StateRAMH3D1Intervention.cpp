#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <vector>
#include <string>
#include <algorithm>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

// H3-D1 verified-payload clustered intervention harness (protocol v7).
// It arms only after the target is cold and all safety gates are rechecked.
static const wchar_t* kPortName = L"\\StateRAMH3BPort";
static const uint32_t kProtocolVersion = 7;
static const uint32_t kCommandQuery = 1;
static const uint32_t kCommandArm = 3;
static const uint32_t kCommandDisarm = 4;
static const SIZE_T kMiB = 1024ull * 1024ull;
static const SIZE_T kPressureChunk = 32ull * kMiB;
static const SIZE_T kTargetMiB = 256;
static const SIZE_T kMinPressureMiB = 512;
static const SIZE_T kMaxPressureMiB = 1280;
static const SIZE_T kEmergencyAvailMiB = 256;
static const SIZE_T kCommitReserveMiB = 1024;
static const DWORD kTargetReadyWaitMs = 180000;
static const DWORD kTargetGoWaitMs = 300000;
static const DWORD kTargetVerifyWaitMs = 300000;
static const DWORD kResidencyPollMs = 100;
static const SIZE_T kResidencySampleStridePages = 16;
static const ULONG kColdPercent = 10;
static const int64_t kEvidenceShadowPages = 16;
static const int64_t kEvidencePagefileWrites = 4;

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
    int64_t PagefileIdentities;
    int64_t PagefileAliases;
    int64_t ReadNewSystemBuffers;
    int64_t CrossObjectComparisons;
    int64_t CrossObjectMatches;
    int64_t CrossObjectMismatches;
    int64_t NewSystemBufferComparisons;
    int64_t NewSystemBufferMismatches;
    int64_t DynamicPagefileDiscoveries;
    int64_t ConcurrentOverlapSkips;
    int64_t DroppedInflightRecords;
    int64_t DroppedInflightOutstanding;

    int64_t PayloadWritePages;
    int64_t PayloadReadPages;
    int64_t PayloadMatches;
    int64_t PayloadMismatches;
    int64_t PayloadReplacements;
    int64_t PayloadLookupMisses;
    int64_t PayloadCaptureSkipped;
    int64_t PayloadBufferUnavailable;
    int64_t PayloadCapacity;

    int64_t InterventionArmed;
    int64_t InterventionEligible;
    int64_t InterventionAttempts;
    int64_t InterventionServedPages;
    int64_t InterventionFallbacks;
    int64_t InterventionGuardRejects;
    int64_t InterventionPayloadMisses;
    int64_t InterventionHashRejects;
    int64_t InterventionCapacity;

    int64_t DiagCaptured;
    int64_t DiagIdentityIndex;
    int64_t DiagReadBaseOffset;
    int64_t DiagPageOffset;
    int64_t DiagReadRequestedBytes;
    int64_t DiagReadCompletedBytes;
    int64_t DiagReadMdlBytes;
    int64_t DiagReadPageOrdinal;
    int64_t DiagReadIrpFlags;
    int64_t DiagReadOperationFlags;
    int64_t DiagReadDataFlags;
    int64_t DiagWriteSequence;
    int64_t DiagWriteIoLength;
    int64_t DiagWriteMdlBytes;
    int64_t DiagWritePageOrdinal;
    int64_t DiagWriteIrpFlags;
    int64_t DiagWriteOperationFlags;
    int64_t DiagWriteDataFlags;
    int64_t DiagExpectedHash1;
    int64_t DiagExpectedHash2;
    int64_t DiagActualHash1;
    int64_t DiagActualHash2;
    int64_t DiagWriterSameObject;
    int64_t DiagGeneration;

    int64_t TaggedWritePages;
    int64_t TaggedReadPages;
    int64_t TaggedFirstWriteOffset;
    int64_t TaggedFirstReadOffset;
    int64_t TaggedFirstWritePageIndex;
    int64_t TaggedFirstReadPageIndex;
    int64_t TaggedFirstWriteSequence;
    int64_t TaggedFirstReadIdentityIndex;
};
#pragma pack(pop)

static_assert(sizeof(H3B_COMMAND) == 8, "H3B command ABI drift");
static_assert(sizeof(H3B_COUNTERS) == 688, "H3B counter ABI drift");

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

static bool SendH3BCommand(uint32_t commandId, H3B_COUNTERS& out)
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

    H3B_COMMAND command = { kProtocolVersion, commandId };
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
        fwprintf(stderr, L"QUERY_ERROR send=0x%08lX command=%lu\n",
            static_cast<unsigned long>(hr),
            static_cast<unsigned long>(commandId));
        return false;
    }

    if ((returned < sizeof(out)) ||
        (out.Version != kProtocolVersion) ||
        (out.Size != sizeof(out)))
    {
        fwprintf(stderr,
            L"QUERY_ERROR protocol returned=%lu version=%lu size=%lu expected=%zu command=%lu\n",
            returned,
            static_cast<unsigned long>(out.Version),
            static_cast<unsigned long>(out.Size),
            sizeof(out),
            static_cast<unsigned long>(commandId));
        return false;
    }

    return true;
}

static bool QueryH3B(H3B_COUNTERS& out)
{
    return SendH3BCommand(kCommandQuery, out);
}

static void PrintDelta(const H3B_COUNTERS& before, const H3B_COUNTERS& after)
{
    wprintf(L"H3B_DELTA PagefileWrites=%lld PagefileReads=%lld "
            L"ShadowWritePages=%lld ShadowReadPages=%lld "
            L"ShadowMatches=%lld ShadowMismatches=%lld "
            L"ShadowUntracked=%lld HistoryExpired=%lld "
            L"HistoryRecordDrops=%lld PublishSkipped=%lld "
            L"VerifyInvalidated=%lld ConcurrentOverlapSkips=%lld "
            L"DroppedInflightOutstanding=%lld PagefileTableFull=%lld "
            L"PayloadWrites=%lld PayloadReads=%lld PayloadMatches=%lld "
            L"PayloadMismatches=%lld PayloadLookupMisses=%lld "
            L"PayloadCaptureSkipped=%lld PayloadBufferUnavailable=%lld "
            L"InterventionArmed=%lld InterventionEligible=%lld "
            L"InterventionAttempts=%lld InterventionServedPages=%lld\n",
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
        after.ShadowVerifyInvalidated - before.ShadowVerifyInvalidated,
        after.ConcurrentOverlapSkips - before.ConcurrentOverlapSkips,
        after.DroppedInflightOutstanding,
        after.PagefileTableFull,
        after.PayloadWritePages - before.PayloadWritePages,
        after.PayloadReadPages - before.PayloadReadPages,
        after.PayloadMatches - before.PayloadMatches,
        after.PayloadMismatches - before.PayloadMismatches,
        after.PayloadLookupMisses - before.PayloadLookupMisses,
        after.PayloadCaptureSkipped - before.PayloadCaptureSkipped,
        after.PayloadBufferUnavailable - before.PayloadBufferUnavailable,
        after.InterventionArmed,
        after.InterventionEligible,
        after.InterventionAttempts - before.InterventionAttempts,
        after.InterventionServedPages - before.InterventionServedPages);
}

static bool GetSampledResidency(
    const void* base,
    SIZE_T bytes,
    SIZE_T& residentSamples,
    SIZE_T& totalSamples)
{
    const SIZE_T pageSize = 4096;
    const SIZE_T totalPages = bytes / pageSize;
    const SIZE_T samples =
        (totalPages + kResidencySampleStridePages - 1) /
        kResidencySampleStridePages;

    if (samples == 0 || samples > (MAXDWORD / sizeof(PSAPI_WORKING_SET_EX_INFORMATION)))
    {
        return false;
    }

    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info(samples);
    SIZE_T index = 0;

    for (SIZE_T page = 0;
         page < totalPages && index < samples;
         page += kResidencySampleStridePages, ++index)
    {
        info[index].VirtualAddress =
            const_cast<BYTE*>(
                static_cast<const BYTE*>(base) + (page * pageSize));
    }

    if (!QueryWorkingSetEx(
            GetCurrentProcess(),
            info.data(),
            static_cast<DWORD>(
                info.size() * sizeof(info[0]))))
    {
        return false;
    }

    residentSamples = 0;
    totalSamples = info.size();

    for (const auto& entry : info)
    {
        if (entry.VirtualAttributes.Valid)
        {
            ++residentSamples;
        }
    }

    return true;
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
    std::wstring cold1Name = EventName(parentPid, L"cold1");
    std::wstring go1Name = EventName(parentPid, L"go1");
    std::wstring done1Name = EventName(parentPid, L"done1");
    std::wstring cold2Name = EventName(parentPid, L"cold2");
    std::wstring go2Name = EventName(parentPid, L"go2");
    std::wstring done2Name = EventName(parentPid, L"done2");

    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    HANDLE cold1 = OpenEventW(EVENT_MODIFY_STATE, FALSE, cold1Name.c_str());
    HANDLE go1 = OpenEventW(SYNCHRONIZE, FALSE, go1Name.c_str());
    HANDLE done1 = OpenEventW(EVENT_MODIFY_STATE, FALSE, done1Name.c_str());
    HANDLE cold2 = OpenEventW(EVENT_MODIFY_STATE, FALSE, cold2Name.c_str());
    HANDLE go2 = OpenEventW(SYNCHRONIZE, FALSE, go2Name.c_str());
    HANDLE done2 = OpenEventW(EVENT_MODIFY_STATE, FALSE, done2Name.c_str());

    if (!ready || !cold1 || !go1 || !done1 || !cold2 || !go2 || !done2)
    {
        fwprintf(stderr, L"TARGET_ERROR open events=%lu\n", GetLastError());
        if (ready) CloseHandle(ready);
        if (cold1) CloseHandle(cold1);
        if (go1) CloseHandle(go1);
        if (done1) CloseHandle(done1);
        if (cold2) CloseHandle(cold2);
        if (go2) CloseHandle(go2);
        if (done2) CloseHandle(done2);
        return 20;
    }

    const SIZE_T bytes = targetMiB * kMiB;
    const uint64_t seed = 0x535441544552414Dull ^ parentPid;

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
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 21;
    }

    wprintf(L"TARGET_ALLOCATED=%zu MiB\n", targetMiB);
    FillRegion(target, bytes, seed);

    if (!SetProcessWorkingSetSize(
            GetCurrentProcess(),
            static_cast<SIZE_T>(-1),
            static_cast<SIZE_T>(-1)))
    {
        fwprintf(stderr, L"TARGET_ERROR initial-working-set-trim=%lu\n", GetLastError());
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 22;
    }

    wprintf(L"TARGET_TRIMMED_PASS1=YES\n");

    if (!SetEvent(ready))
    {
        fwprintf(stderr, L"TARGET_ERROR signal-ready=%lu\n", GetLastError());
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 25;
    }

    auto waitForCold = [&](HANDLE coldEvent, const wchar_t* label) -> bool
    {
        ULONGLONG waitStart = GetTickCount64();

        for (;;)
        {
            SIZE_T residentSamples = 0;
            SIZE_T totalSamples = 0;

            if (GetSampledResidency(
                    target,
                    bytes,
                    residentSamples,
                    totalSamples))
            {
                const ULONG residentPercent =
                    totalSamples == 0 ? 100 :
                    static_cast<ULONG>(
                        (residentSamples * 100) / totalSamples);

                if (residentPercent <= kColdPercent)
                {
                    if (!SetEvent(coldEvent))
                    {
                        fwprintf(stderr,
                            L"TARGET_ERROR signal-%s-cold=%lu\n",
                            label,
                            GetLastError());
                        return false;
                    }

                    wprintf(L"TARGET_%s_COLD residentSamples=%zu totalSamples=%zu residentPercent=%lu\n",
                        label,
                        residentSamples,
                        totalSamples,
                        static_cast<unsigned long>(residentPercent));
                    return true;
                }
            }

            if ((GetTickCount64() - waitStart) >= kTargetGoWaitMs)
            {
                fwprintf(stderr, L"TARGET_ERROR %s-cold=TIMEOUT\n", label);
                return false;
            }

            Sleep(kResidencyPollMs);
        }
    };

    if (!waitForCold(cold1, L"PASS1"))
    {
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 23;
    }

    if (WaitForSingleObject(go1, kTargetGoWaitMs) != WAIT_OBJECT_0)
    {
        fwprintf(stderr, L"TARGET_ERROR wait-go1\n");
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 23;
    }

    wprintf(L"TARGET_VERIFY_PASS1_BEGIN\n");
    bool valid1 = VerifyRegion(target, bytes, seed);
    wprintf(L"TARGET_VERIFY_PASS1=%s\n", valid1 ? L"PASS" : L"FAIL");

    if (!valid1)
    {
        SetEvent(done1);
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 24;
    }

    if (!SetEvent(done1))
    {
        fwprintf(stderr, L"TARGET_ERROR signal-done1=%lu\n", GetLastError());
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 26;
    }

    /*
     * Keep the bytes unchanged. Re-trim the exact same target so the second
     * access faults the same logical pages after their pagefile payloads have
     * been naturally read and verified during pass 1.
     */
    if (!SetProcessWorkingSetSize(
            GetCurrentProcess(),
            static_cast<SIZE_T>(-1),
            static_cast<SIZE_T>(-1)))
    {
        fwprintf(stderr, L"TARGET_ERROR second-working-set-trim=%lu\n", GetLastError());
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 28;
    }

    wprintf(L"TARGET_RETRIMMED_PASS2=YES\n");

    if (!waitForCold(cold2, L"PASS2"))
    {
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 29;
    }

    if (WaitForSingleObject(go2, kTargetGoWaitMs) != WAIT_OBJECT_0)
    {
        fwprintf(stderr, L"TARGET_ERROR wait-go2\n");
        VirtualFree(target, 0, MEM_RELEASE);
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 30;
    }

    wprintf(L"TARGET_VERIFY_PASS2_BEGIN\n");
    bool valid2 = VerifyRegion(target, bytes, seed);
    wprintf(L"TARGET_VERIFY_PASS2=%s\n", valid2 ? L"PASS" : L"FAIL");

    if (!SetEvent(done2))
    {
        fwprintf(stderr, L"TARGET_ERROR signal-done2=%lu\n", GetLastError());
        valid2 = false;
    }

    VirtualFree(target, 0, MEM_RELEASE);
    CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
    CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);

    return valid2 ? 0 : 31;
}

static int SelfTest()
{
    if ((sizeof(H3B_COMMAND) != 8) || (sizeof(H3B_COUNTERS) != 688))
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

static int MachinePreflight()
{
    H3B_COUNTERS counters = {};
    if (!QueryH3B(counters))
    {
        fwprintf(stderr, L"PREFLIGHT=FAIL reason=H3B_QUERY\n");
        return 70;
    }

    if ((counters.InterventionArmed != 0) ||
        (counters.InterventionServedPages != 0))
    {
        fwprintf(stderr,
            L"PREFLIGHT=FAIL reason=INTERVENTION_NOT_PASSIVE armed=%lld served=%lld\n",
            counters.InterventionArmed,
            counters.InterventionServedPages);
        return 79;
    }

    if (counters.PayloadMismatches != 0)
    {
        fwprintf(stderr,
            L"PREFLIGHT=FAIL reason=EXISTING_PAYLOAD_MISMATCHES payload=%lld shadowDiagnostic=%lld\n",
            counters.PayloadMismatches,
            counters.ShadowMismatches);
        return 71;
    }

    if ((counters.KnownPagefiles <= 0) ||
        (counters.PagefileIdentities <= 0) ||
        (counters.PagingFileCreates <= 0) ||
        (counters.PagefileTableFull != 0) ||
        (counters.DroppedInflightOutstanding != 0))
    {
        fwprintf(stderr,
            L"PREFLIGHT=FAIL reason=PAGEFILE_STATE objects=%lld identities=%lld "
            L"creates=%lld tableFull=%lld droppedInflightOutstanding=%lld\n",
            counters.KnownPagefiles,
            counters.PagefileIdentities,
            counters.PagingFileCreates,
            counters.PagefileTableFull,
            counters.DroppedInflightOutstanding);
        return 72;
    }

    MemSnapshot memory = {};
    if (!GetMemorySnapshot(memory))
    {
        fwprintf(stderr, L"PREFLIGHT=FAIL reason=MEMORY_STATUS\n");
        return 73;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    bool hasMemoryPriority =
        (GetProcAddress(kernel32, "SetProcessInformation") != nullptr);

    const SIZE_T bytes = 8 * kMiB;
    const uint64_t seed = 0x505245464C494748ull;
    void* sample = VirtualAlloc(
        nullptr,
        bytes,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);

    if (!sample)
    {
        fwprintf(stderr, L"PREFLIGHT=FAIL reason=ALLOC error=%lu\n", GetLastError());
        return 74;
    }

    FillRegion(sample, bytes, seed);

    if (!SetProcessWorkingSetSize(
            GetCurrentProcess(),
            static_cast<SIZE_T>(-1),
            static_cast<SIZE_T>(-1)))
    {
        DWORD error = GetLastError();
        VirtualFree(sample, 0, MEM_RELEASE);
        fwprintf(stderr, L"PREFLIGHT=FAIL reason=WORKING_SET_TRIM error=%lu\n", error);
        return 75;
    }

    bool valid = VerifyRegion(sample, bytes, seed);
    VirtualFree(sample, 0, MEM_RELEASE);

    if (!valid)
    {
        fwprintf(stderr, L"PREFLIGHT=FAIL reason=DATA_VERIFY\n");
        return 76;
    }

    H3B_COUNTERS after = {};
    if (!QueryH3B(after))
    {
        fwprintf(stderr, L"PREFLIGHT=FAIL reason=FINAL_H3B_QUERY\n");
        return 77;
    }

    if (after.PayloadMismatches != counters.PayloadMismatches)
    {
        fwprintf(stderr,
            L"PREFLIGHT=FAIL reason=NEW_PAYLOAD_MISMATCH payloadDelta=%lld shadowDiagnosticDelta=%lld\n",
            after.PayloadMismatches - counters.PayloadMismatches,
            after.ShadowMismatches - counters.ShadowMismatches);
        return 78;
    }

    wprintf(L"PREFLIGHT=PASS totalPhys=%llu MiB available=%llu MiB "
            L"commitAvailable=%llu MiB memoryPriorityApi=%s "
            L"pagefileObjects=%lld identities=%lld historyDrops=%lld "
            L"droppedInflightOutstanding=%lld\n",
        static_cast<unsigned long long>(memory.totalPhysMiB),
        static_cast<unsigned long long>(memory.availPhysMiB),
        static_cast<unsigned long long>(memory.availCommitMiB),
        hasMemoryPriority ? L"YES" : L"NO",
        after.KnownPagefiles,
        after.PagefileIdentities,
        after.HistoryRecordDrops,
        after.DroppedInflightOutstanding);

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

    if (before.PayloadMismatches != 0)
    {
        fwprintf(stderr,
            L"RESULT=ABORT reason=BASELINE_PAYLOAD_MISMATCHES payload=%lld shadowDiagnostic=%lld\n",
            before.PayloadMismatches,
            before.ShadowMismatches);
        return 31;
    }

    if ((before.KnownPagefiles <= 0) ||
        (before.PagefileIdentities <= 0) ||
        (before.PagingFileCreates <= 0) ||
        (before.PagefileTableFull != 0) ||
        (before.DroppedInflightOutstanding != 0))
    {
        fwprintf(stderr,
            L"RESULT=ABORT reason=PAGEFILE_STATE objects=%lld identities=%lld creates=%lld "
            L"tableFull=%lld droppedInflightOutstanding=%lld\n",
            before.KnownPagefiles,
            before.PagefileIdentities,
            before.PagingFileCreates,
            before.PagefileTableFull,
            before.DroppedInflightOutstanding);
        return 32;
    }

    /*
     * Pass 1 must be completely natural. Disarm explicitly even if an earlier
     * run left the verifier eligible; we want pass 1 to create target-specific
     * verified payloads, not attempt intervention.
     */
    H3B_COUNTERS passive = {};
    if (!SendH3BCommand(kCommandDisarm, passive))
    {
        fwprintf(stderr, L"RESULT=ABORT reason=DISARM_BEFORE_PASS1_FAILED\n");
        return 33;
    }

    if (passive.InterventionArmed != 0)
    {
        fwprintf(stderr, L"RESULT=ABORT reason=DISARM_BEFORE_PASS1_NOT_PASSIVE\n");
        return 34;
    }

    before = passive;

    MemSnapshot initial = {};
    if (!GetMemorySnapshot(initial))
    {
        fwprintf(stderr, L"RESULT=ABORT reason=MEMORY_STATUS_FAILED\n");
        return 35;
    }

    wprintf(L"BASELINE pagefileObjects=%lld identities=%lld creates=%lld payloadWrites=%lld "
            L"payloadReads=%lld payloadMatches=%lld payloadMismatches=%lld\n",
        before.KnownPagefiles,
        before.PagefileIdentities,
        before.PagingFileCreates,
        before.PayloadWritePages,
        before.PayloadReadPages,
        before.PayloadMatches,
        before.PayloadMismatches);

    wprintf(L"MEMORY_INITIAL total=%llu MiB available=%llu MiB commitAvailable=%llu MiB\n",
        static_cast<unsigned long long>(initial.totalPhysMiB),
        static_cast<unsigned long long>(initial.availPhysMiB),
        static_cast<unsigned long long>(initial.availCommitMiB));

    if (initial.totalPhysMiB < 3000)
    {
        fwprintf(stderr,
            L"RESULT=ABORT reason=UNEXPECTED_SMALL_PHYSICAL_MEMORY total=%llu MiB\n",
            static_cast<unsigned long long>(initial.totalPhysMiB));
        return 36;
    }

    DWORD parentPid = GetCurrentProcessId();
    std::wstring readyName = EventName(parentPid, L"ready");
    std::wstring cold1Name = EventName(parentPid, L"cold1");
    std::wstring go1Name = EventName(parentPid, L"go1");
    std::wstring done1Name = EventName(parentPid, L"done1");
    std::wstring cold2Name = EventName(parentPid, L"cold2");
    std::wstring go2Name = EventName(parentPid, L"go2");
    std::wstring done2Name = EventName(parentPid, L"done2");

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE cold1 = CreateEventW(nullptr, TRUE, FALSE, cold1Name.c_str());
    HANDLE go1 = CreateEventW(nullptr, TRUE, FALSE, go1Name.c_str());
    HANDLE done1 = CreateEventW(nullptr, TRUE, FALSE, done1Name.c_str());
    HANDLE cold2 = CreateEventW(nullptr, TRUE, FALSE, cold2Name.c_str());
    HANDLE go2 = CreateEventW(nullptr, TRUE, FALSE, go2Name.c_str());
    HANDLE done2 = CreateEventW(nullptr, TRUE, FALSE, done2Name.c_str());

    if (!ready || !cold1 || !go1 || !done1 || !cold2 || !go2 || !done2)
    {
        fwprintf(stderr, L"RESULT=ABORT reason=CREATE_EVENTS error=%lu\n", GetLastError());
        if (ready) CloseHandle(ready);
        if (cold1) CloseHandle(cold1);
        if (go1) CloseHandle(go1);
        if (done1) CloseHandle(done1);
        if (cold2) CloseHandle(cold2);
        if (go2) CloseHandle(go2);
        if (done2) CloseHandle(done2);
        return 37;
    }

    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, _countof(exe)))
    {
        fwprintf(stderr, L"RESULT=ABORT reason=GET_EXE error=%lu\n", GetLastError());
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 38;
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
        CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
        CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);
        return 39;
    }

    int result = 53;
    std::vector<void*> pressure;

    do
    {
        HANDLE readyWaitHandles[2] = { ready, pi.hProcess };
        DWORD wait = WaitForMultipleObjects(
            2,
            readyWaitHandles,
            FALSE,
            kTargetReadyWaitMs);

        if (wait != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"RESULT=ABORT reason=TARGET_READY wait=%lu\n", wait);
            break;
        }

        MemSnapshot afterTarget = {};
        if (!GetMemorySnapshot(afterTarget))
        {
            fwprintf(stderr, L"RESULT=ABORT reason=MEMORY_STATUS_AFTER_TARGET\n");
            break;
        }

        uint64_t desiredPressureMiB = afterTarget.availPhysMiB + 256;
        desiredPressureMiB = std::max<uint64_t>(desiredPressureMiB, kMinPressureMiB);
        desiredPressureMiB = std::min<uint64_t>(desiredPressureMiB, kMaxPressureMiB);

        if (afterTarget.availCommitMiB <= kCommitReserveMiB + 64)
        {
            fwprintf(stderr, L"RESULT=ABORT reason=LOW_COMMIT_HEADROOM available=%llu MiB\n",
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
        bool targetCold1 = false;

        while (allocatedMiB + (kPressureChunk / kMiB) <= desiredPressureMiB)
        {
            MemSnapshot now = {};
            if (!GetMemorySnapshot(now))
            {
                wprintf(L"PRESSURE_STOP reason=MEMORY_STATUS\n");
                break;
            }

            targetCold1 = (WaitForSingleObject(cold1, 0) == WAIT_OBJECT_0);

            if (targetCold1 && allocatedMiB >= 512)
            {
                H3B_COUNTERS probe = {};
                if (QueryH3B(probe))
                {
                    const int64_t writeDelta =
                        probe.PagefileWrites - before.PagefileWrites;
                    const int64_t payloadWriteDelta =
                        probe.PayloadWritePages - before.PayloadWritePages;

                    if ((writeDelta > 0) && (payloadWriteDelta > 0))
                    {
                        wprintf(L"PRESSURE_STOP reason=PASS1_TARGET_COLD_WITH_PAYLOAD_WRITES "
                                L"writes=%lld payloadWrites=%lld allocated=%llu MiB\n",
                            writeDelta,
                            payloadWriteDelta,
                            static_cast<unsigned long long>(allocatedMiB));
                        break;
                    }
                }
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
        }

        if (allocatedMiB < 256)
        {
            fwprintf(stderr,
                L"RESULT=ABORT reason=INSUFFICIENT_PRESSURE allocated=%llu MiB\n",
                static_cast<unsigned long long>(allocatedMiB));
            break;
        }

        if (WaitForSingleObject(cold1, 0) != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"RESULT=ABORT reason=PASS1_TARGET_NOT_COLD\n");
            break;
        }

        H3B_COUNTERS prePass1 = {};
        if (!QueryH3B(prePass1))
        {
            fwprintf(stderr, L"RESULT=ABORT reason=PRE_PASS1_QUERY_FAILED\n");
            break;
        }

        if ((prePass1.PayloadMismatches != before.PayloadMismatches) ||
            (prePass1.PagefileTableFull != 0) ||
            (prePass1.DroppedInflightOutstanding != 0))
        {
            fwprintf(stderr,
                L"RESULT=ABORT reason=PRE_PASS1_GUARD payloadMismatches=%lld tableFull=%lld "
                L"droppedInflightOutstanding=%lld\n",
                prePass1.PayloadMismatches,
                prePass1.PagefileTableFull,
                prePass1.DroppedInflightOutstanding);
            break;
        }

        wprintf(L"PASS1_NATURAL_READ_BEGIN\n");
        if (!SetEvent(go1))
        {
            fwprintf(stderr, L"RESULT=ABORT reason=SIGNAL_GO1\n");
            break;
        }

        wait = WaitForSingleObject(done1, kTargetVerifyWaitMs);
        if (wait != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"RESULT=ABORT reason=PASS1_VERIFY_TIMEOUT wait=%lu\n", wait);
            break;
        }

        H3B_COUNTERS afterPass1 = {};
        if (!QueryH3B(afterPass1))
        {
            fwprintf(stderr, L"RESULT=ABORT reason=AFTER_PASS1_QUERY_FAILED\n");
            break;
        }

        const int64_t pass1ReadDelta =
            afterPass1.PayloadReadPages - prePass1.PayloadReadPages;
        const int64_t pass1MatchDelta =
            afterPass1.PayloadMatches - prePass1.PayloadMatches;
        const int64_t pass1MismatchDelta =
            afterPass1.PayloadMismatches - prePass1.PayloadMismatches;

        wprintf(L"PASS1_PAYLOAD_RESULT reads=%lld matches=%lld mismatches=%lld eligible=%lld\n",
            pass1ReadDelta,
            pass1MatchDelta,
            pass1MismatchDelta,
            afterPass1.InterventionEligible);

        if ((pass1MatchDelta <= 0) ||
            (pass1MismatchDelta != 0) ||
            (afterPass1.InterventionEligible != 1))
        {
            fwprintf(stderr,
                L"RESULT=ABORT reason=PASS1_DID_NOT_CREATE_VERIFIED_PAYLOADS "
                L"reads=%lld matches=%lld mismatches=%lld eligible=%lld\n",
                pass1ReadDelta,
                pass1MatchDelta,
                pass1MismatchDelta,
                afterPass1.InterventionEligible);
            break;
        }

        /*
         * The child re-trims the same unchanged target after signaling done1.
         * Wait until that exact target is cold again before arming intervention.
         */
        HANDLE pass2WaitHandles[2] = { cold2, pi.hProcess };
        wait = WaitForMultipleObjects(
            2,
            pass2WaitHandles,
            FALSE,
            kTargetGoWaitMs);

        if (wait != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"RESULT=ABORT reason=PASS2_TARGET_NOT_COLD wait=%lu\n", wait);
            break;
        }

        H3B_COUNTERS armed = {};
        if (!SendH3BCommand(kCommandArm, armed) ||
            (armed.InterventionArmed != 1) ||
            (armed.InterventionEligible != 1) ||
            (armed.PayloadMismatches != afterPass1.PayloadMismatches))
        {
            fwprintf(stderr,
                L"RESULT=ABORT reason=PASS2_ARM_REJECTED eligible=%lld armed=%lld "
                L"payloadMismatches=%lld\n",
                armed.InterventionEligible,
                armed.InterventionArmed,
                armed.PayloadMismatches);
            break;
        }

        wprintf(L"INTERVENTION_ARM_BEFORE_PASS2=PASS capacity=%lld payloadMatches=%lld\n",
            armed.InterventionCapacity,
            armed.PayloadMatches);

        const int64_t attemptsBeforePass2 = armed.InterventionAttempts;
        const int64_t servedBeforePass2 = armed.InterventionServedPages;
        const int64_t fallbacksBeforePass2 = armed.InterventionFallbacks;
        const int64_t guardsBeforePass2 = armed.InterventionGuardRejects;
        const int64_t missesBeforePass2 = armed.InterventionPayloadMisses;

        wprintf(L"PASS2_INTERVENTION_READ_BEGIN\n");
        if (!SetEvent(go2))
        {
            fwprintf(stderr, L"RESULT=ABORT reason=SIGNAL_GO2\n");
            break;
        }

        HANDLE done2WaitHandles[2] = { done2, pi.hProcess };
        wait = WaitForMultipleObjects(
            2,
            done2WaitHandles,
            FALSE,
            kTargetVerifyWaitMs);

        if ((wait != WAIT_OBJECT_0) &&
            (wait != (WAIT_OBJECT_0 + 1)))
        {
            fwprintf(stderr, L"RESULT=ABORT reason=PASS2_VERIFY_TIMEOUT wait=%lu\n", wait);
            break;
        }

        if (wait == WAIT_OBJECT_0)
        {
            WaitForSingleObject(pi.hProcess, 30000);
        }

        DWORD childCode = STILL_ACTIVE;
        GetExitCodeProcess(pi.hProcess, &childCode);
        if (childCode != 0)
        {
            fwprintf(stderr, L"RESULT=ABORT reason=PASS2_TARGET_VERIFY_FAILED code=%lu\n",
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

        const int64_t attemptsDelta =
            after.InterventionAttempts - attemptsBeforePass2;
        const int64_t servedDelta =
            after.InterventionServedPages - servedBeforePass2;
        const int64_t fallbackDelta =
            after.InterventionFallbacks - fallbacksBeforePass2;
        const int64_t guardDelta =
            after.InterventionGuardRejects - guardsBeforePass2;
        const int64_t payloadMissDelta =
            after.InterventionPayloadMisses - missesBeforePass2;

        wprintf(L"PASS2_INTERVENTION_RESULT attempts=%lld servedPages=%lld fallbacks=%lld "
                L"guardRejects=%lld payloadMisses=%lld armedFinal=%lld\n",
            attemptsDelta,
            servedDelta,
            fallbackDelta,
            guardDelta,
            payloadMissDelta,
            after.InterventionArmed);

        if ((after.PayloadMismatches == before.PayloadMismatches) &&
            (servedDelta > 0) &&
            (servedDelta <= after.InterventionCapacity) &&
            (attemptsDelta >= 1) &&
            (after.InterventionArmed == 0))
        {
            wprintf(L"RESULT=INTERVENTION_PASS verifiedPayloadClusterPages=%lld attemptsDelta=%lld "
                    L"fallbacksDelta=%lld guardRejectsDelta=%lld payloadMissesDelta=%lld\n",
                servedDelta,
                attemptsDelta,
                fallbackDelta,
                guardDelta,
                payloadMissDelta);
            result = 0;
        }
        else
        {
            wprintf(L"RESULT=INTERVENTION_NOT_SERVED servedPagesDelta=%lld attemptsDelta=%lld "
                    L"fallbacksDelta=%lld guardRejectsDelta=%lld payloadMissesDelta=%lld "
                    L"payloadMismatches=%lld\n",
                servedDelta,
                attemptsDelta,
                fallbackDelta,
                guardDelta,
                payloadMissDelta,
                after.PayloadMismatches - before.PayloadMismatches);
            result = 53;
        }
    }
    while (false);

    H3B_COUNTERS disarmed = {};
    if (SendH3BCommand(kCommandDisarm, disarmed))
    {
        wprintf(L"INTERVENTION_CLEANUP armed=%lld served=%lld attempts=%lld\n",
            disarmed.InterventionArmed,
            disarmed.InterventionServedPages,
            disarmed.InterventionAttempts);
    }

    SetEvent(go1);
    SetEvent(go2);

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
    CloseHandle(ready); CloseHandle(cold1); CloseHandle(go1); CloseHandle(done1);
    CloseHandle(cold2); CloseHandle(go2); CloseHandle(done2);

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

    if ((argc == 2) && (_wcsicmp(argv[1], L"--preflight") == 0))
    {
        return MachinePreflight();
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
        fwprintf(stderr, L"Usage: StateRAMH3D1Intervention.exe [--selftest|--preflight]\n");
        return 1;
    }

    return ParentMode();
}
