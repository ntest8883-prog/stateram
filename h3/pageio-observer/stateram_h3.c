#include <fltKernel.h>

#define H3_PROTOCOL_VERSION 1
#define H3_COMMAND_QUERY    1
#define H3_COMMAND_RESET    2
#define H3_MAX_PAGEFILES    4

typedef struct _H3_COMMAND
{
    ULONG Version;
    ULONG Command;
} H3_COMMAND, *PH3_COMMAND;

typedef struct _H3_COUNTERS
{
    ULONG Version;
    ULONG Size;
    LONG64 TotalReads;
    LONG64 TotalReadBytes;
    LONG64 TotalWrites;
    LONG64 TotalWriteBytes;
    LONG64 PagingReads;
    LONG64 PagingReadBytes;
    LONG64 PagingWrites;
    LONG64 PagingWriteBytes;
    LONG64 PagefileReads;
    LONG64 PagefileReadBytes;
    LONG64 PagefileWrites;
    LONG64 PagefileWriteBytes;
    LONG64 PagingFileCreates;
} H3_COUNTERS, *PH3_COUNTERS;

PFLT_FILTER g_Filter;
PFLT_PORT g_ServerPort;
PFLT_PORT g_ClientPort;

volatile LONG64 g_TotalReads;
volatile LONG64 g_TotalReadBytes;
volatile LONG64 g_TotalWrites;
volatile LONG64 g_TotalWriteBytes;
volatile LONG64 g_PagingReads;
volatile LONG64 g_PagingReadBytes;
volatile LONG64 g_PagingWrites;
volatile LONG64 g_PagingWriteBytes;
volatile LONG64 g_PagefileReads;
volatile LONG64 g_PagefileReadBytes;
volatile LONG64 g_PagefileWrites;
volatile LONG64 g_PagefileWriteBytes;
volatile LONG64 g_PagingFileCreates;

KSPIN_LOCK g_PagefileLock;
PFILE_OBJECT g_PagefileObjects[H3_MAX_PAGEFILES];

DRIVER_INITIALIZE DriverEntry;

static
LONG64
H3ReadCounter (
    _In_ volatile LONG64* Counter
    )
{
    return InterlockedCompareExchange64(Counter, 0, 0);
}

static
VOID
H3ResetCounters (
    VOID
    )
{
    InterlockedExchange64(&g_TotalReads, 0);
    InterlockedExchange64(&g_TotalReadBytes, 0);
    InterlockedExchange64(&g_TotalWrites, 0);
    InterlockedExchange64(&g_TotalWriteBytes, 0);
    InterlockedExchange64(&g_PagingReads, 0);
    InterlockedExchange64(&g_PagingReadBytes, 0);
    InterlockedExchange64(&g_PagingWrites, 0);
    InterlockedExchange64(&g_PagingWriteBytes, 0);
    InterlockedExchange64(&g_PagefileReads, 0);
    InterlockedExchange64(&g_PagefileReadBytes, 0);
    InterlockedExchange64(&g_PagefileWrites, 0);
    InterlockedExchange64(&g_PagefileWriteBytes, 0);
    InterlockedExchange64(&g_PagingFileCreates, 0);
}

static
BOOLEAN
H3IsKnownPagefile (
    _In_ PFILE_OBJECT FileObject
    )
{
    KIRQL oldIrql;
    ULONG i;
    BOOLEAN found;

    found = FALSE;

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    for (i = 0; i < H3_MAX_PAGEFILES; i++)
    {
        if (g_PagefileObjects[i] == FileObject)
        {
            found = TRUE;
            break;
        }
    }

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);
    return found;
}

static
VOID
H3RememberPagefile (
    _In_ PFILE_OBJECT FileObject
    )
{
    KIRQL oldIrql;
    ULONG i;
    ULONG freeSlot;

    freeSlot = H3_MAX_PAGEFILES;

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    for (i = 0; i < H3_MAX_PAGEFILES; i++)
    {
        if (g_PagefileObjects[i] == FileObject)
        {
            KeReleaseSpinLock(&g_PagefileLock, oldIrql);
            return;
        }

        if ((freeSlot == H3_MAX_PAGEFILES) &&
            (g_PagefileObjects[i] == NULL))
        {
            freeSlot = i;
        }
    }

    if (freeSlot < H3_MAX_PAGEFILES)
    {
        ObReferenceObject(FileObject);
        g_PagefileObjects[freeSlot] = FileObject;
        InterlockedIncrement64(&g_PagingFileCreates);
    }

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);
}

static
VOID
H3ReleasePagefiles (
    VOID
    )
{
    KIRQL oldIrql;
    ULONG i;
    PFILE_OBJECT objects[H3_MAX_PAGEFILES];

    RtlZeroMemory(objects, sizeof(objects));

    KeAcquireSpinLock(&g_PagefileLock, &oldIrql);

    for (i = 0; i < H3_MAX_PAGEFILES; i++)
    {
        objects[i] = g_PagefileObjects[i];
        g_PagefileObjects[i] = NULL;
    }

    KeReleaseSpinLock(&g_PagefileLock, oldIrql);

    for (i = 0; i < H3_MAX_PAGEFILES; i++)
    {
        if (objects[i] != NULL)
        {
            ObDereferenceObject(objects[i]);
        }
    }
}

