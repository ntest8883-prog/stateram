#include <fltKernel.h>

#define H3B_PROTOCOL_VERSION 3
#define H3B_COMMAND_QUERY    1
#define H3B_COMMAND_RESET    2

#define H3B_MAX_PAGEFILE_OBJECTS 16
#define H3B_MAX_PAGEFILE_IDENTITIES 4
#define H3B_WRITE_HISTORY_SLOTS 256
#define H3B_SHADOW_SLOTS     32768
#define H3B_POOL_TAG         'B3HS'
#define H3B_MAX_HASH_PAGES_PER_IO 4

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
} H3B_COUNTERS, *PH3B_COUNTERS;

C_ASSERT(sizeof(H3B_COMMAND) == 8);
C_ASSERT(sizeof(H3B_COUNTERS) == 240);

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

typedef struct _H3B_WRITE_RANGE
{
    ULONGLONG Start;
    ULONGLONG EndExclusive;
    ULONGLONG Sequence;
    ULONG Generation;
    ULONG Reserved;
} H3B_WRITE_RANGE, *PH3B_WRITE_RANGE;

typedef struct _H3B_PAGEFILE_STATE
{
    PFLT_INSTANCE Instance;
    ULONG HistoryHead;
    ULONG HistoryCount;
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
    }
}

static
LONG
H3BFindPagefileIndexLocked (
    _In_ PFILE_OBJECT FileObject
    )
{
    ULONG i;

    for (i = 0; i < H3B_MAX_PAGEFILES; i++)
    {
        if (g_Pagefiles[i].FileObject == FileObject)
        {
            return (LONG)i;
        }
    }

    return -1;
}

static
BOOLEAN
H3BIsKnownPagefile (
    _In_ PFILE_OBJECT FileObject
    )
{
    KIRQL oldIrql;
    BOOLEAN found;

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);
    found = (H3BFindPagefileIndexLocked(FileObject) >= 0) ? TRUE : FALSE;
    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    return found;
}

static
VOID
H3BRememberPagefile (
    _In_ PFILE_OBJECT FileObject
    )
{
    KIRQL oldIrql;
    ULONG i;
    ULONG freeSlot;

    freeSlot = H3B_MAX_PAGEFILES;

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    if (H3BFindPagefileIndexLocked(FileObject) >= 0)
    {
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return;
    }

    for (i = 0; i < H3B_MAX_PAGEFILES; i++)
    {
        if (g_Pagefiles[i].FileObject == NULL)
        {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot < H3B_MAX_PAGEFILES)
    {
        ObReferenceObject(FileObject);
        RtlZeroMemory(
            &g_Pagefiles[freeSlot],
            sizeof(g_Pagefiles[freeSlot]));
        g_Pagefiles[freeSlot].FileObject = FileObject;
        InterlockedIncrement64(&g_PagingFileCreates);
        InterlockedIncrement64(&g_KnownPagefiles);
    }
    else
    {
        InterlockedIncrement64(&g_PagefileTableFull);
    }

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);
}

