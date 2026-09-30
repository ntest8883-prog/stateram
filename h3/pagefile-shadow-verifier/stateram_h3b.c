#include <fltKernel.h>

#define H3B_PROTOCOL_VERSION 5
#define H3B_COMMAND_QUERY        1
#define H3B_COMMAND_RESET        2
#define H3B_COMMAND_ARM_ONCE     3
#define H3B_COMMAND_DISARM       4

#define H3B_MAX_PAGEFILE_OBJECTS 16
#define H3B_MAX_PAGEFILE_IDENTITIES 4
#define H3B_WRITE_HISTORY_SLOTS 2048
#define H3B_SHADOW_SLOTS     32768
#define H3B_PAYLOAD_SLOTS    2048
#define H3B_PAYLOAD_WRITES_RETAINED (H3B_PAYLOAD_SLOTS / H3B_MAX_HASH_PAGES_PER_IO)
#define H3B_POOL_TAG         'B3HS'
#define H3B_MAX_HASH_PAGES_PER_IO 4
#define H3B_WRITE_STATE_INFLIGHT  1
#define H3B_WRITE_STATE_COMPLETED 2
#define H3B_WRITE_FLAG_CONCURRENT_OVERLAP 0x00000001UL

typedef struct _H3B_COMMAND
{
    ULONG Version;
    ULONG Command;
} H3B_COMMAND, *PH3B_COMMAND;

typedef struct _H3B_COUNTERS
{
    ULONG Version;
    ULONG Size;

    LONG64 PagefileReads;
    LONG64 PagefileReadBytes;
    LONG64 PagefileWrites;
    LONG64 PagefileWriteBytes;
    LONG64 PagingFileCreates;

    LONG64 ShadowWritePages;
    LONG64 ShadowReadPages;
    LONG64 ShadowMatches;
    LONG64 ShadowMismatches;
    LONG64 ShadowUntracked;
    LONG64 ShadowReplacements;
    LONG64 ShadowBufferUnavailable;
    LONG64 ShadowUnaligned;
    LONG64 ShadowHighIrqlSkips;
    LONG64 ShadowTableEntries;
    LONG64 ShadowTableCapacity;
    LONG64 ShadowPublishSkipped;
    LONG64 ShadowVerifyInvalidated;
    LONG64 HistoryExpired;
    LONG64 HistoryRecordDrops;
    LONG64 KnownPagefiles;
    LONG64 HistoryCapacity;
    LONG64 PagefileTableFull;
    LONG64 PagefileIdentities;
    LONG64 PagefileAliases;
    LONG64 ReadNewSystemBuffers;
    LONG64 CrossObjectComparisons;
    LONG64 CrossObjectMatches;
    LONG64 CrossObjectMismatches;
    LONG64 NewSystemBufferComparisons;
    LONG64 NewSystemBufferMismatches;
    LONG64 DynamicPagefileDiscoveries;
    LONG64 ConcurrentOverlapSkips;
    LONG64 DroppedInflightRecords;
    LONG64 DroppedInflightOutstanding;

    LONG64 PayloadWritePages;
    LONG64 PayloadReadPages;
    LONG64 PayloadMatches;
    LONG64 PayloadMismatches;
    LONG64 PayloadReplacements;
    LONG64 PayloadLookupMisses;
    LONG64 PayloadCaptureSkipped;
    LONG64 PayloadBufferUnavailable;
    LONG64 PayloadCapacity;

    LONG64 InterventionArmed;
    LONG64 InterventionAttempts;
    LONG64 InterventionEligible;
    LONG64 InterventionServed;
    LONG64 InterventionBytes;
    LONG64 InterventionFallbacks;
    LONG64 InterventionPayloadMisses;
    LONG64 InterventionBufferUnavailable;
    LONG64 InterventionSafetyRejects;
} H3B_COUNTERS, *PH3B_COUNTERS;

C_ASSERT(sizeof(H3B_COMMAND) == 8);
C_ASSERT(sizeof(H3B_COUNTERS) == 432);

typedef struct _H3B_SHADOW_ENTRY
{
    ULONG IdentityIndex;
    ULONG Reserved;
    PFILE_OBJECT WriterFileObject;
    ULONGLONG Offset;
    ULONGLONG Hash1;
    ULONGLONG Hash2;
    ULONG Generation;
    ULONG Reserved2;
    ULONGLONG WriteSequence;
} H3B_SHADOW_ENTRY, *PH3B_SHADOW_ENTRY;

typedef struct _H3B_PAYLOAD_ENTRY
{
    ULONG IdentityIndex;
    ULONG Generation;
    ULONGLONG Offset;
    ULONGLONG WriteSequence;
    UCHAR Bytes[PAGE_SIZE];
} H3B_PAYLOAD_ENTRY, *PH3B_PAYLOAD_ENTRY;

typedef struct _H3B_WRITE_RANGE
{
    ULONGLONG Start;
    ULONGLONG EndExclusive;
    ULONGLONG Sequence;
    ULONG Generation;
    ULONG State;
    ULONG Flags;
    ULONG Reserved;
} H3B_WRITE_RANGE, *PH3B_WRITE_RANGE;

typedef struct _H3B_WRITE_CONTEXT
{
    ULONGLONG Sequence;
    ULONG PageCount;
    BOOLEAN PreHashValid;
    UCHAR Reserved[3];
    ULONGLONG Hash1[H3B_MAX_HASH_PAGES_PER_IO];
    ULONGLONG Hash2[H3B_MAX_HASH_PAGES_PER_IO];
} H3B_WRITE_CONTEXT, *PH3B_WRITE_CONTEXT;

typedef struct _H3B_PAGEFILE_STATE
{
    PFLT_INSTANCE Instance;
    ULONG HistoryHead;
    ULONG HistoryCount;
    ULONG DroppedInflightCount;
    ULONG Reserved;
    ULONGLONG HistoryFloor;
    H3B_WRITE_RANGE History[H3B_WRITE_HISTORY_SLOTS];
} H3B_PAGEFILE_STATE, *PH3B_PAGEFILE_STATE;

typedef struct _H3B_PAGEFILE_OBJECT
{
    PFILE_OBJECT FileObject;
    ULONG IdentityIndex;
    ULONG Reserved;
} H3B_PAGEFILE_OBJECT, *PH3B_PAGEFILE_OBJECT;

PFLT_FILTER g_Filter;
PFLT_PORT g_ServerPort;
PFLT_PORT g_ClientPort;

KSPIN_LOCK g_PagefileLock;
H3B_PAGEFILE_STATE g_PagefileIdentities[H3B_MAX_PAGEFILE_IDENTITIES];
H3B_PAGEFILE_OBJECT g_PagefileObjects[H3B_MAX_PAGEFILE_OBJECTS];

KSPIN_LOCK g_ShadowLock;
PH3B_SHADOW_ENTRY g_ShadowTable;
KSPIN_LOCK g_PayloadLock;
H3B_PAYLOAD_ENTRY g_PayloadTable[H3B_PAYLOAD_SLOTS];
volatile LONG g_ShadowGeneration;
volatile LONG64 g_WriteSequence;

volatile LONG64 g_PagefileReads;
volatile LONG64 g_PagefileReadBytes;
volatile LONG64 g_PagefileWrites;
volatile LONG64 g_PagefileWriteBytes;
volatile LONG64 g_PagingFileCreates;

volatile LONG64 g_ShadowWritePages;
volatile LONG64 g_ShadowReadPages;
volatile LONG64 g_ShadowMatches;
volatile LONG64 g_ShadowMismatches;
volatile LONG64 g_ShadowUntracked;
volatile LONG64 g_ShadowReplacements;
volatile LONG64 g_ShadowBufferUnavailable;
volatile LONG64 g_ShadowUnaligned;
volatile LONG64 g_ShadowHighIrqlSkips;
volatile LONG64 g_ShadowTableEntries;
volatile LONG64 g_ShadowPublishSkipped;
volatile LONG64 g_ShadowVerifyInvalidated;
volatile LONG64 g_HistoryExpired;
volatile LONG64 g_HistoryRecordDrops;
volatile LONG64 g_KnownPagefiles;
volatile LONG64 g_PagefileTableFull;
volatile LONG64 g_PagefileIdentitiesCount;
volatile LONG64 g_PagefileAliases;
volatile LONG64 g_ReadNewSystemBuffers;
volatile LONG64 g_CrossObjectComparisons;
volatile LONG64 g_CrossObjectMatches;
volatile LONG64 g_CrossObjectMismatches;
volatile LONG64 g_NewSystemBufferComparisons;
volatile LONG64 g_NewSystemBufferMismatches;
volatile LONG64 g_DynamicPagefileDiscoveries;
volatile LONG64 g_ConcurrentOverlapSkips;
volatile LONG64 g_DroppedInflightRecords;
volatile LONG64 g_DroppedInflightOutstanding;

volatile LONG64 g_PayloadWritePages;
volatile LONG64 g_PayloadReadPages;
volatile LONG64 g_PayloadMatches;
volatile LONG64 g_PayloadMismatches;
volatile LONG64 g_PayloadReplacements;
volatile LONG64 g_PayloadLookupMisses;
volatile LONG64 g_PayloadCaptureSkipped;
volatile LONG64 g_PayloadBufferUnavailable;

volatile LONG g_InterventionArmed;
volatile LONG64 g_InterventionAttempts;
volatile LONG64 g_InterventionEligible;
volatile LONG64 g_InterventionServed;
volatile LONG64 g_InterventionBytes;
volatile LONG64 g_InterventionFallbacks;
volatile LONG64 g_InterventionPayloadMisses;
volatile LONG64 g_InterventionBufferUnavailable;
volatile LONG64 g_InterventionSafetyRejects;

volatile LONG g_TrackingCompromised;

DRIVER_INITIALIZE DriverEntry;

static
LONG64
H3BReadCounter (
    _In_ volatile LONG64* Counter
    )
{
    return InterlockedCompareExchange64(Counter, 0, 0);
}

