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
PVOID g_H1TargetPages[H1D_PAGE_COUNT];
PVOID g_H1BackingPages[H1D_PAGE_COUNT];

#define H1D_WRITE_XOR 0x5A
#define H1R_POOL_TAG   'R1HS'
#define H1R_ATTEMPTS   8
#define H1R_WRITE_XOR  0x6D

VOID
ShvH1FreePages (
    VOID
    )
{
    ULONG i;

    for (i = 0; i < H1D_PAGE_COUNT; i++)
    {
        if (g_H1BackingPages[i] != NULL)
        {
            ShvOsFreeContiguousAlignedMemory(g_H1BackingPages[i]);
            g_H1BackingPages[i] = NULL;
        }

        if (g_H1TargetPages[i] != NULL)
        {
            ShvOsFreeContiguousAlignedMemory(g_H1TargetPages[i]);
            g_H1TargetPages[i] = NULL;
        }

        ShvH1TargetPageVirtualAddresses[i] = 0;
        ShvH1BackingPageVirtualAddresses[i] = 0;
        ShvH1TargetPagePhysicalAddresses[i] = 0;
        ShvH1BackingPagePhysicalAddresses[i] = 0;
    }
}

NTSTATUS
ShvH1PreparePages (
    VOID
    )
{
    ULONG i;
    ULONG j;
    PUCHAR targetBytes;
    PUCHAR backingBytes;

    for (i = 0; i < H1D_PAGE_COUNT; i++)
    {
        g_H1TargetPages[i] =
            ShvOsAllocateContigousAlignedMemory(PAGE_SIZE);
        if (g_H1TargetPages[i] == NULL)
        {
            ShvH1FreePages();
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        g_H1BackingPages[i] =
            ShvOsAllocateContigousAlignedMemory(PAGE_SIZE);
        if (g_H1BackingPages[i] == NULL)
        {
            ShvH1FreePages();
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        targetBytes = (PUCHAR)g_H1TargetPages[i];
        backingBytes = (PUCHAR)g_H1BackingPages[i];

        for (j = 0; j < PAGE_SIZE; j++)
        {
            targetBytes[j] =
                (UCHAR)(((j * 131u) + (i * 29u) + 0x5Du) & 0xFFu);
            backingBytes[j] = targetBytes[j];
        }

        ShvH1TargetPageVirtualAddresses[i] =
            (UINT64)(ULONG_PTR)g_H1TargetPages[i];
        ShvH1BackingPageVirtualAddresses[i] =
            (UINT64)(ULONG_PTR)g_H1BackingPages[i];
        ShvH1TargetPagePhysicalAddresses[i] =
            ShvOsGetPhysicalAddress(g_H1TargetPages[i]);
        ShvH1BackingPagePhysicalAddresses[i] =
            ShvOsGetPhysicalAddress(g_H1BackingPages[i]);

        if ((ShvH1TargetPagePhysicalAddresses[i] == 0) ||
            (ShvH1BackingPagePhysicalAddresses[i] == 0) ||
            (ShvH1TargetPagePhysicalAddresses[i] ==
             ShvH1BackingPagePhysicalAddresses[i]))
        {
            ShvH1FreePages();
            return STATUS_UNSUCCESSFUL;
        }
    }

    ShvH0EptTrapCount = 0;
    ShvH0LastGuestPhysicalAddress = 0;
    ShvH0LastExitQualification = 0;
    ShvH1RemapCount = 0;
    ShvH1WriteTrapCount = 0;
    ShvH1DetachedFrameVerifiedCount = 0;
    ShvH1DResetCount = 0;
    ShvH1DInveptCount = 0;
    ShvH1DInveptFailureCount = 0;
    ShvH1DCompletedCycles = 0;

    return STATUS_SUCCESS;
}

BOOLEAN
ShvH1DRunCycles (
    VOID
    )
{
    BOOLEAN result;
    INT32 cpuInfo[4];
    INT32 cpuCount;
    INT32 cpu;
    ULONG cycle;
    ULONG sequence;
    ULONG targetIndex;
    ULONG offset;
    ULONG offset2;
    SIZE_T matched;
    KAFFINITY previousAffinity;
    UCHAR oldByte;
    UCHAR newByte;
    UCHAR oldByte2;
    UCHAR newByte2;
    long trapBefore;
    long remapBefore;
    long writeBefore;
    long detachedBefore;
    long failureBefore;

    result = TRUE;
    cpuCount = (INT32)KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    if ((cpuCount <= 0) ||
        (cpuCount > (INT32)(sizeof(KAFFINITY) * 8)))
    {
        return FALSE;
    }

    //
    // Exercise every logical processor separately. Each VP owns a distinct
    // EPT hierarchy and EPTP, so every CPU must pass the live-remap path.
    //
    for (cpu = 0; cpu < cpuCount; cpu++)
    {
        previousAffinity =
            KeSetSystemAffinityThreadEx(((KAFFINITY)1) << cpu);

        for (cycle = 0; cycle < H1D_CYCLES_PER_CPU; cycle++)
        {
            sequence =
                ((ULONG)cpu * H1D_CYCLES_PER_CPU) + cycle;

            trapBefore = ShvH0EptTrapCount;
            remapBefore = ShvH1RemapCount;
            writeBefore = ShvH1WriteTrapCount;
            detachedBefore = ShvH1DetachedFrameVerifiedCount;
            failureBefore = ShvH1DInveptFailureCount;

            __cpuidex(cpuInfo,
                      H1D_CPUID_RESET_LEAF,
                      (INT32)sequence);

            if ((UINT32)cpuInfo[0] != H1D_CPUID_RESET_OK)
            {
                result = FALSE;
                break;
            }

            targetIndex = (ULONG)(UINT32)cpuInfo[1];
            if (targetIndex >= H1D_PAGE_COUNT)
            {
                result = FALSE;
                break;
            }

            //
            // Full-page compare generates the first EPT violation, remaps the
            // same GPA to backing HPA B, and then continues transparently.
            //
            matched =
                RtlCompareMemory(g_H1TargetPages[targetIndex],
                                 g_H1BackingPages[targetIndex],
                                 PAGE_SIZE);
            if (matched != PAGE_SIZE)
            {
                result = FALSE;
                break;
            }

            //
            // First write: second EPT violation. Root mode independently
            // overwrites/verifies detached HPA A, then retries this write on B.
            //
            offset =
                ((sequence * 977u) + 1379u) & (PAGE_SIZE - 1);
            oldByte =
                ((PUCHAR)g_H1BackingPages[targetIndex])[offset];
            newByte = oldByte ^ H1D_WRITE_XOR;

            *(volatile UCHAR*)
                ((PUCHAR)g_H1TargetPages[targetIndex] + offset) =
                    newByte;

            if ((((PUCHAR)g_H1TargetPages[targetIndex])[offset] !=
                 newByte) ||
                (((PUCHAR)g_H1BackingPages[targetIndex])[offset] !=
                 newByte))
            {
                result = FALSE;
                break;
            }

            //
            // Second write should require no new semantic phase transition and
            // must remain coherent through the already-remapped writable leaf.
            //
            offset2 =
                ((sequence * 313u) + 257u) & (PAGE_SIZE - 1);
            if (offset2 == offset)
            {
                offset2 = (offset2 + 1) & (PAGE_SIZE - 1);
            }

            oldByte2 =
                ((PUCHAR)g_H1BackingPages[targetIndex])[offset2];
            newByte2 = oldByte2 ^ (H1D_WRITE_XOR + 1);

            *(volatile UCHAR*)
                ((PUCHAR)g_H1TargetPages[targetIndex] + offset2) =
                    newByte2;

            if ((((PUCHAR)g_H1TargetPages[targetIndex])[offset2] !=
                 newByte2) ||
                (((PUCHAR)g_H1BackingPages[targetIndex])[offset2] !=
                 newByte2))
            {
                result = FALSE;
                break;
            }

            matched =
                RtlCompareMemory(g_H1TargetPages[targetIndex],
                                 g_H1BackingPages[targetIndex],
                                 PAGE_SIZE);
            if (matched != PAGE_SIZE)
            {
                result = FALSE;
                break;
            }

            if ((ShvH1RemapCount != (remapBefore + 1)) ||
                (ShvH1WriteTrapCount != (writeBefore + 1)) ||
                (ShvH1DetachedFrameVerifiedCount !=
                 (detachedBefore + 1)) ||
                (ShvH0EptTrapCount < (trapBefore + 2)) ||
                (ShvH1DInveptFailureCount != failureBefore))
            {
                result = FALSE;
                break;
            }

            _InterlockedIncrement(&ShvH1DCompletedCycles);
        }

        KeRevertToUserAffinityThreadEx(previousAffinity);

        if (result == FALSE)
        {
            break;
        }
    }

    if (result != FALSE)
    {
        long expectedCycles;

        expectedCycles =
            ((long)cpuCount) * H1D_CYCLES_PER_CPU;

        if ((ShvH1DCompletedCycles != expectedCycles) ||
            (ShvH1DResetCount != expectedCycles) ||
            (ShvH1RemapCount != expectedCycles) ||
            (ShvH1WriteTrapCount != expectedCycles) ||
            (ShvH1DetachedFrameVerifiedCount != expectedCycles) ||
            (ShvH0EptTrapCount < (expectedCycles * 2)) ||
            (ShvH1DInveptCount < (expectedCycles * 3)) ||
            (ShvH1DInveptFailureCount != 0))
        {
            result = FALSE;
        }
    }

    return result;
}


VOID
ShvH1RFreeMdl (
    _Inout_ PMDL* Mdl
    )
{
    if ((Mdl != NULL) && (*Mdl != NULL))
    {
        MmFreePagesFromMdl(*Mdl);
        ExFreePool(*Mdl);
        *Mdl = NULL;
    }
}

BOOLEAN
ShvH1RRunRealReclaim (
    VOID
    )
{
    ULONG attempt;
    ULONG i;
    BOOLEAN success;
    PVOID mappingAddress;
    PVOID mappedAddress;
    PUCHAR backup;
    PMDL mdlA;
    PMDL mdlB;
    PMDL mdlReuse;
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS skip;
    PHYSICAL_ADDRESS mappedPhysical;
    PFN_NUMBER pfnA;
    PFN_NUMBER pfnB;
    PFN_NUMBER pfnReuse;
    UCHAR oldByte;
    UCHAR newByte;
    SIZE_T matched;

    success = FALSE;

    for (attempt = 0; attempt < H1R_ATTEMPTS; attempt++)
    {
        mappingAddress = NULL;
        mappedAddress = NULL;
        backup = NULL;
        mdlA = NULL;
        mdlB = NULL;
        mdlReuse = NULL;

        mappingAddress =
            MmAllocateMappingAddress(PAGE_SIZE, H1R_POOL_TAG);
        if (mappingAddress == NULL)
        {
            continue;
        }

        backup =
            (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx,
                                         PAGE_SIZE,
                                         H1R_POOL_TAG);
        if (backup == NULL)
        {
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        low.QuadPart = 0;
        high.QuadPart = -1;
        skip.QuadPart = 0;

        mdlA =
            MmAllocatePagesForMdlEx(low,
                                    high,
                                    skip,
                                    PAGE_SIZE,
                                    MmCached,
                                    0);
        if ((mdlA == NULL) ||
            (MmGetMdlByteCount(mdlA) < PAGE_SIZE))
        {
            ShvH1RFreeMdl(&mdlA);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        pfnA = MmGetMdlPfnArray(mdlA)[0];

        mappedAddress =
            MmMapLockedPagesWithReservedMapping(mappingAddress,
                                                H1R_POOL_TAG,
                                                mdlA,
                                                MmCached);
        if (mappedAddress != mappingAddress)
        {
            if (mappedAddress != NULL)
            {
                MmUnmapReservedMapping(mappedAddress,
                                       H1R_POOL_TAG,
                                       mdlA);
            }
            ShvH1RFreeMdl(&mdlA);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        for (i = 0; i < PAGE_SIZE; i++)
        {
            ((PUCHAR)mappedAddress)[i] =
                (UCHAR)(((i * 131u) +
                         (attempt * 17u) +
                         0xA7u) & 0xFFu);
            backup[i] = ((PUCHAR)mappedAddress)[i];
        }

        MmUnmapReservedMapping(mappedAddress,
                               H1R_POOL_TAG,
                               mdlA);
        mappedAddress = NULL;

        //
        // Allocate replacement HPA B while A is still owned by mdlA. This
        // guarantees the replacement frame is physically different from A.
        //
        mdlB =
            MmAllocatePagesForMdlEx(low,
                                    high,
                                    skip,
                                    PAGE_SIZE,
                                    MmCached,
                                    0);
        if ((mdlB == NULL) ||
            (MmGetMdlByteCount(mdlB) < PAGE_SIZE))
        {
            ShvH1RFreeMdl(&mdlB);
            ShvH1RFreeMdl(&mdlA);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        pfnB = MmGetMdlPfnArray(mdlB)[0];
        if (pfnB == pfnA)
        {
            ShvH1RFreeMdl(&mdlB);
            ShvH1RFreeMdl(&mdlA);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        //
        // Critical H1-C completion point: return physical frame A to the
        // Windows memory manager. There is no remaining VA mapping to A.
        //
        ShvH1RFreeMdl(&mdlA);

        //
        // Ask Windows for exactly that physical frame again. Success proves
        // the frame was genuinely returned to the allocator and can be handed
        // out as a new allocation, rather than merely hidden by EPT.
        //
        low.QuadPart = ((LONGLONG)pfnA) << PAGE_SHIFT;
        high.QuadPart = low.QuadPart + PAGE_SIZE - 1;
        skip.QuadPart = 0;

        mdlReuse =
            MmAllocatePagesForMdlEx(low,
                                    high,
                                    skip,
                                    PAGE_SIZE,
                                    MmCached,
                                    0);
        if ((mdlReuse == NULL) ||
            (MmGetMdlByteCount(mdlReuse) < PAGE_SIZE))
        {
            ShvH1RFreeMdl(&mdlReuse);
            ShvH1RFreeMdl(&mdlB);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        pfnReuse = MmGetMdlPfnArray(mdlReuse)[0];
        if (pfnReuse != pfnA)
        {
            ShvH1RFreeMdl(&mdlReuse);
            ShvH1RFreeMdl(&mdlB);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        mappedAddress =
            MmMapLockedPagesWithReservedMapping(mappingAddress,
                                                H1R_POOL_TAG,
                                                mdlReuse,
                                                MmCached);
        if (mappedAddress != mappingAddress)
        {
            if (mappedAddress != NULL)
            {
                MmUnmapReservedMapping(mappedAddress,
                                       H1R_POOL_TAG,
                                       mdlReuse);
            }
            ShvH1RFreeMdl(&mdlReuse);
            ShvH1RFreeMdl(&mdlB);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        RtlFillMemory(mappedAddress, PAGE_SIZE, 0xD3);
        for (i = 0; i < PAGE_SIZE; i++)
        {
            if (((PUCHAR)mappedAddress)[i] != 0xD3)
            {
                break;
            }
        }

        if (i != PAGE_SIZE)
        {
            MmUnmapReservedMapping(mappedAddress,
                                   H1R_POOL_TAG,
                                   mdlReuse);
            mappedAddress = NULL;
            ShvH1RFreeMdl(&mdlReuse);
            ShvH1RFreeMdl(&mdlB);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        MmUnmapReservedMapping(mappedAddress,
                               H1R_POOL_TAG,
                               mdlReuse);
        mappedAddress = NULL;
        ShvH1RFreeMdl(&mdlReuse);

        //
        // Restore the logical page at the exact same reserved virtual address,
        // but now backed by the distinct physical frame B.
        //
        mappedAddress =
            MmMapLockedPagesWithReservedMapping(mappingAddress,
                                                H1R_POOL_TAG,
                                                mdlB,
                                                MmCached);
        if (mappedAddress != mappingAddress)
        {
            if (mappedAddress != NULL)
            {
                MmUnmapReservedMapping(mappedAddress,
                                       H1R_POOL_TAG,
                                       mdlB);
            }
            ShvH1RFreeMdl(&mdlB);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        RtlCopyMemory(mappedAddress, backup, PAGE_SIZE);

        matched =
            RtlCompareMemory(mappedAddress, backup, PAGE_SIZE);
        if (matched != PAGE_SIZE)
        {
            MmUnmapReservedMapping(mappedAddress,
                                   H1R_POOL_TAG,
                                   mdlB);
            mappedAddress = NULL;
            ShvH1RFreeMdl(&mdlB);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        mappedPhysical = MmGetPhysicalAddress(mappedAddress);
        if ((PFN_NUMBER)(mappedPhysical.QuadPart >> PAGE_SHIFT) != pfnB)
        {
            MmUnmapReservedMapping(mappedAddress,
                                   H1R_POOL_TAG,
                                   mdlB);
            mappedAddress = NULL;
            ShvH1RFreeMdl(&mdlB);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        //
        // Verify that ordinary writes after restoration affect the replacement
        // frame and remain readable through the stable logical VA.
        //
        i = ((attempt * 977u) + 1379u) & (PAGE_SIZE - 1);
        oldByte = backup[i];
        newByte = oldByte ^ H1R_WRITE_XOR;
        *(volatile UCHAR*)((PUCHAR)mappedAddress + i) = newByte;

        if (((PUCHAR)mappedAddress)[i] != newByte)
        {
            MmUnmapReservedMapping(mappedAddress,
                                   H1R_POOL_TAG,
                                   mdlB);
            mappedAddress = NULL;
            ShvH1RFreeMdl(&mdlB);
            ExFreePool(backup);
            MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);
            continue;
        }

        ShvOsDebugPrint("H1 REAL RECLAIM PASS: attempt=%lu VA=%p released_reacquired_PFN=0x%llX replacement_PFN=0x%llX\n",
                        attempt + 1,
                        mappingAddress,
                        (UINT64)pfnA,
                        (UINT64)pfnB);

        MmUnmapReservedMapping(mappedAddress,
                               H1R_POOL_TAG,
                               mdlB);
        mappedAddress = NULL;
        ShvH1RFreeMdl(&mdlB);
        ExFreePool(backup);
        MmFreeMappingAddress(mappingAddress, H1R_POOL_TAG);

        success = TRUE;
        break;
    }

    return success;
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

    //
    // H1-C completion: use documented Windows MDL + reserved-mapping APIs to
    // unmap a logical page, genuinely return its physical frame to the Windows
    // allocator, reacquire that exact PFN as a new allocation, then restore the
    // logical contents at the same VA on a distinct replacement PFN.
    //
    if (ShvH1RRunRealReclaim() == FALSE)
    {
        ShvOsDebugPrint("H1 REAL RECLAIM FAILED after %u attempts\n",
                        H1R_ATTEMPTS);
        ShvUnload();
        ExUnregisterCallback(g_PowerCallbackRegistration);
        ShvH1FreePages();
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