static
VOID
H3BReleasePagefiles (
    VOID
    )
{
    KIRQL oldIrql;
    ULONG i;
    PFILE_OBJECT objects[H3B_MAX_PAGEFILES];

    RtlZeroMemory(objects, sizeof(objects));

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    for (i = 0; i < H3B_MAX_PAGEFILES; i++)
    {
        objects[i] = g_Pagefiles[i].FileObject;
        RtlZeroMemory(&g_Pagefiles[i], sizeof(g_Pagefiles[i]));
    }

    InterlockedExchange64(&g_KnownPagefiles, 0);
    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    for (i = 0; i < H3B_MAX_PAGEFILES; i++)
    {
        if (objects[i] != NULL)
        {
            ObDereferenceObject(objects[i]);
        }
    }
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
H3BRecordWriteRange (
    _In_ PFILE_OBJECT FileObject,
    _In_ LONGLONG ByteOffset,
    _In_ ULONG Length,
    _Out_ PULONGLONG Sequence
    )
{
    KIRQL oldIrql;
    LONG index;
    PH3B_PAGEFILE_STATE state;
    PH3B_WRITE_RANGE record;
    ULONGLONG start;
    ULONGLONG endExclusive;
    ULONGLONG sequence;
    ULONG generation;

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

    index = H3BFindPagefileIndexLocked(FileObject);
    if (index < 0)
    {
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    state = &g_Pagefiles[index];

    if (state->HistoryCount == H3B_WRITE_HISTORY_SLOTS)
    {
        record = &state->History[state->HistoryHead];

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
    record->Reserved = 0;

    state->HistoryHead =
        (state->HistoryHead + 1) % H3B_WRITE_HISTORY_SLOTS;

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    *Sequence = sequence;
    return TRUE;
}

static
BOOLEAN
H3BHistoryAllows (
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONGLONG PageOffset,
    _In_ ULONGLONG Sequence,
    _Out_opt_ PULONG Generation
    )
{
    KIRQL oldIrql;
    LONG index;
    PH3B_PAGEFILE_STATE state;
    ULONG i;
    BOOLEAN foundOwn;
    BOOLEAN newerOverlap;
    ULONG ownGeneration;

    foundOwn = FALSE;
    newerOverlap = FALSE;
    ownGeneration = 0;

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    index = H3BFindPagefileIndexLocked(FileObject);
    if (index < 0)
    {
        KeReleaseSpinLock(&g_PagefileLock, oldIrql);
        return FALSE;
    }

    state = &g_Pagefiles[index];

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
        }
        else if ((record->Sequence > Sequence) &&
                 H3BRangeOverlapsPage(record, PageOffset))
        {
            newerOverlap = TRUE;
        }
    }

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    if (!foundOwn || newerOverlap)
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

    if (Generation != NULL)
    {
        *Generation = ownGeneration;
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
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONGLONG Offset
    )
{
    ULONGLONG value;

    value = (Offset >> PAGE_SHIFT);
    value ^= ((ULONGLONG)(ULONG_PTR)FileObject >> 4);
    value ^= ((ULONGLONG)(ULONG_PTR)FileObject >> 19);
    value ^= (value >> 17);

    return (ULONG)(value & (H3B_SHADOW_SLOTS - 1));
}

static
BOOLEAN
H3BStoreShadow (
    _In_ PFILE_OBJECT FileObject,
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

    index = H3BShadowIndex(FileObject, Offset);
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
    else if ((entry->FileObject != FileObject) ||
             (entry->Offset != Offset))
    {
        InterlockedIncrement64(&g_ShadowReplacements);
    }

    entry->FileObject = FileObject;
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
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONGLONG Offset,
    _Out_ PULONGLONG Hash1,
    _Out_ PULONGLONG Hash2,
    _Out_ PULONGLONG WriteSequence
    )
{
    KIRQL oldIrql;
    ULONG index;
    LONG generation;
    PH3B_SHADOW_ENTRY entry;
    BOOLEAN found;

    found = FALSE;
    index = H3BShadowIndex(FileObject, Offset);
    generation = InterlockedCompareExchange(&g_ShadowGeneration, 0, 0);

    KeAcquireSpinLock(&g_ShadowLock, &oldIrql);

    entry = &g_ShadowTable[index];

    if ((entry->Generation == (ULONG)generation) &&
        (entry->FileObject == FileObject) &&
        (entry->Offset == Offset))
    {
        *Hash1 = entry->Hash1;
        *Hash2 = entry->Hash2;
        *WriteSequence = entry->WriteSequence;
        found = TRUE;
    }

    KeReleaseSpinLock(&g_ShadowLock, oldIrql);
    return found;
}

static
PVOID
H3BGetReadBuffer (
    _Inout_ PFLT_CALLBACK_DATA Data
    )
{
    PMDL mdl;

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
VOID
H3BShadowCompletedWrite (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ ULONGLONG WriteSequence
    )
{
    PVOID mappedBuffer;
    PUCHAR bytes;
    ULONGLONG baseOffset;
    ULONG_PTR completedBytes;
    ULONG pageCount;
    ULONG i;
    ULONG writeGeneration;

    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        InterlockedIncrement64(&g_ShadowHighIrqlSkips);
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

    if (!H3BHistoryAllows(
            Data->Iopb->TargetFileObject,
            baseOffset,
            WriteSequence,
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

    bytes = (PUCHAR)mappedBuffer;
    pageCount = (ULONG)(completedBytes / PAGE_SIZE);

    /*
     * H3-B is a verifier, not the final data path. Bound work done in the
     * completion callback so DISPATCH_LEVEL residence stays short.
     */
    if (pageCount > H3B_MAX_HASH_PAGES_PER_IO)
    {
        pageCount = H3B_MAX_HASH_PAGES_PER_IO;
    }

    __try
    {
        for (i = 0; i < pageCount; i++)
        {
            ULONGLONG hash1;
            ULONGLONG hash2;
            ULONGLONG offset;

            hash1 = H3BHashPage(
                bytes + ((SIZE_T)i * PAGE_SIZE),
                0x9E3779B97F4A7C15ULL);

            hash2 = H3BHashPage(
                bytes + ((SIZE_T)i * PAGE_SIZE),
                0xD6E8FEB86659FD93ULL);

            offset = baseOffset + ((ULONGLONG)i * PAGE_SIZE);

            if (!H3BHistoryAllows(
                    Data->Iopb->TargetFileObject,
                    offset,
                    WriteSequence,
                    &writeGeneration))
            {
                InterlockedIncrement64(&g_ShadowPublishSkipped);
                continue;
            }

            if (H3BStoreShadow(
                    Data->Iopb->TargetFileObject,
                    offset,
                    hash1,
                    hash2,
                    WriteSequence,
                    writeGeneration))
            {
                InterlockedIncrement64(&g_ShadowWritePages);
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
}

static
VOID
H3BVerifyCompletedRead (
    _Inout_ PFLT_CALLBACK_DATA Data
    )
{
    PVOID mappedBuffer;
    PUCHAR bytes;
    ULONGLONG baseOffset;
    ULONG_PTR completedBytes;
    ULONG pageCount;
    ULONG i;

    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        InterlockedIncrement64(&g_ShadowHighIrqlSkips);
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

    bytes = (PUCHAR)mappedBuffer;
    pageCount = (ULONG)(completedBytes / PAGE_SIZE);

    /*
     * Bound verifier work in the completion path. Matching is sampled, but a
     * mismatch remains a stop condition.
     */
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

            offset = baseOffset + ((ULONGLONG)i * PAGE_SIZE);

            if (!H3BLookupShadow(
                    Data->Iopb->TargetFileObject,
                    offset,
                    &expected1,
                    &expected2,
                    &writeSequence))
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

            if (!H3BHistoryAllows(
                    Data->Iopb->TargetFileObject,
                    offset,
                    writeSequence,
                    NULL))
            {
                InterlockedIncrement64(&g_ShadowVerifyInvalidated);
                InterlockedIncrement64(&g_ShadowUntracked);
                continue;
            }

            InterlockedIncrement64(&g_ShadowReadPages);

            if ((actual1 == expected1) &&
                (actual2 == expected2))
            {
                InterlockedIncrement64(&g_ShadowMatches);
            }
            else
            {
                InterlockedIncrement64(&g_ShadowMismatches);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedIncrement64(&g_ShadowBufferUnavailable);
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

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    if (!H3BIsKnownPagefile(Data->Iopb->TargetFileObject))
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    length = Data->Iopb->Parameters.Read.Length;

    InterlockedIncrement64(&g_PagefileReads);
    InterlockedAdd64(&g_PagefileReadBytes, length);

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

    UNREFERENCED_PARAMETER(FltObjects);

    if (!H3BIsKnownPagefile(Data->Iopb->TargetFileObject))
    {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    length = Data->Iopb->Parameters.Write.Length;

    if (!H3BRecordWriteRange(
            Data->Iopb->TargetFileObject,
            Data->Iopb->Parameters.Write.ByteOffset.QuadPart,
            length,
            &writeSequence))
    {
        InterlockedIncrement64(&g_PagefileWrites);
        InterlockedAdd64(&g_PagefileWriteBytes, length);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    *CompletionContext = (PVOID)(ULONG_PTR)writeSequence;

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
    ULONGLONG writeSequence;

    UNREFERENCED_PARAMETER(FltObjects);

    writeSequence = (ULONGLONG)(ULONG_PTR)CompletionContext;

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING))
    {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (NT_SUCCESS(Data->IoStatus.Status) &&
        (Data->IoStatus.Information != 0))
    {
        H3BShadowCompletedWrite(Data, writeSequence);
    }

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
        H3BRememberPagefile(FltObjects->FileObject);
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

    RtlZeroMemory(g_Pagefiles, sizeof(g_Pagefiles));

    KeInitializeSpinLock(&g_PagefileLock);
    KeInitializeSpinLock(&g_ShadowLock);

    g_ShadowTable = (PH3B_SHADOW_ENTRY)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        sizeof(H3B_SHADOW_ENTRY) * H3B_SHADOW_SLOTS,
        H3B_POOL_TAG);

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