static
VOID
H3BResetCounters (
    VOID
    )
{
    LONG generation;

    InterlockedExchange64(&g_PagefileReads, 0);
    InterlockedExchange64(&g_PagefileReadBytes, 0);
    InterlockedExchange64(&g_PagefileWrites, 0);
    InterlockedExchange64(&g_PagefileWriteBytes, 0);
    InterlockedExchange64(&g_PagingFileCreates, 0);

    InterlockedExchange64(&g_ShadowWritePages, 0);
    InterlockedExchange64(&g_ShadowReadPages, 0);
    InterlockedExchange64(&g_ShadowMatches, 0);
    InterlockedExchange64(&g_ShadowMismatches, 0);
    InterlockedExchange64(&g_ShadowUntracked, 0);
    InterlockedExchange64(&g_ShadowReplacements, 0);
    InterlockedExchange64(&g_ShadowBufferUnavailable, 0);
    InterlockedExchange64(&g_ShadowUnaligned, 0);
    InterlockedExchange64(&g_ShadowHighIrqlSkips, 0);
    InterlockedExchange64(&g_ShadowTableEntries, 0);
    InterlockedExchange64(&g_ShadowPublishSkipped, 0);
    InterlockedExchange64(&g_ShadowVerifyInvalidated, 0);
    InterlockedExchange64(&g_HistoryExpired, 0);
    InterlockedExchange64(&g_HistoryRecordDrops, 0);
    InterlockedExchange64(&g_PagefileTableFull, 0);
    InterlockedExchange64(&g_ReadNewSystemBuffers, 0);
    InterlockedExchange64(&g_CrossObjectComparisons, 0);
    InterlockedExchange64(&g_CrossObjectMatches, 0);
    InterlockedExchange64(&g_CrossObjectMismatches, 0);
    InterlockedExchange64(&g_NewSystemBufferComparisons, 0);
    InterlockedExchange64(&g_NewSystemBufferMismatches, 0);
    InterlockedExchange64(&g_DynamicPagefileDiscoveries, 0);
    InterlockedExchange64(&g_ConcurrentOverlapSkips, 0);
    InterlockedExchange64(&g_DroppedInflightRecords, 0);

    InterlockedExchange64(&g_PayloadWritePages, 0);
    InterlockedExchange64(&g_PayloadReadPages, 0);
    InterlockedExchange64(&g_PayloadMatches, 0);
    InterlockedExchange64(&g_PayloadMismatches, 0);
    InterlockedExchange64(&g_PayloadReplacements, 0);
    InterlockedExchange64(&g_PayloadLookupMisses, 0);
    InterlockedExchange64(&g_PayloadCaptureSkipped, 0);
    InterlockedExchange64(&g_PayloadBufferUnavailable, 0);

    InterlockedExchange(&g_InterventionArmed, 0);
    InterlockedExchange64(&g_InterventionAttempts, 0);
    InterlockedExchange64(&g_InterventionEligible, 0);
    InterlockedExchange64(&g_InterventionServed, 0);
    InterlockedExchange64(&g_InterventionBytes, 0);
    InterlockedExchange64(&g_InterventionFallbacks, 0);
    InterlockedExchange64(&g_InterventionPayloadMisses, 0);
    InterlockedExchange64(&g_InterventionBufferUnavailable, 0);
    InterlockedExchange64(&g_InterventionSafetyRejects, 0);

    generation = InterlockedIncrement(&g_ShadowGeneration);

    if (generation == 0)
    {
        KIRQL oldIrql;

        KeAcquireSpinLock(&g_ShadowLock, &oldIrql);
        RtlZeroMemory(
            g_ShadowTable,
            sizeof(H3B_SHADOW_ENTRY) * H3B_SHADOW_SLOTS);
        g_ShadowGeneration = 1;
        KeReleaseSpinLock(&g_ShadowLock, oldIrql);

        KeAcquireSpinLock(&g_PayloadLock, &oldIrql);
        RtlZeroMemory(
            g_PayloadTable,
            sizeof(g_PayloadTable));
        KeReleaseSpinLock(&g_PayloadLock, oldIrql);
    }
}

static
LONG
H3BFindPagefileObjectIndexLocked (
    _In_ PFILE_OBJECT FileObject
    )
{
    ULONG i;

    for (i = 0; i < H3B_MAX_PAGEFILE_OBJECTS; i++)
    {
        if (g_PagefileObjects[i].FileObject == FileObject)
        {
            return (LONG)i;
        }
    }

    return -1;
}

static
LONG
H3BFindPagefileIdentityIndexLocked (
    _In_ PFLT_INSTANCE Instance
    )
{
    ULONG i;

    for (i = 0; i < H3B_MAX_PAGEFILE_IDENTITIES; i++)
    {
        if (g_PagefileIdentities[i].Instance == Instance)
        {
            return (LONG)i;
        }
    }

    return -1;
}

static
BOOLEAN
H3BGetIdentityIndexForObjectLocked (
    _In_ PFILE_OBJECT FileObject,
    _Out_ PULONG IdentityIndex
    )
{
    LONG objectIndex;

    objectIndex = H3BFindPagefileObjectIndexLocked(FileObject);

    if (objectIndex < 0)
    {
        return FALSE;
    }

    if (g_PagefileObjects[objectIndex].IdentityIndex >=
        H3B_MAX_PAGEFILE_IDENTITIES)
    {
        return FALSE;
    }

    *IdentityIndex = g_PagefileObjects[objectIndex].IdentityIndex;
    return TRUE;
}

static
BOOLEAN
H3BGetPagefileIdentityIndex (
    _In_ PFILE_OBJECT FileObject,
    _Out_ PULONG IdentityIndex
    )
{
    KIRQL oldIrql;
    BOOLEAN found;

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);
    found = H3BGetIdentityIndexForObjectLocked(
        FileObject,
        IdentityIndex);
    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    return found;
}

static
BOOLEAN
H3BIsKnownPagefile (
    _In_ PFILE_OBJECT FileObject
    )
{
    ULONG identityIndex;

    return H3BGetPagefileIdentityIndex(
        FileObject,
        &identityIndex);
}

static
BOOLEAN
H3BRememberPagefile (
    _In_ PFILE_OBJECT FileObject,
    _In_ PFLT_INSTANCE Instance
    )
{
    KIRQL oldIrql;
    LONG identityIndex;
    ULONG objectSlot;
    ULONG identitySlot;
    BOOLEAN newIdentity;

    objectSlot = H3B_MAX_PAGEFILE_OBJECTS;
    identitySlot = H3B_MAX_PAGEFILE_IDENTITIES;
    newIdentity = FALSE;

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    if (H3BFindPagefileObjectIndexLocked(FileObject) >= 0)
    {
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    for (objectSlot = 0;
         objectSlot < H3B_MAX_PAGEFILE_OBJECTS;
         objectSlot++)
    {
        if (g_PagefileObjects[objectSlot].FileObject == NULL)
        {
            break;
        }
    }

    if (objectSlot == H3B_MAX_PAGEFILE_OBJECTS)
    {
        InterlockedExchange(&g_TrackingCompromised, 1);
        InterlockedIncrement64(&g_PagefileTableFull);
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    identityIndex = H3BFindPagefileIdentityIndexLocked(Instance);

    if (identityIndex < 0)
    {
        for (identitySlot = 0;
             identitySlot < H3B_MAX_PAGEFILE_IDENTITIES;
             identitySlot++)
        {
            if (g_PagefileIdentities[identitySlot].Instance == NULL)
            {
                break;
            }
        }

        if (identitySlot == H3B_MAX_PAGEFILE_IDENTITIES)
        {
            InterlockedExchange(&g_TrackingCompromised, 1);
            InterlockedIncrement64(&g_PagefileTableFull);
            KeReleaseSpinLock(&g_PagefileLock, oldIrql);
            return FALSE;
        }

        RtlZeroMemory(
            &g_PagefileIdentities[identitySlot],
            sizeof(g_PagefileIdentities[identitySlot]));

        g_PagefileIdentities[identitySlot].Instance = Instance;
        identityIndex = (LONG)identitySlot;
        newIdentity = TRUE;
    }

    ObReferenceObject(FileObject);

    g_PagefileObjects[objectSlot].FileObject = FileObject;
    g_PagefileObjects[objectSlot].IdentityIndex = (ULONG)identityIndex;
    g_PagefileObjects[objectSlot].Reserved = 0;

    InterlockedIncrement64(&g_PagingFileCreates);
    InterlockedIncrement64(&g_KnownPagefiles);

    if (newIdentity)
    {
        InterlockedIncrement64(&g_PagefileIdentitiesCount);
    }
    else
    {
        InterlockedIncrement64(&g_PagefileAliases);
    }

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);
    return TRUE;
}

static
VOID
H3BReleasePagefiles (
    VOID
    )
{
    KIRQL oldIrql;
    ULONG i;
    PFILE_OBJECT objects[H3B_MAX_PAGEFILE_OBJECTS];

    RtlZeroMemory(objects, sizeof(objects));

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    for (i = 0; i < H3B_MAX_PAGEFILE_OBJECTS; i++)
    {
        objects[i] = g_PagefileObjects[i].FileObject;
        RtlZeroMemory(
            &g_PagefileObjects[i],
            sizeof(g_PagefileObjects[i]));
    }

    RtlZeroMemory(
        g_PagefileIdentities,
        sizeof(g_PagefileIdentities));

    InterlockedExchange64(&g_KnownPagefiles, 0);
    InterlockedExchange64(&g_PagefileIdentitiesCount, 0);
    InterlockedExchange64(&g_PagefileAliases, 0);
    InterlockedExchange64(&g_DroppedInflightOutstanding, 0);

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    for (i = 0; i < H3B_MAX_PAGEFILE_OBJECTS; i++)
    {
        if (objects[i] != NULL)
        {
            ObDereferenceObject(objects[i]);
        }
    }
}

static
BOOLEAN
H3BEnsureKnownPagefile (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects
    )
{
    PFILE_OBJECT fileObject;

    fileObject = Data->Iopb->TargetFileObject;

    if (H3BIsKnownPagefile(fileObject))
    {
        return TRUE;
    }

    /*
     * This fallback lets a DEMAND_START verifier discover already-open
     * pagefiles without requiring a reboot.  We invoke FsRtlIsPagingFile only
     * for IRP-based paging I/O, and only from the pre-operation path where
     * Filter Manager callbacks run at <= APC_LEVEL.
     */
    if (!FLT_IS_IRP_OPERATION(Data) ||
        !FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO) ||
        (KeGetCurrentIrql() > APC_LEVEL))
    {
        return FALSE;
    }

    if (!FsRtlIsPagingFile(fileObject))
    {
        return FALSE;
    }

    if (H3BRememberPagefile(
            fileObject,
            FltObjects->Instance))
    {
        InterlockedIncrement64(&g_DynamicPagefileDiscoveries);
    }

    return H3BIsKnownPagefile(fileObject);
}

