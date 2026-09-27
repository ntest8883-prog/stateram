/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shvos.c

Abstract:

    This module implements the OS-facing Windows stubs for SimpleVisor.

Author:

    Alex Ionescu (@aionescu) 29-Aug-2016 - Initial version

Environment:

    Kernel mode only.

--*/

#include <ntifs.h>
#include <stdarg.h>
#include "..\shv_x.h"
#pragma warning(disable:4221)
#pragma warning(disable:4204)

NTKERNELAPI
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
VOID
KeGenericCallDpc (
    _In_ PKDEFERRED_ROUTINE Routine,
    _In_opt_ PVOID Context
    );

NTKERNELAPI
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
VOID
KeSignalCallDpcDone (
    _In_ PVOID SystemArgument1
    );

NTKERNELAPI
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
LOGICAL
KeSignalCallDpcSynchronize (
    _In_ PVOID SystemArgument2
    );

DRIVER_INITIALIZE DriverEntry;

DECLSPEC_NORETURN
VOID
__cdecl
ShvOsRestoreContext2 (
    _In_ PCONTEXT ContextRecord,
    _In_opt_ struct _EXCEPTION_RECORD * ExceptionRecord
    );

VOID
ShvVmxCleanup (
    _In_ UINT16 Data,
    _In_ UINT16 Teb
    );

typedef struct _SHV_DPC_CONTEXT
{
    PSHV_CPU_CALLBACK Routine;
    struct _SHV_CALLBACK_CONTEXT* Context;
} SHV_DPC_CONTEXT, *PSHV_DPC_CONTEXT;

#define KGDT64_R3_DATA      0x28
#define KGDT64_R3_CMTEB     0x50

VOID
ShvOsFreeContiguousAlignedMemory (
    _In_ PVOID BaseAddress
    );

PVOID
ShvOsAllocateContigousAlignedMemory (
    _In_ SIZE_T Size
    );

ULONGLONG
ShvOsGetPhysicalAddress (
    _In_ PVOID BaseAddress
    );

PVOID g_PowerCallbackRegistration;
PVOID g_H2TargetPages[H2A_PAGE_COUNT];
PVOID g_H2CachePage;
PVOID g_H2StorePages[H2A_STORE_PAGE_COUNT];
UCHAR g_H2InitialMarker[H2A_PAGE_COUNT];

#define H2A_POISON_BYTE 0xCC
#define H2A_MARKER_BASE 3072

ULONG
ShvH2MarkerOffset (
    _In_ ULONG PageIndex
    )
{
    return H2A_MARKER_BASE + (PageIndex * 37);
}

VOID
ShvH2FreePages (
    VOID
    )
{
    ULONG i;

    if (g_H2CachePage != NULL)
    {
        ShvOsFreeContiguousAlignedMemory(g_H2CachePage);
        g_H2CachePage = NULL;
    }

    ShvH2CachePageVirtualAddress = 0;
    ShvH2CachePagePhysicalAddress = 0;

    for (i = 0; i < H2A_STORE_PAGE_COUNT; i++)
    {
        if (g_H2StorePages[i] != NULL)
        {
            ShvOsFreeContiguousAlignedMemory(g_H2StorePages[i]);
            g_H2StorePages[i] = NULL;
        }

        ShvH2StorePageVirtualAddresses[i] = 0;
    }

    for (i = 0; i < H2A_PAGE_COUNT; i++)
    {
        if (g_H2TargetPages[i] != NULL)
        {
            ShvOsFreeContiguousAlignedMemory(g_H2TargetPages[i]);
            g_H2TargetPages[i] = NULL;
        }

        ShvH1TargetPageVirtualAddresses[i] = 0;
        ShvH1TargetPagePhysicalAddresses[i] = 0;
        ShvH1BackingPageVirtualAddresses[i] = 0;
        ShvH1BackingPagePhysicalAddresses[i] = 0;
        ShvH2CompressedLength[i] = 0;
        ShvH2PageHash[i] = 0;
        g_H2InitialMarker[i] = 0;
    }
}