FLT_PREOP_CALLBACK_STATUS
H3PreRead (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    ULONG length;
    BOOLEAN paging;
    BOOLEAN pagefile;

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    length = Data->Iopb->Parameters.Read.Length;
    paging = FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO) ? TRUE : FALSE;
    pagefile = H3IsKnownPagefile(Data->Iopb->TargetFileObject);

    InterlockedIncrement64(&g_TotalReads);
    InterlockedAdd64(&g_TotalReadBytes, length);

    if (paging != FALSE)
    {
        InterlockedIncrement64(&g_PagingReads);
        InterlockedAdd64(&g_PagingReadBytes, length);
    }

    if (pagefile != FALSE)
    {
        InterlockedIncrement64(&g_PagefileReads);
        InterlockedAdd64(&g_PagefileReadBytes, length);
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

FLT_PREOP_CALLBACK_STATUS
H3PreWrite (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
    )
{
    ULONG length;
    BOOLEAN paging;
    BOOLEAN pagefile;

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    length = Data->Iopb->Parameters.Write.Length;
    paging = FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO) ? TRUE : FALSE;
    pagefile = H3IsKnownPagefile(Data->Iopb->TargetFileObject);

    InterlockedIncrement64(&g_TotalWrites);
    InterlockedAdd64(&g_TotalWriteBytes, length);

    if (paging != FALSE)
    {
        InterlockedIncrement64(&g_PagingWrites);
        InterlockedAdd64(&g_PagingWriteBytes, length);
    }

    if (pagefile != FALSE)
    {
        InterlockedIncrement64(&g_PagefileWrites);
        InterlockedAdd64(&g_PagefileWriteBytes, length);
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS
H3PostCreate (
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    if (NT_SUCCESS(Data->IoStatus.Status) &&
        FlagOn(Data->Iopb->OperationFlags, SL_OPEN_PAGING_FILE) &&
        (FltObjects->FileObject != NULL))
    {
        H3RememberPagefile(FltObjects->FileObject);
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}

NTSTATUS
H3InstanceSetup (
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
H3Connect (
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
H3Disconnect (
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
H3Message (
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    )
{
    PH3_COMMAND command;
    PH3_COUNTERS reply;

    UNREFERENCED_PARAMETER(PortCookie);

    *ReturnOutputBufferLength = 0;

    if ((InputBuffer == NULL) ||
        (InputBufferLength < sizeof(H3_COMMAND)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    command = (PH3_COMMAND)InputBuffer;

    if (command->Version != H3_PROTOCOL_VERSION)
    {
        return STATUS_REVISION_MISMATCH;
    }

    if (command->Command == H3_COMMAND_RESET)
    {
        H3ResetCounters();
    }
    else if (command->Command != H3_COMMAND_QUERY)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((OutputBuffer == NULL) ||
        (OutputBufferLength < sizeof(H3_COUNTERS)))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    reply = (PH3_COUNTERS)OutputBuffer;
    RtlZeroMemory(reply, sizeof(*reply));

    reply->Version = H3_PROTOCOL_VERSION;
    reply->Size = sizeof(*reply);
    reply->TotalReads = H3ReadCounter(&g_TotalReads);
    reply->TotalReadBytes = H3ReadCounter(&g_TotalReadBytes);
    reply->TotalWrites = H3ReadCounter(&g_TotalWrites);
    reply->TotalWriteBytes = H3ReadCounter(&g_TotalWriteBytes);
    reply->PagingReads = H3ReadCounter(&g_PagingReads);
    reply->PagingReadBytes = H3ReadCounter(&g_PagingReadBytes);
    reply->PagingWrites = H3ReadCounter(&g_PagingWrites);
    reply->PagingWriteBytes = H3ReadCounter(&g_PagingWriteBytes);
    reply->PagefileReads = H3ReadCounter(&g_PagefileReads);
    reply->PagefileReadBytes = H3ReadCounter(&g_PagefileReadBytes);
    reply->PagefileWrites = H3ReadCounter(&g_PagefileWrites);
    reply->PagefileWriteBytes = H3ReadCounter(&g_PagefileWriteBytes);
    reply->PagingFileCreates = H3ReadCounter(&g_PagingFileCreates);

    *ReturnOutputBufferLength = sizeof(*reply);
    return STATUS_SUCCESS;
}

NTSTATUS
H3Unload (
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

    H3ReleasePagefiles();

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
        H3PostCreate
    },
    {
        IRP_MJ_READ,
        0,
        H3PreRead,
        NULL
    },
    {
        IRP_MJ_WRITE,
        0,
        H3PreWrite,
        NULL
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
    H3Unload,
    H3InstanceSetup,
    NULL,
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

    g_Filter = NULL;
    g_ServerPort = NULL;
    g_ClientPort = NULL;
    RtlZeroMemory(g_PagefileObjects, sizeof(g_PagefileObjects));
    KeInitializeSpinLock(&g_PagefileLock);
    H3ResetCounters();

    status = FltRegisterFilter(
        DriverObject,
        &g_Registration,
        &g_Filter);

    if (!NT_SUCCESS(status))
    {
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
        return status;
    }

    RtlInitUnicodeString(
        &portName,
        L"\\StateRAMH3Port");

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
        H3Connect,
        H3Disconnect,
        H3Message,
        1);

    FltFreeSecurityDescriptor(securityDescriptor);

    if (!NT_SUCCESS(status))
    {
        FltUnregisterFilter(g_Filter);
        g_Filter = NULL;
        return status;
    }

    status = FltStartFiltering(g_Filter);

    if (!NT_SUCCESS(status))
    {
        FltCloseCommunicationPort(g_ServerPort);
        g_ServerPort = NULL;
        FltUnregisterFilter(g_Filter);
        g_Filter = NULL;
        return status;
    }

    return STATUS_SUCCESS;
}