static
BOOLEAN
H3BRangeOverlapsPage (
    _In_ const H3B_WRITE_RANGE* Range,
    _In_ ULONGLONG PageOffset
    )
{
    ULONGLONG pageEnd;

    if (PageOffset > (~0ULL - PAGE_SIZE))
    {
        pageEnd = ~0ULL;
    }
    else
    {
        pageEnd = PageOffset + PAGE_SIZE;
    }

    return ((Range->Start < pageEnd) &&
            (Range->EndExclusive > PageOffset)) ? TRUE : FALSE;
}

static
BOOLEAN
H3BRangeOverlapsRange (
    _In_ const H3B_WRITE_RANGE* Range,
    _In_ ULONGLONG Start,
    _In_ ULONGLONG EndExclusive
    )
{
    return ((Range->Start < EndExclusive) &&
            (Range->EndExclusive > Start)) ? TRUE : FALSE;
}

static
BOOLEAN
H3BRecordWriteRange (
    _In_ PFILE_OBJECT FileObject,
    _In_ LONGLONG ByteOffset,
    _In_ ULONG Length,
    _Out_ PULONGLONG Sequence
    )
{
    KIRQL oldIrql;
    ULONG identityIndex;
    PH3B_PAGEFILE_STATE state;
    PH3B_WRITE_RANGE record;
    ULONGLONG start;
    ULONGLONG endExclusive;
    ULONGLONG sequence;
    ULONG generation;
    ULONG i;
    ULONG newFlags;

    newFlags = 0;
    sequence = (ULONGLONG)InterlockedIncrement64(&g_WriteSequence);
    if (sequence == 0)
    {
        sequence = (ULONGLONG)InterlockedIncrement64(&g_WriteSequence);
    }

    generation = (ULONG)InterlockedCompareExchange(
        &g_ShadowGeneration,
        0,
        0);

    if ((ByteOffset < 0) || (Length == 0) ||
        ((ULONGLONG)ByteOffset > (~0ULL - (ULONGLONG)Length)))
    {
        start = 0;
        endExclusive = ~0ULL;
    }
    else
    {
        start = (ULONGLONG)ByteOffset;
        endExclusive = start + (ULONGLONG)Length;
    }

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    if (!H3BGetIdentityIndexForObjectLocked(
            FileObject,
            &identityIndex))
    {
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    state = &g_PagefileIdentities[identityIndex];

    /*
     * Remember that two overlapping writes existed concurrently.  Callback
     * execution order alone cannot be used to infer which completed bytes are
     * the final storage contents on a multi-CPU system, so neither member of
     * such a pair is allowed to publish a fingerprint.
     */
    for (i = 0; i < state->HistoryCount; i++)
    {
        PH3B_WRITE_RANGE existing = &state->History[i];

        if ((existing->State == H3B_WRITE_STATE_INFLIGHT) &&
            H3BRangeOverlapsRange(
                existing,
                start,
                endExclusive))
        {
            existing->Flags |= H3B_WRITE_FLAG_CONCURRENT_OVERLAP;
            newFlags |= H3B_WRITE_FLAG_CONCURRENT_OVERLAP;
        }
    }

    if (state->HistoryCount == H3B_WRITE_HISTORY_SLOTS)
    {
        record = &state->History[state->HistoryHead];

        if (record->State == H3B_WRITE_STATE_INFLIGHT)
        {
            state->DroppedInflightCount++;
            InterlockedIncrement64(&g_DroppedInflightRecords);
            InterlockedIncrement64(&g_DroppedInflightOutstanding);
        }

        if (record->Sequence > state->HistoryFloor)
        {
            state->HistoryFloor = record->Sequence;
        }

        InterlockedIncrement64(&g_HistoryRecordDrops);
    }
    else
    {
        state->HistoryCount++;
    }

    record = &state->History[state->HistoryHead];
    record->Start = start;
    record->EndExclusive = endExclusive;
    record->Sequence = sequence;
    record->Generation = generation;
    record->State = H3B_WRITE_STATE_INFLIGHT;
    record->Flags = newFlags;
    record->Reserved = 0;

    state->HistoryHead =
        (state->HistoryHead + 1) % H3B_WRITE_HISTORY_SLOTS;

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    *Sequence = sequence;
    return TRUE;
}

static
BOOLEAN
H3BMarkWriteComplete (
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONGLONG Sequence,
    _Out_opt_ PULONG IdentityIndex
    )
{
    KIRQL oldIrql;
    ULONG identityIndex;
    PH3B_PAGEFILE_STATE state;
    ULONG i;
    BOOLEAN found;

    found = FALSE;
    identityIndex = H3B_MAX_PAGEFILE_IDENTITIES;

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    if (!H3BGetIdentityIndexForObjectLocked(
            FileObject,
            &identityIndex))
    {
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    state = &g_PagefileIdentities[identityIndex];

    /*
     * A sequence at/below the floor has already been evicted from the bounded
     * history.  Because its post-write callback is only arriving now, that
     * evicted record was necessarily still INFLIGHT when overwritten.
     * Retire exactly one conservative unknown-write barrier.
     */
    if (Sequence <= state->HistoryFloor)
    {
        if (state->DroppedInflightCount != 0)
        {
            state->DroppedInflightCount--;
            InterlockedDecrement64(&g_DroppedInflightOutstanding);
        }

        KeReleaseSpinLock(&g_PagefileLock, oldIrql);

        if (IdentityIndex != NULL)
        {
            *IdentityIndex = identityIndex;
        }

        return FALSE;
    }

    for (i = 0; i < state->HistoryCount; i++)
    {
        PH3B_WRITE_RANGE record = &state->History[i];

        if (record->Sequence == Sequence)
        {
            if (record->State == H3B_WRITE_STATE_INFLIGHT)
            {
                record->State = H3B_WRITE_STATE_COMPLETED;
            }

            found = TRUE;
            break;
        }
    }

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    if (IdentityIndex != NULL)
    {
        *IdentityIndex = identityIndex;
    }

    return found;
}

static
BOOLEAN
H3BHistoryAllowsPublish (
    _In_ ULONG IdentityIndex,
    _In_ ULONGLONG PageOffset,
    _In_ ULONGLONG Sequence,
    _Out_opt_ PULONG Generation
    )
{
    KIRQL oldIrql;
    PH3B_PAGEFILE_STATE state;
    ULONG i;
    BOOLEAN foundOwn;
    BOOLEAN newerOverlap;
    BOOLEAN olderInflightOverlap;
    ULONG ownGeneration;
    ULONG ownState;
    ULONG ownFlags;

    foundOwn = FALSE;
    newerOverlap = FALSE;
    olderInflightOverlap = FALSE;
    ownGeneration = 0;
    ownState = 0;
    ownFlags = 0;

    if ((IdentityIndex >= H3B_MAX_PAGEFILE_IDENTITIES) ||
        (InterlockedCompareExchange(
            &g_TrackingCompromised,
            0,
            0) != 0))
    {
        return FALSE;
    }

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    state = &g_PagefileIdentities[IdentityIndex];

    if (state->Instance == NULL)
    {
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    /*
     * If any INFLIGHT record was dropped from this identity's bounded history,
     * its range is no longer known.  Until that write completes we cannot prove
     * that a candidate page will stay current, so publishing any new sample
     * would be unsafe.
     */
    if (state->DroppedInflightCount != 0)
    {
        InterlockedIncrement64(&g_ConcurrentOverlapSkips);
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    if (Sequence <= state->HistoryFloor)
    {
        InterlockedIncrement64(&g_HistoryExpired);
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    for (i = 0; i < state->HistoryCount; i++)
    {
        const H3B_WRITE_RANGE* record = &state->History[i];

        if (record->Sequence == Sequence)
        {
            foundOwn = TRUE;
            ownGeneration = record->Generation;
            ownState = record->State;
            ownFlags = record->Flags;
        }
        else if (H3BRangeOverlapsPage(record, PageOffset))
        {
            if (record->Sequence > Sequence)
            {
                /*
                 * Any newer overlapping pre-write conservatively invalidates
                 * this candidate, whether that newer I/O has completed or not.
                 */
                newerOverlap = TRUE;
            }
            else if ((record->Sequence < Sequence) &&
                     (record->State == H3B_WRITE_STATE_INFLIGHT))
            {
                /*
                 * This is the out-of-order completion case H3-B4 missed:
                 * an older overlapping write is still in flight.  It can finish
                 * after this write and become the final storage contents.
                 */
                olderInflightOverlap = TRUE;
            }
        }
    }

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    if (!foundOwn ||
        (ownState != H3B_WRITE_STATE_COMPLETED) ||
        FlagOn(ownFlags, H3B_WRITE_FLAG_CONCURRENT_OVERLAP) ||
        newerOverlap ||
        olderInflightOverlap)
    {
        if (FlagOn(ownFlags, H3B_WRITE_FLAG_CONCURRENT_OVERLAP) ||
            olderInflightOverlap)
        {
            InterlockedIncrement64(&g_ConcurrentOverlapSkips);
        }

        return FALSE;
    }

    if ((ULONG)InterlockedCompareExchange(
            &g_ShadowGeneration,
            0,
            0) != ownGeneration)
    {
        return FALSE;
    }

    if (Generation != NULL)
    {
        *Generation = ownGeneration;
    }

    return TRUE;
}

static
BOOLEAN
H3BHistoryAllowsVerify (
    _In_ ULONG IdentityIndex,
    _In_ ULONGLONG PageOffset,
    _In_ ULONGLONG Sequence
    )
{
    KIRQL oldIrql;
    PH3B_PAGEFILE_STATE state;
    ULONG i;
    BOOLEAN foundOwn;
    BOOLEAN newerOverlap;
    ULONG ownGeneration;
    ULONG ownState;

    foundOwn = FALSE;
    newerOverlap = FALSE;
    ownGeneration = 0;
    ownState = 0;

    if ((IdentityIndex >= H3B_MAX_PAGEFILE_IDENTITIES) ||
        (InterlockedCompareExchange(
            &g_TrackingCompromised,
            0,
            0) != 0))
    {
        return FALSE;
    }

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    state = &g_PagefileIdentities[IdentityIndex];

    if (state->Instance == NULL)
    {
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    /*
     * A dropped INFLIGHT range is an unknown overlapping-write possibility.
     * Refuse comparisons until its post-write completion retires the barrier.
     */
    if (state->DroppedInflightCount != 0)
    {
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    if (Sequence <= state->HistoryFloor)
    {
        InterlockedIncrement64(&g_HistoryExpired);
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    for (i = 0; i < state->HistoryCount; i++)
    {
        const H3B_WRITE_RANGE* record = &state->History[i];

        if (record->Sequence == Sequence)
        {
            foundOwn = TRUE;
            ownGeneration = record->Generation;
            ownState = record->State;
        }
        else if ((record->Sequence > Sequence) &&
                 H3BRangeOverlapsPage(record, PageOffset))
        {
            newerOverlap = TRUE;
        }
    }

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    if (!foundOwn ||
        (ownState != H3B_WRITE_STATE_COMPLETED) ||
        newerOverlap)
    {
        return FALSE;
    }

    if ((ULONG)InterlockedCompareExchange(
            &g_ShadowGeneration,
            0,
            0) != ownGeneration)
    {
        return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
H3BHistoryAllowsServeLocked (
    _In_ ULONG IdentityIndex,
    _In_ ULONGLONG PageOffset,
    _In_ ULONGLONG Sequence
    )
{
    PH3B_PAGEFILE_STATE state;
    ULONG i;
    BOOLEAN foundOwn;
    BOOLEAN newerOverlap;
    BOOLEAN olderInflightOverlap;
    ULONG ownGeneration;
    ULONG ownState;
    ULONG ownFlags;

    foundOwn = FALSE;
    newerOverlap = FALSE;
    olderInflightOverlap = FALSE;
    ownGeneration = 0;
    ownState = 0;
    ownFlags = 0;

    if ((IdentityIndex >= H3B_MAX_PAGEFILE_IDENTITIES) ||
        (InterlockedCompareExchange(&g_TrackingCompromised, 0, 0) != 0))
    {
        return FALSE;
    }

    state = &g_PagefileIdentities[IdentityIndex];

    if ((state->Instance == NULL) ||
        (state->DroppedInflightCount != 0) ||
        (Sequence <= state->HistoryFloor))
    {
        return FALSE;
    }

    for (i = 0; i < state->HistoryCount; i++)
    {
        const H3B_WRITE_RANGE* record = &state->History[i];

        if (record->Sequence == Sequence)
        {
            foundOwn = TRUE;
            ownGeneration = record->Generation;
            ownState = record->State;
            ownFlags = record->Flags;
        }
        else if (H3BRangeOverlapsPage(record, PageOffset))
        {
            if (record->Sequence > Sequence)
            {
                newerOverlap = TRUE;
            }
            else if ((record->Sequence < Sequence) &&
                     (record->State == H3B_WRITE_STATE_INFLIGHT))
            {
                olderInflightOverlap = TRUE;
            }
        }
    }

    if (!foundOwn ||
        (ownState != H3B_WRITE_STATE_COMPLETED) ||
        FlagOn(ownFlags, H3B_WRITE_FLAG_CONCURRENT_OVERLAP) ||
        newerOverlap ||
        olderInflightOverlap)
    {
        return FALSE;
    }

    if ((ULONG)InterlockedCompareExchange(
            &g_ShadowGeneration,
            0,
            0) != ownGeneration)
    {
        return FALSE;
    }

    return TRUE;
}

static
ULONGLONG
H3BHashPage (
    _In_reads_bytes_(PAGE_SIZE) const UCHAR* Buffer,
    _In_ ULONGLONG Seed
    )
{
    ULONGLONG hash;
    ULONG i;

    hash = 1469598103934665603ULL ^ Seed;

    for (i = 0; i < PAGE_SIZE; i++)
    {
        hash ^= Buffer[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

static
ULONG
H3BShadowIndex (
    _In_ ULONG IdentityIndex,
    _In_ ULONGLONG Offset
    )
{
    ULONGLONG value;

    value = (Offset >> PAGE_SHIFT);
    value ^= ((ULONGLONG)IdentityIndex * 0x9E3779B97F4A7C15ULL);
    value ^= (value >> 17);

    return (ULONG)(value & (H3B_SHADOW_SLOTS - 1));
}

static
BOOLEAN
H3BStoreShadow (
    _In_ ULONG IdentityIndex,
    _In_ PFILE_OBJECT WriterFileObject,
    _In_ ULONGLONG Offset,
    _In_ ULONGLONG Hash1,
    _In_ ULONGLONG Hash2,
    _In_ ULONGLONG WriteSequence,
    _In_ ULONG WriteGeneration
    )
{
    KIRQL oldIrql;
    ULONG index;
    LONG generation;
    PH3B_SHADOW_ENTRY entry;
    BOOLEAN currentEntry;

    index = H3BShadowIndex(IdentityIndex, Offset);
    generation = InterlockedCompareExchange(&g_ShadowGeneration, 0, 0);

    if ((ULONG)generation != WriteGeneration)
    {
        return FALSE;
    }

    KeAcquireSpinLock(&g_ShadowLock, &oldIrql);

    entry = &g_ShadowTable[index];
    currentEntry = (entry->Generation == (ULONG)generation) ? TRUE : FALSE;

    if (currentEntry == FALSE)
    {
        InterlockedIncrement64(&g_ShadowTableEntries);
    }
    else if ((entry->IdentityIndex != IdentityIndex) ||
             (entry->Offset != Offset))
    {
        InterlockedIncrement64(&g_ShadowReplacements);
    }

    entry->IdentityIndex = IdentityIndex;
    entry->WriterFileObject = WriterFileObject;
    entry->Offset = Offset;
    entry->Hash1 = Hash1;
    entry->Hash2 = Hash2;
    entry->Generation = (ULONG)generation;
    entry->WriteSequence = WriteSequence;

    KeReleaseSpinLock(&g_ShadowLock, oldIrql);
    return TRUE;
}

static
BOOLEAN
H3BLookupShadow (
    _In_ ULONG IdentityIndex,
    _In_ ULONGLONG Offset,
    _Out_ PULONGLONG Hash1,
    _Out_ PULONGLONG Hash2,
    _Out_ PULONGLONG WriteSequence,
    _Out_ PFILE_OBJECT* WriterFileObject
    )
{
    KIRQL oldIrql;
    ULONG index;
    LONG generation;
    PH3B_SHADOW_ENTRY entry;
    BOOLEAN found;

    found = FALSE;
    index = H3BShadowIndex(IdentityIndex, Offset);
    generation = InterlockedCompareExchange(&g_ShadowGeneration, 0, 0);

    KeAcquireSpinLock(&g_ShadowLock, &oldIrql);

    entry = &g_ShadowTable[index];

    if ((entry->Generation == (ULONG)generation) &&
        (entry->IdentityIndex == IdentityIndex) &&
        (entry->Offset == Offset))
    {
        *Hash1 = entry->Hash1;
        *Hash2 = entry->Hash2;
        *WriteSequence = entry->WriteSequence;
        *WriterFileObject = entry->WriterFileObject;
        found = TRUE;
    }

    KeReleaseSpinLock(&g_ShadowLock, oldIrql);
    return found;
}

static
ULONG
H3BPayloadWriteBase (
    _In_ ULONGLONG WriteSequence
    )
{
    ULONGLONG writeSlot;

    /*
     * A payload record is retained by write sequence rather than by hashing
     * its pagefile offset.  Each recent write gets four dedicated page slots,
     * matching H3B_MAX_HASH_PAGES_PER_IO.  This removes direct-map collision
     * eviction between unrelated offsets.
     */
    writeSlot = WriteSequence & (H3B_PAYLOAD_WRITES_RETAINED - 1);

    return (ULONG)(writeSlot * H3B_MAX_HASH_PAGES_PER_IO);
}

static
BOOLEAN
H3BPayloadBytesEqual (
    _In_reads_bytes_(PAGE_SIZE) const UCHAR* Left,
    _In_reads_bytes_(PAGE_SIZE) const UCHAR* Right
    )
{
    ULONG i;

    for (i = 0; i < PAGE_SIZE; i++)
    {
        if (Left[i] != Right[i])
        {
            return FALSE;
        }
    }

    return TRUE;
}

static
BOOLEAN
H3BStorePayload (
    _In_ ULONG IdentityIndex,
    _In_ ULONGLONG Offset,
    _In_ ULONGLONG WriteSequence,
    _In_ ULONG WriteGeneration,
    _In_ ULONG PageOrdinal,
    _In_reads_bytes_(PAGE_SIZE) const UCHAR* Bytes
    )
{
    KIRQL oldIrql;
    ULONG index;
    LONG generation;
    PH3B_PAYLOAD_ENTRY entry;
    BOOLEAN currentEntry;

    generation = InterlockedCompareExchange(&g_ShadowGeneration, 0, 0);

    if ((ULONG)generation != WriteGeneration)
    {
        return FALSE;
    }

    if (PageOrdinal >= H3B_MAX_HASH_PAGES_PER_IO)
    {
        return FALSE;
    }

    index = H3BPayloadWriteBase(WriteSequence) + PageOrdinal;

    KeAcquireSpinLock(&g_PayloadLock, &oldIrql);

    entry = &g_PayloadTable[index];
    currentEntry = (entry->Generation == (ULONG)generation) ? TRUE : FALSE;

    if (currentEntry &&
        ((entry->IdentityIndex != IdentityIndex) ||
         (entry->Offset != Offset) ||
         (entry->WriteSequence != WriteSequence)))
    {
        InterlockedIncrement64(&g_PayloadReplacements);
    }

    entry->IdentityIndex = IdentityIndex;
    entry->Generation = (ULONG)generation;
    entry->Offset = Offset;
    entry->WriteSequence = WriteSequence;
    RtlCopyMemory(entry->Bytes, Bytes, PAGE_SIZE);

    KeReleaseSpinLock(&g_PayloadLock, oldIrql);
    return TRUE;
}

static
BOOLEAN
H3BComparePayload (
    _In_ ULONG IdentityIndex,
    _In_ ULONGLONG Offset,
    _In_ ULONGLONG WriteSequence,
    _In_reads_bytes_(PAGE_SIZE) const UCHAR* Bytes,
    _Out_ PBOOLEAN Match
    )
{
    KIRQL oldIrql;
    ULONG baseIndex;
    ULONG i;
    LONG generation;
    PH3B_PAYLOAD_ENTRY entry;
    BOOLEAN found;

    found = FALSE;
    *Match = FALSE;

    generation = InterlockedCompareExchange(&g_ShadowGeneration, 0, 0);
    baseIndex = H3BPayloadWriteBase(WriteSequence);

    KeAcquireSpinLock(&g_PayloadLock, &oldIrql);

    for (i = 0; i < H3B_MAX_HASH_PAGES_PER_IO; i++)
    {
        entry = &g_PayloadTable[baseIndex + i];

        if ((entry->Generation == (ULONG)generation) &&
            (entry->IdentityIndex == IdentityIndex) &&
            (entry->Offset == Offset) &&
            (entry->WriteSequence == WriteSequence))
        {
            *Match = H3BPayloadBytesEqual(entry->Bytes, Bytes);
            found = TRUE;
            break;
        }
    }

    KeReleaseSpinLock(&g_PayloadLock, oldIrql);
    return found;
}

static
PVOID
H3BGetReadBuffer (
    _Inout_ PFLT_CALLBACK_DATA Data
    )
{
    PMDL mdl;

    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_NEW_SYSTEM_BUFFER))
    {
        PVOID newSystemBuffer;

        newSystemBuffer = FltGetNewSystemBufferAddress(Data);

        if (newSystemBuffer != NULL)
        {
            InterlockedIncrement64(&g_ReadNewSystemBuffers);
            return newSystemBuffer;
        }

        return NULL;
    }

    mdl = Data->Iopb->Parameters.Read.MdlAddress;

    if (mdl != NULL)
    {
        return MmGetSystemAddressForMdlSafe(
            mdl,
            NormalPagePriority);
    }

    if (FLT_IS_SYSTEM_BUFFER(Data))
    {
        return Data->Iopb->Parameters.Read.ReadBuffer;
    }

    /*
     * Paging-file completions on the target machine arrive at DISPATCH_LEVEL.
     * At elevated IRQL we only touch MDL-backed or system-buffered memory.
     * Raw buffer addresses are only considered at IRQL <= APC_LEVEL.
     */
    if (KeGetCurrentIrql() > APC_LEVEL)
    {
        return NULL;
    }

    if (FLT_IS_FASTIO_OPERATION(Data) ||
        (Data->RequestorMode == KernelMode))
    {
        return Data->Iopb->Parameters.Read.ReadBuffer;
    }

    return NULL;
}

static
PVOID
H3BGetWriteBuffer (
    _Inout_ PFLT_CALLBACK_DATA Data
    )
{
    PMDL mdl;

    mdl = Data->Iopb->Parameters.Write.MdlAddress;

    if (mdl != NULL)
    {
        return MmGetSystemAddressForMdlSafe(
            mdl,
            NormalPagePriority);
    }

    if (FLT_IS_SYSTEM_BUFFER(Data))
    {
        return Data->Iopb->Parameters.Write.WriteBuffer;
    }

    /*
     * Paging-file completions on the target machine arrive at DISPATCH_LEVEL.
     * At elevated IRQL we only touch MDL-backed or system-buffered memory.
     * Raw buffer addresses are only considered at IRQL <= APC_LEVEL.
     */
    if (KeGetCurrentIrql() > APC_LEVEL)
    {
        return NULL;
    }

    if (FLT_IS_FASTIO_OPERATION(Data) ||
        (Data->RequestorMode == KernelMode))
    {
        return Data->Iopb->Parameters.Write.WriteBuffer;
    }

    return NULL;
}

static
PVOID
H3BGetReadBufferForServe (
    _Inout_ PFLT_CALLBACK_DATA Data
    )
{
    PMDL mdl;

    /*
     * Intervention never touches a raw user virtual address.  Paging I/O
     * normally arrives with an MDL; a system buffer is also safe to write.
     * Anything else falls through to the real pagefile I/O unchanged.
     */
    mdl = Data->Iopb->Parameters.Read.MdlAddress;

    if (mdl != NULL)
    {
        return MmGetSystemAddressForMdlSafe(
            mdl,
            NormalPagePriority);
    }

    if (FLT_IS_SYSTEM_BUFFER(Data))
    {
        return Data->Iopb->Parameters.Read.ReadBuffer;
    }

    return NULL;
}

static
BOOLEAN
H3BTryServeSinglePageRead (
    _Inout_ PFLT_CALLBACK_DATA Data
    )
{
    LONGLONG signedOffset;
    ULONGLONG offset;
    ULONGLONG expected1;
    ULONGLONG expected2;
    ULONGLONG payloadHash1;
    ULONGLONG payloadHash2;
    ULONGLONG writeSequence;
    PFILE_OBJECT writerFileObject;
    ULONG identityIndex;
    PUCHAR payloadScratch;
    PVOID destination;
    ULONG baseIndex;
    ULONG i;
    KIRQL pagefileIrql;
    KIRQL payloadIrql;
    PH3B_PAYLOAD_ENTRY entry;
    BOOLEAN payloadFound;
    BOOLEAN copySucceeded;
    BOOLEAN armConsumed;

    if (InterlockedCompareExchange(&g_InterventionArmed, 0, 0) != 1)
    {
        return FALSE;
    }

    if (!FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_IRP_OPERATION) ||
        !FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO) ||
        (Data->RequestorMode != KernelMode) ||
        (Data->Iopb->Parameters.Read.Length != PAGE_SIZE))
    {
        return FALSE;
    }

    signedOffset = Data->Iopb->Parameters.Read.ByteOffset.QuadPart;

    if ((signedOffset < 0) ||
        (((ULONGLONG)signedOffset & (PAGE_SIZE - 1)) != 0))
    {
        return FALSE;
    }

    InterlockedIncrement64(&g_InterventionAttempts);

    if (!H3BGetPagefileIdentityIndex(
            Data->Iopb->TargetFileObject,
            &identityIndex))
    {
        InterlockedIncrement64(&g_InterventionFallbacks);
        return FALSE;
    }

    offset = (ULONGLONG)signedOffset;
    writerFileObject = NULL;

    if (!H3BLookupShadow(
            identityIndex,
            offset,
            &expected1,
            &expected2,
            &writeSequence,
            &writerFileObject))
    {
        InterlockedIncrement64(&g_InterventionFallbacks);
        return FALSE;
    }

    if (!H3BHistoryAllowsVerify(
            identityIndex,
            offset,
            writeSequence))
    {
        InterlockedIncrement64(&g_InterventionFallbacks);
        return FALSE;
    }

#pragma warning(push)
#pragma warning(disable:4996)
    payloadScratch = (PUCHAR)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        PAGE_SIZE,
        H3B_POOL_TAG);
#pragma warning(pop)

    if (payloadScratch == NULL)
    {
        InterlockedIncrement64(&g_InterventionBufferUnavailable);
        InterlockedIncrement64(&g_InterventionFallbacks);
        return FALSE;
    }

    payloadFound = FALSE;
    baseIndex = H3BPayloadWriteBase(writeSequence);

    KeAcquireSpinLock(&g_PayloadLock, &payloadIrql);

    for (i = 0; i < H3B_MAX_HASH_PAGES_PER_IO; i++)
    {
        entry = &g_PayloadTable[baseIndex + i];

        if ((entry->Generation ==
                (ULONG)InterlockedCompareExchange(
                    &g_ShadowGeneration, 0, 0)) &&
            (entry->IdentityIndex == identityIndex) &&
            (entry->Offset == offset) &&
            (entry->WriteSequence == writeSequence))
        {
            RtlCopyMemory(
                payloadScratch,
                entry->Bytes,
                PAGE_SIZE);
            payloadFound = TRUE;
            break;
        }
    }

    KeReleaseSpinLock(&g_PayloadLock, payloadIrql);

    if (!payloadFound)
    {
        ExFreePoolWithTag(payloadScratch, H3B_POOL_TAG);
        InterlockedIncrement64(&g_InterventionPayloadMisses);
        InterlockedIncrement64(&g_InterventionFallbacks);
        return FALSE;
    }

    payloadHash1 = H3BHashPage(
        payloadScratch,
        0x9E3779B97F4A7C15ULL);
    payloadHash2 = H3BHashPage(
        payloadScratch,
        0xD6E8FEB86659FD93ULL);

    if ((payloadHash1 != expected1) ||
        (payloadHash2 != expected2))
    {
        ExFreePoolWithTag(payloadScratch, H3B_POOL_TAG);
        InterlockedExchange(&g_InterventionArmed, 0);
        InterlockedIncrement64(&g_InterventionSafetyRejects);
        InterlockedIncrement64(&g_InterventionFallbacks);
        return FALSE;
    }

    destination = H3BGetReadBufferForServe(Data);

    if (destination == NULL)
    {
        ExFreePoolWithTag(payloadScratch, H3B_POOL_TAG);
        InterlockedIncrement64(&g_InterventionBufferUnavailable);
        InterlockedIncrement64(&g_InterventionFallbacks);
        return FALSE;
    }

    copySucceeded = FALSE;
    armConsumed = FALSE;

    /*
     * Final proof is performed while holding the pagefile-history lock.
     * We then hold the payload lock as well while copying the exact retained
     * page.  No newer overlapping write can be recorded between this final
     * history proof and completion of the one-page read.
     */
    KeAcquireSpinLock(&g_PagefileLock, &pagefileIrql);

    if (!H3BHistoryAllowsServeLocked(
            identityIndex,
            offset,
            writeSequence))
    {
        KeReleaseSpinLock(&g_PagefileLock, pagefileIrql);
        ExFreePoolWithTag(payloadScratch, H3B_POOL_TAG);
        InterlockedIncrement64(&g_InterventionFallbacks);
        return FALSE;
    }

    KeAcquireSpinLock(&g_PayloadLock, &payloadIrql);

    payloadFound = FALSE;
    baseIndex = H3BPayloadWriteBase(writeSequence);

    for (i = 0; i < H3B_MAX_HASH_PAGES_PER_IO; i++)
    {
        entry = &g_PayloadTable[baseIndex + i];

        if ((entry->Generation ==
                (ULONG)InterlockedCompareExchange(
                    &g_ShadowGeneration, 0, 0)) &&
            (entry->IdentityIndex == identityIndex) &&
            (entry->Offset == offset) &&
            (entry->WriteSequence == writeSequence))
        {
            payloadFound = TRUE;
            break;
        }
    }

    if (payloadFound &&
        (InterlockedCompareExchange(
            &g_InterventionArmed,
            0,
            1) == 1))
    {
        armConsumed = TRUE;
        InterlockedIncrement64(&g_InterventionEligible);

        __try
        {
            RtlCopyMemory(
                destination,
                entry->Bytes,
                PAGE_SIZE);

            Data->IoStatus.Status = STATUS_SUCCESS;
            Data->IoStatus.Information = PAGE_SIZE;
            copySucceeded = TRUE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            copySucceeded = FALSE;
        }
    }

    KeReleaseSpinLock(&g_PayloadLock, payloadIrql);
    KeReleaseSpinLock(&g_PagefileLock, pagefileIrql);

    ExFreePoolWithTag(payloadScratch, H3B_POOL_TAG);

    if (!copySucceeded)
    {
        /*
         * If the arm was consumed but the destination copy failed, leave the
         * experiment disarmed and pass the request through.  The real
         * pagefile read will overwrite any partial destination bytes.
         */
        if (armConsumed)
        {
            InterlockedIncrement64(&g_InterventionSafetyRejects);
        }

        InterlockedIncrement64(&g_InterventionFallbacks);
        return FALSE;
    }

    InterlockedIncrement64(&g_InterventionServed);
    InterlockedAdd64(&g_InterventionBytes, PAGE_SIZE);

    return TRUE;
}

static
VOID
H3BCapturePreWriteHashes (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _Inout_ PH3B_WRITE_CONTEXT Context
    )
{
    PVOID mappedBuffer;
    PUCHAR bytes;
    LONGLONG signedOffset;
    ULONG length;
    ULONG pageCount;
    ULONG i;

    Context->PageCount = 0;
    Context->PreHashValid = FALSE;

    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        return;
    }

    signedOffset = Data->Iopb->Parameters.Write.ByteOffset.QuadPart;
    length = Data->Iopb->Parameters.Write.Length;

    if ((signedOffset < 0) ||
        (length == 0) ||
        (((ULONGLONG)signedOffset & (PAGE_SIZE - 1)) != 0) ||
        ((length & (PAGE_SIZE - 1)) != 0))
    {
        return;
    }

    mappedBuffer = H3BGetWriteBuffer(Data);
    if (mappedBuffer == NULL)
    {
        return;
    }

    bytes = (PUCHAR)mappedBuffer;
    pageCount = length / PAGE_SIZE;
    if (pageCount > H3B_MAX_HASH_PAGES_PER_IO)
    {
        pageCount = H3B_MAX_HASH_PAGES_PER_IO;
    }

    __try
    {
        for (i = 0; i < pageCount; i++)
        {
            Context->Hash1[i] = H3BHashPage(
                bytes + ((SIZE_T)i * PAGE_SIZE),
                0x9E3779B97F4A7C15ULL);

            Context->Hash2[i] = H3BHashPage(
                bytes + ((SIZE_T)i * PAGE_SIZE),
                0xD6E8FEB86659FD93ULL);
        }

        Context->PageCount = pageCount;
        Context->PreHashValid = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Context->PageCount = 0;
        Context->PreHashValid = FALSE;
    }
}

static
VOID
H3BShadowCompletedWrite (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PH3B_WRITE_CONTEXT Context
    )
{
    PVOID mappedBuffer;
    PUCHAR bytes;
    PUCHAR payloadScratch;
    ULONGLONG baseOffset;
    ULONGLONG writeSequence;
    ULONG_PTR completedBytes;
    ULONG pageCount;
    ULONG i;
    ULONG writeGeneration;
    ULONG identityIndex;

    payloadScratch = NULL;
    writeSequence = Context->Sequence;

    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        InterlockedIncrement64(&g_ShadowHighIrqlSkips);
        return;
    }

    if (!H3BGetPagefileIdentityIndex(
            Data->Iopb->TargetFileObject,
            &identityIndex))
    {
        return;
    }

    if (Data->Iopb->Parameters.Write.ByteOffset.QuadPart < 0)
    {
        InterlockedIncrement64(&g_ShadowUnaligned);
        return;
    }

    baseOffset = (ULONGLONG)Data->Iopb->Parameters.Write.ByteOffset.QuadPart;
    completedBytes = Data->IoStatus.Information;

    if ((completedBytes == 0) ||
        ((baseOffset & (PAGE_SIZE - 1)) != 0) ||
        ((completedBytes & (PAGE_SIZE - 1)) != 0))
    {
        InterlockedIncrement64(&g_ShadowUnaligned);
        return;
    }

    if (!H3BHistoryAllowsPublish(
            identityIndex,
            baseOffset,
            writeSequence,
            &writeGeneration))
    {
        InterlockedIncrement64(&g_ShadowPublishSkipped);
        return;
    }

    mappedBuffer = H3BGetWriteBuffer(Data);

    if (mappedBuffer == NULL)
    {
        if (KeGetCurrentIrql() > APC_LEVEL)
        {
            InterlockedIncrement64(&g_ShadowHighIrqlSkips);
        }
        else
        {
            InterlockedIncrement64(&g_ShadowBufferUnavailable);
        }
        return;
    }

    if (!Context->PreHashValid || (Context->PageCount == 0))
    {
        InterlockedIncrement64(&g_ShadowPublishSkipped);
        return;
    }

    bytes = (PUCHAR)mappedBuffer;

#pragma warning(push)
#pragma warning(disable:4996)
    payloadScratch = (PUCHAR)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        PAGE_SIZE,
        H3B_POOL_TAG);
#pragma warning(pop)

    if (payloadScratch == NULL)
    {
        InterlockedIncrement64(&g_PayloadBufferUnavailable);
    }

    pageCount = (ULONG)(completedBytes / PAGE_SIZE);

    if (pageCount > H3B_MAX_HASH_PAGES_PER_IO)
    {
        pageCount = H3B_MAX_HASH_PAGES_PER_IO;
    }

    if (pageCount > Context->PageCount)
    {
        pageCount = Context->PageCount;
    }

    __try
    {
        for (i = 0; i < pageCount; i++)
        {
            ULONGLONG postHash1;
            ULONGLONG postHash2;
            ULONGLONG offset;

            postHash1 = H3BHashPage(
                bytes + ((SIZE_T)i * PAGE_SIZE),
                0x9E3779B97F4A7C15ULL);

            postHash2 = H3BHashPage(
                bytes + ((SIZE_T)i * PAGE_SIZE),
                0xD6E8FEB86659FD93ULL);

            if ((postHash1 != Context->Hash1[i]) ||
                (postHash2 != Context->Hash2[i]))
            {
                InterlockedIncrement64(&g_ShadowPublishSkipped);
                continue;
            }

            offset = baseOffset + ((ULONGLONG)i * PAGE_SIZE);

            if (!H3BHistoryAllowsPublish(
                    identityIndex,
                    offset,
                    writeSequence,
                    &writeGeneration))
            {
                InterlockedIncrement64(&g_ShadowPublishSkipped);
                continue;
            }

            if (H3BStoreShadow(
                    identityIndex,
                    Data->Iopb->TargetFileObject,
                    offset,
                    Context->Hash1[i],
                    Context->Hash2[i],
                    writeSequence,
                    writeGeneration))
            {
                InterlockedIncrement64(&g_ShadowWritePages);

                if (payloadScratch != NULL)
                {
                    ULONGLONG payloadHash1;
                    ULONGLONG payloadHash2;

                    RtlCopyMemory(
                        payloadScratch,
                        bytes + ((SIZE_T)i * PAGE_SIZE),
                        PAGE_SIZE);

                    payloadHash1 = H3BHashPage(
                        payloadScratch,
                        0x9E3779B97F4A7C15ULL);

                    payloadHash2 = H3BHashPage(
                        payloadScratch,
                        0xD6E8FEB86659FD93ULL);

                    if ((payloadHash1 == Context->Hash1[i]) &&
                        (payloadHash2 == Context->Hash2[i]) &&
                        H3BStorePayload(
                            identityIndex,
                            offset,
                            writeSequence,
                            writeGeneration,
                            i,
                            payloadScratch))
                    {
                        InterlockedIncrement64(&g_PayloadWritePages);
                    }
                    else
                    {
                        InterlockedIncrement64(&g_PayloadCaptureSkipped);
                    }
                }
            }
            else
            {
                InterlockedIncrement64(&g_ShadowPublishSkipped);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedIncrement64(&g_ShadowBufferUnavailable);
    }

    if (payloadScratch != NULL)
    {
        ExFreePoolWithTag(
            payloadScratch,
            H3B_POOL_TAG);
    }
}

static
VOID
H3BVerifyCompletedRead (
    _Inout_ PFLT_CALLBACK_DATA Data
    )
{
    PVOID mappedBuffer;
    PUCHAR bytes;
    PUCHAR payloadScratch;
    ULONGLONG baseOffset;
    ULONG_PTR completedBytes;
    ULONG pageCount;
    ULONG i;
    ULONG identityIndex;
    BOOLEAN newSystemBufferRead;

    payloadScratch = NULL;

    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        InterlockedIncrement64(&g_ShadowHighIrqlSkips);
        return;
    }

    newSystemBufferRead =
        FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_NEW_SYSTEM_BUFFER) ?
        TRUE : FALSE;

    if (!H3BGetPagefileIdentityIndex(
            Data->Iopb->TargetFileObject,
            &identityIndex))
    {
        return;
    }

    if (Data->Iopb->Parameters.Read.ByteOffset.QuadPart < 0)
    {
        InterlockedIncrement64(&g_ShadowUnaligned);
        return;
    }

    baseOffset = (ULONGLONG)Data->Iopb->Parameters.Read.ByteOffset.QuadPart;
    completedBytes = Data->IoStatus.Information;

    if ((completedBytes == 0) ||
        ((baseOffset & (PAGE_SIZE - 1)) != 0) ||
        ((completedBytes & (PAGE_SIZE - 1)) != 0))
    {
        InterlockedIncrement64(&g_ShadowUnaligned);
        return;
    }

    mappedBuffer = H3BGetReadBuffer(Data);

    if (mappedBuffer == NULL)
    {
        if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_NEW_SYSTEM_BUFFER))
        {
            InterlockedIncrement64(&g_ShadowBufferUnavailable);
        }
        else if (KeGetCurrentIrql() > APC_LEVEL)
        {
            InterlockedIncrement64(&g_ShadowHighIrqlSkips);
        }
        else
        {
            InterlockedIncrement64(&g_ShadowBufferUnavailable);
        }
        return;
    }

    bytes = (PUCHAR)mappedBuffer;

#pragma warning(push)
#pragma warning(disable:4996)
    payloadScratch = (PUCHAR)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        PAGE_SIZE,
        H3B_POOL_TAG);
#pragma warning(pop)

    if (payloadScratch == NULL)
    {
        InterlockedIncrement64(&g_PayloadBufferUnavailable);
    }

    pageCount = (ULONG)(completedBytes / PAGE_SIZE);

    if (pageCount > H3B_MAX_HASH_PAGES_PER_IO)
    {
        pageCount = H3B_MAX_HASH_PAGES_PER_IO;
    }

    __try
    {
        for (i = 0; i < pageCount; i++)
        {
            ULONGLONG expected1;
            ULONGLONG expected2;
            ULONGLONG actual1;
            ULONGLONG actual2;
            ULONGLONG offset;
            ULONGLONG writeSequence;
            PFILE_OBJECT writerFileObject;
            BOOLEAN crossObject;

            offset = baseOffset + ((ULONGLONG)i * PAGE_SIZE);
            writerFileObject = NULL;

            if (!H3BLookupShadow(
                    identityIndex,
                    offset,
                    &expected1,
                    &expected2,
                    &writeSequence,
                    &writerFileObject))
            {
                InterlockedIncrement64(&g_ShadowUntracked);
                continue;
            }

            actual1 = H3BHashPage(
                bytes + ((SIZE_T)i * PAGE_SIZE),
                0x9E3779B97F4A7C15ULL);

            actual2 = H3BHashPage(
                bytes + ((SIZE_T)i * PAGE_SIZE),
                0xD6E8FEB86659FD93ULL);

            if (!H3BHistoryAllowsVerify(
                    identityIndex,
                    offset,
                    writeSequence))
            {
                InterlockedIncrement64(&g_ShadowVerifyInvalidated);
                InterlockedIncrement64(&g_ShadowUntracked);
                continue;
            }

            crossObject =
                (writerFileObject != Data->Iopb->TargetFileObject) ?
                TRUE : FALSE;

            if (crossObject)
            {
                InterlockedIncrement64(&g_CrossObjectComparisons);
            }

            if (newSystemBufferRead)
            {
                InterlockedIncrement64(&g_NewSystemBufferComparisons);
            }

            InterlockedIncrement64(&g_ShadowReadPages);

            if ((actual1 == expected1) &&
                (actual2 == expected2))
            {
                InterlockedIncrement64(&g_ShadowMatches);

                if (payloadScratch != NULL)
                {
                    BOOLEAN payloadMatch;

                    RtlCopyMemory(
                        payloadScratch,
                        bytes + ((SIZE_T)i * PAGE_SIZE),
                        PAGE_SIZE);

                    if (H3BComparePayload(
                            identityIndex,
                            offset,
                            writeSequence,
                            payloadScratch,
                            &payloadMatch))
                    {
                        InterlockedIncrement64(&g_PayloadReadPages);

                        if (payloadMatch)
                        {
                            InterlockedIncrement64(&g_PayloadMatches);
                        }
                        else
                        {
                            InterlockedIncrement64(&g_PayloadMismatches);
                        }
                    }
                    else
                    {
                        InterlockedIncrement64(&g_PayloadLookupMisses);
                    }
                }

                if (crossObject)
                {
                    InterlockedIncrement64(&g_CrossObjectMatches);
                }
            }
            else
            {
                InterlockedIncrement64(&g_ShadowMismatches);

                if (crossObject)
                {
                    InterlockedIncrement64(&g_CrossObjectMismatches);
                }

                if (newSystemBufferRead)
                {
                    InterlockedIncrement64(&g_NewSystemBufferMismatches);
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedIncrement64(&g_ShadowBufferUnavailable);
    }

    if (payloadScratch != NULL)
    {
        ExFreePoolWithTag(
            payloadScratch,
            H3B_POOL_TAG);
    }
}

FLT_PREOP_CALLBACK_STATUS
H3BPreRead (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    ULONG length;

    *CompletionContext = NULL;

    if (!H3BEnsureKnownPagefile(Data, FltObjects))
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    length = Data->Iopb->Parameters.Read.Length;

    InterlockedIncrement64(&g_PagefileReads);
    InterlockedAdd64(&g_PagefileReadBytes, length);

    if (H3BTryServeSinglePageRead(Data))
    {
        return FLT_PREOP_COMPLETE;
    }

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS
H3BPostRead (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (NT_SUCCESS(Data->IoStatus.Status) &&
        (Data->IoStatus.Information != 0))
    {
        H3BVerifyCompletedRead(Data);
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}

FLT_PREOP_CALLBACK_STATUS
H3BPreWrite (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    ULONG length;
    ULONGLONG writeSequence;
    PH3B_WRITE_CONTEXT writeContext;

    if (!H3BEnsureKnownPagefile(Data, FltObjects))
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    length = Data->Iopb->Parameters.Write.Length;

#pragma warning(push)
#pragma warning(disable:4996)
    writeContext = (PH3B_WRITE_CONTEXT)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        sizeof(H3B_WRITE_CONTEXT),
        H3B_POOL_TAG);
#pragma warning(pop)

    if (writeContext == NULL)
    {
        InterlockedIncrement64(&g_ShadowBufferUnavailable);
        InterlockedIncrement64(&g_PagefileWrites);
        InterlockedAdd64(&g_PagefileWriteBytes, length);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    RtlZeroMemory(writeContext, sizeof(*writeContext));

    if (!H3BRecordWriteRange(
            Data->Iopb->TargetFileObject,
            Data->Iopb->Parameters.Write.ByteOffset.QuadPart,
            length,
            &writeSequence))
    {
        ExFreePoolWithTag(
            writeContext,
            H3B_POOL_TAG);

        InterlockedIncrement64(&g_PagefileWrites);
        InterlockedAdd64(&g_PagefileWriteBytes, length);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    writeContext->Sequence = writeSequence;
    H3BCapturePreWriteHashes(Data, writeContext);
    *CompletionContext = writeContext;

    InterlockedIncrement64(&g_PagefileWrites);
    InterlockedAdd64(&g_PagefileWriteBytes, length);

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS
H3BPostWrite (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    PH3B_WRITE_CONTEXT writeContext;
    ULONGLONG writeSequence;

    UNREFERENCED_PARAMETER(FltObjects);

    writeContext = (PH3B_WRITE_CONTEXT)CompletionContext;
    if (writeContext == NULL)
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    writeSequence = writeContext->Sequence;

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
    {
        ExFreePoolWithTag(
            writeContext,
            H3B_POOL_TAG);
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (!H3BMarkWriteComplete(
            Data->Iopb->TargetFileObject,
            writeSequence,
            NULL))
    {
        /*
         * The bounded history no longer retains this write (or its pagefile
         * identity vanished).  It cannot safely publish a sample.
         */
        if (NT_SUCCESS(Data->IoStatus.Status) &&
            (Data->IoStatus.Information != 0))
        {
            InterlockedIncrement64(&g_ShadowPublishSkipped);
        }

        ExFreePoolWithTag(
            writeContext,
            H3B_POOL_TAG);
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (NT_SUCCESS(Data->IoStatus.Status) &&
        (Data->IoStatus.Information != 0))
    {
        H3BShadowCompletedWrite(Data, writeContext);
    }

    ExFreePoolWithTag(
        writeContext,
        H3B_POOL_TAG);

    return FLT_POSTOP_FINISHED_PROCESSING;
}

FLT_POSTOP_CALLBACK_STATUS
H3BPostCreate (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (NT_SUCCESS(Data->IoStatus.Status) &&
        FlagOn(Data->Iopb->OperationFlags, SL_OPEN_PAGING_FILE) &&
        (FltObjects->FileObject != NULL))
    {
        (VOID)H3BRememberPagefile(
            FltObjects->FileObject,
            FltObjects->Instance);
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}

NTSTATUS
H3BInstanceSetup (
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
    )
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeDeviceType);

    if (VolumeFilesystemType != FLT_FSTYPE_NTFS)
    {
        return STATUS_FLT_DO_NOT_ATTACH;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
H3BInstanceQueryTeardown (
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);

    return STATUS_SUCCESS;
}

NTSTATUS
H3BConnect (
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID* ConnectionPortCookie
    )
{
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);

    *ConnectionPortCookie = NULL;

    if (InterlockedCompareExchangePointer(
            (PVOID volatile*)&g_ClientPort,
            ClientPort,
            NULL) != NULL)
    {
        return STATUS_DEVICE_BUSY;
    }

    return STATUS_SUCCESS;
}

VOID
H3BDisconnect (
    _In_opt_ PVOID ConnectionCookie
    )
{
    UNREFERENCED_PARAMETER(ConnectionCookie);

    if (g_ClientPort != NULL)
    {
        FltCloseClientPort(g_Filter, &g_ClientPort);
    }
}

NTSTATUS
H3BMessage (
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    )
{
    PH3B_COMMAND command;
    PH3B_COUNTERS reply;

    UNREFERENCED_PARAMETER(PortCookie);

    *ReturnOutputBufferLength = 0;

    if ((InputBuffer == NULL) ||
        (InputBufferLength < sizeof(H3B_COMMAND)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    command = (PH3B_COMMAND)InputBuffer;

    if (command->Version != H3B_PROTOCOL_VERSION)
    {
        return STATUS_REVISION_MISMATCH;
    }

    if (command->Command == H3B_COMMAND_RESET)
    {
        H3BResetCounters();
    }
    else if (command->Command == H3B_COMMAND_ARM_ONCE)
    {
        if ((H3BReadCounter(&g_ShadowMismatches) != 0) ||
            (H3BReadCounter(&g_PayloadMismatches) != 0) ||
            (H3BReadCounter(&g_HistoryRecordDrops) != 0) ||
            (H3BReadCounter(&g_DroppedInflightOutstanding) != 0) ||
            (InterlockedCompareExchange(&g_TrackingCompromised, 0, 0) != 0))
        {
            return STATUS_INVALID_DEVICE_STATE;
        }

        InterlockedExchange(&g_InterventionArmed, 1);
    }
    else if (command->Command == H3B_COMMAND_DISARM)
    {
        InterlockedExchange(&g_InterventionArmed, 0);
    }
    else if (command->Command != H3B_COMMAND_QUERY)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((OutputBuffer == NULL) ||
        (OutputBufferLength < sizeof(H3B_COUNTERS)))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    reply = (PH3B_COUNTERS)OutputBuffer;
    RtlZeroMemory(reply, sizeof(*reply));

    reply->Version = H3B_PROTOCOL_VERSION;
    reply->Size = sizeof(*reply);

    reply->PagefileReads = H3BReadCounter(&g_PagefileReads);
    reply->PagefileReadBytes = H3BReadCounter(&g_PagefileReadBytes);
    reply->PagefileWrites = H3BReadCounter(&g_PagefileWrites);
    reply->PagefileWriteBytes = H3BReadCounter(&g_PagefileWriteBytes);
    reply->PagingFileCreates = H3BReadCounter(&g_PagingFileCreates);

    reply->ShadowWritePages = H3BReadCounter(&g_ShadowWritePages);
    reply->ShadowReadPages = H3BReadCounter(&g_ShadowReadPages);
    reply->ShadowMatches = H3BReadCounter(&g_ShadowMatches);
    reply->ShadowMismatches = H3BReadCounter(&g_ShadowMismatches);
    reply->ShadowUntracked = H3BReadCounter(&g_ShadowUntracked);
    reply->ShadowReplacements = H3BReadCounter(&g_ShadowReplacements);
    reply->ShadowBufferUnavailable = H3BReadCounter(&g_ShadowBufferUnavailable);
    reply->ShadowUnaligned = H3BReadCounter(&g_ShadowUnaligned);
    reply->ShadowHighIrqlSkips = H3BReadCounter(&g_ShadowHighIrqlSkips);
    reply->ShadowTableEntries = H3BReadCounter(&g_ShadowTableEntries);
    reply->ShadowTableCapacity = H3B_SHADOW_SLOTS;
    reply->ShadowPublishSkipped = H3BReadCounter(&g_ShadowPublishSkipped);
    reply->ShadowVerifyInvalidated = H3BReadCounter(&g_ShadowVerifyInvalidated);
    reply->HistoryExpired = H3BReadCounter(&g_HistoryExpired);
    reply->HistoryRecordDrops = H3BReadCounter(&g_HistoryRecordDrops);
    reply->KnownPagefiles = H3BReadCounter(&g_KnownPagefiles);
    reply->HistoryCapacity = H3B_WRITE_HISTORY_SLOTS;
    reply->PagefileTableFull = H3BReadCounter(&g_PagefileTableFull);
    reply->PagefileIdentities = H3BReadCounter(&g_PagefileIdentitiesCount);
    reply->PagefileAliases = H3BReadCounter(&g_PagefileAliases);
    reply->ReadNewSystemBuffers = H3BReadCounter(&g_ReadNewSystemBuffers);
    reply->CrossObjectComparisons = H3BReadCounter(&g_CrossObjectComparisons);
    reply->CrossObjectMatches = H3BReadCounter(&g_CrossObjectMatches);
    reply->CrossObjectMismatches = H3BReadCounter(&g_CrossObjectMismatches);
    reply->NewSystemBufferComparisons = H3BReadCounter(&g_NewSystemBufferComparisons);
    reply->NewSystemBufferMismatches = H3BReadCounter(&g_NewSystemBufferMismatches);
    reply->DynamicPagefileDiscoveries = H3BReadCounter(&g_DynamicPagefileDiscoveries);
    reply->ConcurrentOverlapSkips = H3BReadCounter(&g_ConcurrentOverlapSkips);
    reply->DroppedInflightRecords = H3BReadCounter(&g_DroppedInflightRecords);
    reply->DroppedInflightOutstanding = H3BReadCounter(&g_DroppedInflightOutstanding);

    reply->PayloadWritePages = H3BReadCounter(&g_PayloadWritePages);
    reply->PayloadReadPages = H3BReadCounter(&g_PayloadReadPages);
    reply->PayloadMatches = H3BReadCounter(&g_PayloadMatches);
    reply->PayloadMismatches = H3BReadCounter(&g_PayloadMismatches);
    reply->PayloadReplacements = H3BReadCounter(&g_PayloadReplacements);
    reply->PayloadLookupMisses = H3BReadCounter(&g_PayloadLookupMisses);
    reply->PayloadCaptureSkipped = H3BReadCounter(&g_PayloadCaptureSkipped);
    reply->PayloadBufferUnavailable = H3BReadCounter(&g_PayloadBufferUnavailable);
    reply->PayloadCapacity = H3B_PAYLOAD_SLOTS;

    reply->InterventionArmed =
        InterlockedCompareExchange(&g_InterventionArmed, 0, 0);
    reply->InterventionAttempts = H3BReadCounter(&g_InterventionAttempts);
    reply->InterventionEligible = H3BReadCounter(&g_InterventionEligible);
    reply->InterventionServed = H3BReadCounter(&g_InterventionServed);
    reply->InterventionBytes = H3BReadCounter(&g_InterventionBytes);
    reply->InterventionFallbacks = H3BReadCounter(&g_InterventionFallbacks);
    reply->InterventionPayloadMisses = H3BReadCounter(&g_InterventionPayloadMisses);
    reply->InterventionBufferUnavailable =
        H3BReadCounter(&g_InterventionBufferUnavailable);
    reply->InterventionSafetyRejects =
        H3BReadCounter(&g_InterventionSafetyRejects);

    *ReturnOutputBufferLength = sizeof(*reply);
    return STATUS_SUCCESS;
}

NTSTATUS
H3BUnload (
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(Flags);

    if (g_ServerPort != NULL)
    {
        FltCloseCommunicationPort(g_ServerPort);
        g_ServerPort = NULL;
    }

    if (g_ClientPort != NULL)
    {
        FltCloseClientPort(g_Filter, &g_ClientPort);
    }

    H3BReleasePagefiles();

    if (g_ShadowTable != NULL)
    {
        ExFreePoolWithTag(g_ShadowTable, H3B_POOL_TAG);
        g_ShadowTable = NULL;
    }

    FltUnregisterFilter(g_Filter);
    g_Filter = NULL;

    return STATUS_SUCCESS;
}

CONST FLT_OPERATION_REGISTRATION g_Operations[] =
{
    {
        IRP_MJ_CREATE,
        0,
        NULL,
        H3BPostCreate
    },
    {
        IRP_MJ_READ,
        0,
        H3BPreRead,
        H3BPostRead
    },
    {
        IRP_MJ_WRITE,
        0,
        H3BPreWrite,
        H3BPostWrite
    },
    { IRP_MJ_OPERATION_END }
};

CONST FLT_REGISTRATION g_Registration =
{
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    NULL,
    g_Operations,
    H3BUnload,
    H3BInstanceSetup,
    H3BInstanceQueryTeardown,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    PSECURITY_DESCRIPTOR securityDescriptor;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING portName;

    UNREFERENCED_PARAMETER(RegistryPath);

    g_Filter = NULL;
    g_ServerPort = NULL;
    g_ClientPort = NULL;
    g_ShadowTable = NULL;
    g_ShadowGeneration = 1;
    g_WriteSequence = 1;
    g_InterventionArmed = 0;
    g_TrackingCompromised = 0;

    RtlZeroMemory(
        g_PagefileIdentities,
        sizeof(g_PagefileIdentities));
    RtlZeroMemory(
        g_PagefileObjects,
        sizeof(g_PagefileObjects));
    RtlZeroMemory(
        g_PayloadTable,
        sizeof(g_PayloadTable));

    KeInitializeSpinLock(&g_PagefileLock);
    KeInitializeSpinLock(&g_ShadowLock);
    KeInitializeSpinLock(&g_PayloadLock);

    /*
     * ExAllocatePool2 would remove the deprecation warning in newer WDKs, but
     * it is not available on the target Windows 10 1909 build.  Keep the
     * legacy allocator deliberately for target compatibility and suppress
     * only this one WDK deprecation warning.
     */
#pragma warning(push)
#pragma warning(disable:4996)
    g_ShadowTable = (PH3B_SHADOW_ENTRY)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        sizeof(H3B_SHADOW_ENTRY) * H3B_SHADOW_SLOTS,
        H3B_POOL_TAG);
#pragma warning(pop)

    if (g_ShadowTable == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(
        g_ShadowTable,
        sizeof(H3B_SHADOW_ENTRY) * H3B_SHADOW_SLOTS);

    H3BResetCounters();

    status = FltRegisterFilter(
        DriverObject,
        &g_Registration,
        &g_Filter);

    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(g_ShadowTable, H3B_POOL_TAG);
        g_ShadowTable = NULL;
        return status;
    }

    securityDescriptor = NULL;

    status = FltBuildDefaultSecurityDescriptor(
        &securityDescriptor,
        FLT_PORT_ALL_ACCESS);

    if (!NT_SUCCESS(status))
    {
        FltUnregisterFilter(g_Filter);
        g_Filter = NULL;
        ExFreePoolWithTag(g_ShadowTable, H3B_POOL_TAG);
        g_ShadowTable = NULL;
        return status;
    }

    RtlInitUnicodeString(
        &portName,
        L"\\StateRAMH3BPort");

    InitializeObjectAttributes(
        &objectAttributes,
        &portName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        securityDescriptor);

    status = FltCreateCommunicationPort(
        g_Filter,
        &g_ServerPort,
        &objectAttributes,
        NULL,
        H3BConnect,
        H3BDisconnect,
        H3BMessage,
        1);

    FltFreeSecurityDescriptor(securityDescriptor);

    if (!NT_SUCCESS(status))
    {
        FltUnregisterFilter(g_Filter);
        g_Filter = NULL;
        ExFreePoolWithTag(g_ShadowTable, H3B_POOL_TAG);
        g_ShadowTable = NULL;
        return status;
    }

    status = FltStartFiltering(g_Filter);

    if (!NT_SUCCESS(status))
    {
        FltCloseCommunicationPort(g_ServerPort);
        g_ServerPort = NULL;
        FltUnregisterFilter(g_Filter);
        g_Filter = NULL;
        ExFreePoolWithTag(g_ShadowTable, H3B_POOL_TAG);
        g_ShadowTable = NULL;
        return status;
    }

    return STATUS_SUCCESS;
}