NTSTATUS
ShvH2PreparePages (
    VOID
    )
{
    ULONG i;
    ULONG j;
    ULONG block;
    ULONG within;
    ULONG markerOffset;
    ULONG totalCompressed;
    UINT32 compressedLength;
    UINT64 hash;
    PUCHAR targetBytes;
    PUCHAR cacheBytes;

    //
    // Allocate the exact retained H2-A backing geometry: three compressed
    // store pages plus one hot materialization cache page.
    //
    for (i = 0; i < H2A_STORE_PAGE_COUNT; i++)
    {
        g_H2StorePages[i] =
            ShvOsAllocateContigousAlignedMemory(PAGE_SIZE);
        if (g_H2StorePages[i] == NULL)
        {
            ShvH2FreePages();
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(g_H2StorePages[i], PAGE_SIZE);
        ShvH2StorePageVirtualAddresses[i] =
            (UINT64)(ULONG_PTR)g_H2StorePages[i];
    }

    g_H2CachePage =
        ShvOsAllocateContigousAlignedMemory(PAGE_SIZE);
    if (g_H2CachePage == NULL)
    {
        ShvH2FreePages();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ShvH2CachePageVirtualAddress =
        (UINT64)(ULONG_PTR)g_H2CachePage;
    ShvH2CachePagePhysicalAddress =
        ShvOsGetPhysicalAddress(g_H2CachePage);

    if (ShvH2CachePagePhysicalAddress == 0)
    {
        ShvH2FreePages();
        return STATUS_UNSUCCESSFUL;
    }

    cacheBytes = (PUCHAR)g_H2CachePage;
    totalCompressed = 0;

    for (i = 0; i < H2A_PAGE_COUNT; i++)
    {
        g_H2TargetPages[i] =
            ShvOsAllocateContigousAlignedMemory(PAGE_SIZE);
        if (g_H2TargetPages[i] == NULL)
        {
            ShvH2FreePages();
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        targetBytes = (PUCHAR)g_H2TargetPages[i];

        //
        // Build a nontrivial but compressible 4KB page: every 64-byte block
        // contains a 48-byte run followed by 16 deterministic literal bytes.
        // This is intentionally more demanding than an all-zero synthetic page.
        //
        for (j = 0; j < PAGE_SIZE; j++)
        {
            block = j >> 6;
            within = j & 63;

            if (within < 48)
            {
                targetBytes[j] =
                    (UCHAR)(0x20 +
                            ((i * 13u + block * 7u) & 0x5Fu));
            }
            else
            {
                targetBytes[j] =
                    (UCHAR)(((j * 131u) +
                             (i * 29u) +
                             (block * 17u) +
                             0x5Du) & 0xFFu);
            }
        }

        markerOffset = ShvH2MarkerOffset(i);
        g_H2InitialMarker[i] = targetBytes[markerOffset];

        ShvH1TargetPageVirtualAddresses[i] =
            (UINT64)(ULONG_PTR)g_H2TargetPages[i];
        ShvH1TargetPagePhysicalAddresses[i] =
            ShvOsGetPhysicalAddress(g_H2TargetPages[i]);

        if ((ShvH1TargetPagePhysicalAddresses[i] == 0) ||
            (ShvH1TargetPagePhysicalAddresses[i] ==
             ShvH2CachePagePhysicalAddress))
        {
            ShvH2FreePages();
            return STATUS_UNSUCCESSFUL;
        }

        hash = ShvH2HashBuffer(targetBytes, PAGE_SIZE);
        compressedLength = 0;

        if (ShvH2CompressPage(i,
                              targetBytes,
                              &compressedLength) == FALSE)
        {
            ShvH2FreePages();
            return STATUS_BUFFER_TOO_SMALL;
        }

        ShvH2CompressedLength[i] = compressedLength;
        ShvH2PageHash[i] = hash;
        totalCompressed += compressedLength;

        //
        // Immediately decode and hash-check each initial slot before VMX is
        // started. This validates the codec/store path independently.
        //
        RtlFillMemory(cacheBytes, PAGE_SIZE, H2A_POISON_BYTE);
        if ((ShvH2DecompressPage(i, cacheBytes) == FALSE) ||
            (ShvH2HashBuffer(cacheBytes, PAGE_SIZE) != hash))
        {
            ShvH2FreePages();
            return STATUS_DATA_ERROR;
        }

        //
        // Destroy the original payload. From this point forward the compressed
        // store is the authoritative copy until the page is demand-materialized
        // into the one shared cache frame.
        //
        RtlFillMemory(targetBytes, PAGE_SIZE, H2A_POISON_BYTE);
    }

    //
    // H2-A's compressed payload must fit in the exact three-page store. With
    // the one hot cache page, retained backing is exactly 16KB versus 32KB for
    // eight full 4KB backing pages. The eight target GPA frames remain allocated
    // in this controlled milestone and are NOT counted as freed Windows RAM.
    //
    if ((totalCompressed == 0) ||
        (totalCompressed > (H2A_STORE_PAGE_COUNT * PAGE_SIZE)) ||
        ((H2A_STORE_PAGE_COUNT + 1) >= H2A_PAGE_COUNT))
    {
        ShvH2FreePages();
        return STATUS_UNSUCCESSFUL;
    }

    RtlFillMemory(g_H2CachePage, PAGE_SIZE, H2A_POISON_BYTE);

    ShvH0EptTrapCount = 0;
    ShvH0LastGuestPhysicalAddress = 0;
    ShvH0LastExitQualification = 0;
    ShvH1DInveptCount = 0;
    ShvH1DInveptFailureCount = 0;
    ShvH2PageInCount = 0;
    ShvH2EvictionCount = 0;
    ShvH2CompressionCount = 0;
    ShvH2DecompressionCount = 0;
    ShvH2HashFailureCount = 0;
    ShvH2CompletedTouches = 0;
    ShvH2FlushCount = 0;

    return STATUS_SUCCESS;
}

BOOLEAN
ShvH2RunCompressionHarness (
    VOID
    )
{
    BOOLEAN result;
    INT32 cpuInfo[4];
    ULONG round;
    ULONG targetIndex;
    ULONG markerOffset;
    ULONG totalCompressed;
    ULONG expectedPageIns;
    ULONG expectedEvictions;
    KAFFINITY previousAffinity;
    UCHAR marker;
    UCHAR newMarker;
    UCHAR expectedMarker[H2A_PAGE_COUNT];
    UINT64 hash;
    UINT64 expectedHash[H2A_PAGE_COUNT];

    result = TRUE;

    for (targetIndex = 0;
         targetIndex < H2A_PAGE_COUNT;
         targetIndex++)
    {
        expectedMarker[targetIndex] =
            g_H2InitialMarker[targetIndex];
        expectedHash[targetIndex] =
            ShvH2PageHash[targetIndex];
    }

    //
    // H2-A deliberately serializes the controlled cache experiment on CPU 0.
    // Multi-CPU cache coherence belongs to the later Windows-integration gate.
    //
    previousAffinity = KeSetSystemAffinityThreadEx((KAFFINITY)1);

    for (round = 0; round < H2A_ROUNDS; round++)
    {
        for (targetIndex = 0;
             targetIndex < H2A_PAGE_COUNT;
             targetIndex++)
        {
            markerOffset = ShvH2MarkerOffset(targetIndex);

            //
            // This first byte access faults if the logical page is cold. VMX
            // root evicts/compresses the old page, decompresses this page into
            // the single cache HPA, remaps the GPA, and retries this read.
            //
            marker = *(volatile UCHAR*)
                ((PUCHAR)g_H2TargetPages[targetIndex] +
                 markerOffset);

            if (marker != expectedMarker[targetIndex])
            {
                result = FALSE;
                break;
            }

            hash =
                ShvH2HashBuffer(
                    (PUCHAR)g_H2TargetPages[targetIndex],
                    PAGE_SIZE);
            if (hash != expectedHash[targetIndex])
            {
                result = FALSE;
                break;
            }

            //
            // Dirty the resident logical page. The next different page fault
            // must recompress this modified full 4KB image before reusing the
            // cache frame.
            //
            newMarker =
                (UCHAR)(marker ^
                        (UCHAR)(0x31u +
                                ((round + targetIndex) & 0x1Fu)));

            *(volatile UCHAR*)
                ((PUCHAR)g_H2TargetPages[targetIndex] +
                 markerOffset) = newMarker;

            expectedMarker[targetIndex] = newMarker;
            expectedHash[targetIndex] =
                ShvH2HashBuffer(
                    (PUCHAR)g_H2TargetPages[targetIndex],
                    PAGE_SIZE);

            _InterlockedIncrement(&ShvH2CompletedTouches);
        }

        if (result == FALSE)
        {
            break;
        }
    }

    //
    // Force the final hot page back into compressed storage so all eight
    // logical pages are cold and represented only by their packed slots.
    //
    if (result != FALSE)
    {
        __cpuidex(cpuInfo, H2A_CPUID_FLUSH_LEAF, 0);
        if ((UINT32)cpuInfo[0] != H2A_CPUID_FLUSH_OK)
        {
            result = FALSE;
        }
    }

    //
    // Final byte-for-byte integrity pass after all repeated dirty
    // compress/decompress cycles.
    //
    if (result != FALSE)
    {
        for (targetIndex = 0;
             targetIndex < H2A_PAGE_COUNT;
             targetIndex++)
        {
            markerOffset = ShvH2MarkerOffset(targetIndex);
            marker = *(volatile UCHAR*)
                ((PUCHAR)g_H2TargetPages[targetIndex] +
                 markerOffset);

            if (marker != expectedMarker[targetIndex])
            {
                result = FALSE;
                break;
            }

            hash =
                ShvH2HashBuffer(
                    (PUCHAR)g_H2TargetPages[targetIndex],
                    PAGE_SIZE);
            if (hash != expectedHash[targetIndex])
            {
                result = FALSE;
                break;
            }
        }
    }

    if (result != FALSE)
    {
        __cpuidex(cpuInfo, H2A_CPUID_FLUSH_LEAF, 0);
        if ((UINT32)cpuInfo[0] != H2A_CPUID_FLUSH_OK)
        {
            result = FALSE;
        }
    }

    KeRevertToUserAffinityThreadEx(previousAffinity);

    if (result == FALSE)
    {
        return FALSE;
    }

    expectedPageIns =
        (H2A_ROUNDS * H2A_PAGE_COUNT) +
        H2A_PAGE_COUNT;
    expectedEvictions = expectedPageIns;

    if ((ShvH2PageInCount != (long)expectedPageIns) ||
        (ShvH2DecompressionCount != (long)expectedPageIns) ||
        (ShvH2EvictionCount != (long)expectedEvictions) ||
        (ShvH2CompressionCount != (long)expectedEvictions) ||
        (ShvH2CompletedTouches !=
         (long)(H2A_ROUNDS * H2A_PAGE_COUNT)) ||
        (ShvH2FlushCount != 2) ||
        (ShvH2HashFailureCount != 0) ||
        (ShvH1DInveptFailureCount != 0) ||
        (ShvH0EptTrapCount < (long)expectedPageIns) ||
        (ShvH1DInveptCount <
         (long)(expectedPageIns + expectedEvictions)))
    {
        return FALSE;
    }

    totalCompressed = 0;
    for (targetIndex = 0;
         targetIndex < H2A_PAGE_COUNT;
         targetIndex++)
    {
        if ((ShvH2CompressedLength[targetIndex] == 0) ||
            (ShvH2CompressedLength[targetIndex] >
             H2A_SLOT_SIZE))
        {
            return FALSE;
        }

        totalCompressed +=
            ShvH2CompressedLength[targetIndex];
    }

    if (totalCompressed >
        (H2A_STORE_PAGE_COUNT * PAGE_SIZE))
    {
        return FALSE;
    }

    return TRUE;
}


NTSTATUS
FORCEINLINE
ShvOsErrorToError (
    INT32 Error
    )
{
    //
    // Convert the possible SimpleVisor errors into NT Hyper-V Errors
    //
    if (Error == SHV_STATUS_NOT_AVAILABLE)
    {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    if (Error == SHV_STATUS_NO_RESOURCES)
    {
        return STATUS_HV_NO_RESOURCES;
    }
    if (Error == SHV_STATUS_NOT_PRESENT)
    {
        return STATUS_HV_NOT_PRESENT;
    }
    if (Error == SHV_STATUS_SUCCESS)
    {
        return STATUS_SUCCESS;
    }

    //
    // Unknown/unexpected error
    //
    return STATUS_UNSUCCESSFUL;
}

VOID
ShvOsDpcRoutine (
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    )
{
    PSHV_DPC_CONTEXT dpcContext = DeferredContext;
    UNREFERENCED_PARAMETER(Dpc);

    __analysis_assume(DeferredContext != NULL);
    __analysis_assume(SystemArgument1 != NULL);
    __analysis_assume(SystemArgument2 != NULL);

    //
    // Execute the internal callback function
    //
    dpcContext->Routine(dpcContext->Context);

    //
    // During unload SimpleVisor uses the RtlRestoreContext function which will
    // unfortunately use the "iretq" opcode in order to restore execution back.
    // This causes the processor to remove the RPL bits off the segments. As
    // the x64 kernel does not expect kernel-mode code to change the value of
    // any segments, this results in the DS and ES segments being stuck 0x20,
    // and the FS segment being stuck at 0x50, until the next context switch.
    //
    // If the DPC happened to have interrupted either the idle thread or system
    // thread, that's perfectly fine (albeit unusual). If the DPC interrupted a
    // 64-bit long-mode thread, that's also fine. However if the DPC interrupts
    // a thread in compatibility-mode, running as part of WoW64, it will hit a
    // GPF instantaneously and crash.
    //
    // Thus, set the segments to their correct value, one more time, as a fix.
    //
    ShvVmxCleanup(KGDT64_R3_DATA | RPL_MASK, KGDT64_R3_CMTEB | RPL_MASK);

    //
    // Wait for all DPCs to synchronize at this point
    //
    KeSignalCallDpcSynchronize(SystemArgument2);

    //
    // Mark the DPC as being complete
    //
    KeSignalCallDpcDone(SystemArgument1);
}

INT32
ShvOsPrepareProcessor (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // Nothing to do on NT, only return SHV_STATUS_SUCCESS
    //
    UNREFERENCED_PARAMETER(VpData);
    return SHV_STATUS_SUCCESS;
}

VOID
ShvOsUnprepareProcessor (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // When running in VMX root mode, the processor will set limits of the
    // GDT and IDT to 0xFFFF (notice that there are no Host VMCS fields to
    // set these values). This causes problems with PatchGuard, which will
    // believe that the GDTR and IDTR have been modified by malware, and
    // eventually crash the system. Since we know what the original state
    // of the GDTR and IDTR was, simply restore it now.
    //
    __lgdt(&VpData->SpecialRegisters.Gdtr.Limit);
    __lidt(&VpData->SpecialRegisters.Idtr.Limit);
}

VOID
PowerCallback (
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
    )
{
    UNREFERENCED_PARAMETER(CallbackContext);

    //
    // Ignore non-Sx changes
    //
    if (Argument1 != (PVOID)PO_CB_SYSTEM_STATE_LOCK)
    {
        return;
    }

    //
    // Check if this is S0->Sx, or Sx->S0
    //
    if (ARGUMENT_PRESENT(Argument2))
    {
        //
        // Reload the hypervisor
        //
        ShvLoad();
    }
    else
    {
        //
        // Unload the hypervisor
        //
        ShvUnload();
    }
}

VOID
ShvOsFreeContiguousAlignedMemory (
    _In_ PVOID BaseAddress
    )
{
    //
    // Free the memory
    //
    MmFreeContiguousMemory(BaseAddress);
}

PVOID
ShvOsAllocateContigousAlignedMemory (
    _In_ SIZE_T Size
    )
{
    PHYSICAL_ADDRESS lowest, highest;

    //
    // The entire address range is OK for this allocation
    //
    lowest.QuadPart = 0;
    highest.QuadPart = lowest.QuadPart - 1;

    //
    // Allocate a contiguous chunk of RAM to back this allocation and make sure
    // that it is RW only, instead of RWX, by using the new Windows 8 API.
    //
    return MmAllocateContiguousNodeMemory(Size,
                                          lowest,
                                          highest,
                                          lowest,
                                          PAGE_READWRITE,
                                          KeGetCurrentNodeNumber());
}

ULONGLONG
ShvOsGetPhysicalAddress (
    _In_ PVOID BaseAddress
    )
{
    //
    // Let the memory manager convert it
    //
    return MmGetPhysicalAddress(BaseAddress).QuadPart;
}

VOID
ShvOsRunCallbackOnProcessors (
    _In_ PSHV_CPU_CALLBACK Routine,
    _In_opt_ PVOID Context
    )
{
    SHV_DPC_CONTEXT dpcContext;

    //
    // Wrap the internal routine and context under a Windows DPC
    //
    dpcContext.Routine = Routine;
    dpcContext.Context = Context;
    KeGenericCallDpc(ShvOsDpcRoutine, &dpcContext);
}

VOID
ShvOsRestoreContext(
    _In_ PCONTEXT ContextRecord
    )
{
    ShvOsRestoreContext2(ContextRecord, NULL);
}

VOID
ShvOsCaptureContext (
    _In_ PCONTEXT ContextRecord
    )
{
    //
    // Windows provides a nice OS function to do this
    //
    RtlCaptureContext(ContextRecord);
}

INT32
ShvOsGetCurrentProcessorNumber (
    VOID
    )
{
    //
    // Get the group-wide CPU index
    //
    return (INT32)KeGetCurrentProcessorNumberEx(NULL);
}

INT32
ShvOsGetActiveProcessorCount (
    VOID
    )
{
    //
    // Get the group-wide CPU count
    //
    return (INT32)KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
}

VOID
ShvOsDebugPrint (
    _In_ PCCH Format,
    ...
    )
{
    va_list arglist;

    //
    // Call the debugger API
    //
    va_start(arglist, Format);
    vDbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, Format, arglist);
    va_end(arglist);
}

VOID
DriverUnload (
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    //
    // Unregister the power callback. We would not have loaded without it
    //
    ExUnregisterCallback(g_PowerCallbackRegistration);

    //
    // Unload the hypervisor before releasing the private H1-D page pool.
    //
    ShvUnload();
    ShvH1FreePages();
}

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    PCALLBACK_OBJECT callbackObject;
    UNICODE_STRING callbackName =
        RTL_CONSTANT_STRING(L"\\Callback\\PowerState");
    OBJECT_ATTRIBUTES objectAttributes =
        RTL_CONSTANT_OBJECT_ATTRIBUTES(&callbackName,
                                       OBJ_CASE_INSENSITIVE |
                                       OBJ_KERNEL_HANDLE);
    UNREFERENCED_PARAMETER(RegistryPath);

    //
    // H1-D owns eight private target pages plus eight private backing pages.
    // All allocations are page-sized; no application or ordinary Windows page
    // is used by this controlled repeated-reclamation milestone.
    //
    status = ShvH1PreparePages();
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    //
    // Make the driver (and SHV itself) unloadable
    //
    DriverObject->DriverUnload = DriverUnload;

    //
    // Create the power state callback
    //
    status = ExCreateCallback(&callbackObject, &objectAttributes, FALSE, TRUE);
    if (!NT_SUCCESS(status))
    {
        ShvH1FreePages();
        return status;
    }

    //
    // Now register our routine with this callback
    //
    g_PowerCallbackRegistration = ExRegisterCallback(callbackObject,
                                                     PowerCallback,
                                                     NULL);

    //
    // Dereference it in both cases -- either it's registered, so that is now
    // taking a reference, and we'll unregister later, or it failed to register
    // so we failing now, and it's gone.
    //
    ObDereferenceObject(callbackObject);

    //
    // Fail if we couldn't register the power callback
    //
    if (g_PowerCallbackRegistration == NULL)
    {
        ShvH1FreePages();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Load the hypervisor
    //
    status = ShvOsErrorToError(ShvLoad());

    //
    // If load of the hypervisor happened to fail, unregister previously registered
    // power callback, otherwise we would get BSOD on shutdown.
    //
    if (!NT_SUCCESS(status))
    {
        ExUnregisterCallback(g_PowerCallbackRegistration);
        ShvH1FreePages();
        return status;
    }

    //
    // H1-D runs hundreds of complete reset -> trap -> remap -> detached-frame
    // reuse -> write -> verify cycles on every logical processor, rotating
    // across eight independent target/backing page pairs.
    //
    if (ShvH1DRunCycles() == FALSE)
    {
        ShvUnload();
        ExUnregisterCallback(g_PowerCallbackRegistration);
        ShvH1FreePages();
        return STATUS_UNSUCCESSFUL;
    }

    ShvOsDebugPrint("H1-D PASS: cycles=%ld resets=%ld traps=%ld remaps=%ld write_traps=%ld detached_verified=%ld invept=%ld invept_fail=%ld last_GPA=0x%llX qualification=0x%llX\n",
                    ShvH1DCompletedCycles,
                    ShvH1DResetCount,
                    ShvH0EptTrapCount,
                    ShvH1RemapCount,
                    ShvH1WriteTrapCount,
                    ShvH1DetachedFrameVerifiedCount,
                    ShvH1DInveptCount,
                    ShvH1DInveptFailureCount,
                    ShvH0LastGuestPhysicalAddress,
                    ShvH0LastExitQualification);

    return STATUS_SUCCESS;
}

