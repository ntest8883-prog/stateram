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
PVOID g_H1TestPage;
PVOID g_H1BackingPage;

#define H1_POISON_BYTE 0xCC
#define H1_WRITE_OFFSET 1379
#define H1_WRITE_XOR    0x5A

VOID
ShvH1FreePages (
    VOID
    )
{
    if (g_H1BackingPage != NULL)
    {
        ShvOsFreeContiguousAlignedMemory(g_H1BackingPage);
        g_H1BackingPage = NULL;
    }

    if (g_H1TestPage != NULL)
    {
        ShvOsFreeContiguousAlignedMemory(g_H1TestPage);
        g_H1TestPage = NULL;
    }

    ShvH0TestPagePhysicalAddress = 0;
    ShvH1TestPageVirtualAddress = 0;
    ShvH1BackingPageVirtualAddress = 0;
    ShvH1BackingPagePhysicalAddress = 0;
}

NTSTATUS
ShvH1PreparePages (
    VOID
    )
{
    ULONG i;
    PUCHAR testBytes;
    PUCHAR backingBytes;

    g_H1TestPage = ShvOsAllocateContigousAlignedMemory(PAGE_SIZE);
    if (g_H1TestPage == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    g_H1BackingPage = ShvOsAllocateContigousAlignedMemory(PAGE_SIZE);
    if (g_H1BackingPage == NULL)
    {
        ShvH1FreePages();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    testBytes = (PUCHAR)g_H1TestPage;
    backingBytes = (PUCHAR)g_H1BackingPage;

    for (i = 0; i < PAGE_SIZE; i++)
    {
        testBytes[i] = (UCHAR)(((i * 131u) + 17u) & 0xFFu);
        backingBytes[i] = testBytes[i];
    }

    //
    // The original 4KB payload is intentionally destroyed here. The backing
    // page is now the only copy of the expected payload before VMX starts.
    //
    RtlFillMemory(g_H1TestPage, PAGE_SIZE, H1_POISON_BYTE);

    ShvH0EptTrapCount = 0;
    ShvH0LastGuestPhysicalAddress = 0;
    ShvH0LastExitQualification = 0;
    ShvH1RemapCount = 0;
    ShvH1WriteTrapCount = 0;
    ShvH1DetachedFrameVerifiedCount = 0;

    ShvH0TestPagePhysicalAddress = ShvOsGetPhysicalAddress(g_H1TestPage);
    ShvH1BackingPagePhysicalAddress = ShvOsGetPhysicalAddress(g_H1BackingPage);
    ShvH1TestPageVirtualAddress = (UINT64)(ULONG_PTR)g_H1TestPage;
    ShvH1BackingPageVirtualAddress = (UINT64)(ULONG_PTR)g_H1BackingPage;

    if ((ShvH0TestPagePhysicalAddress == 0) ||
        (ShvH1BackingPagePhysicalAddress == 0) ||
        (ShvH0TestPagePhysicalAddress == ShvH1BackingPagePhysicalAddress))
    {
        ShvH1FreePages();
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

BOOLEAN
ShvH1TriggerAndVerifyReclaim (
    VOID
    )
{
    SIZE_T matched;
    volatile UCHAR firstByte;
    UCHAR oldByte;
    UCHAR newByte;

    //
    // Phase 1: first access must be transparently redirected from the target
    // GPA to the alternate physical backing page.
    //
    firstByte = *(volatile UCHAR*)g_H1TestPage;

    if (firstByte != *(PUCHAR)g_H1BackingPage)
    {
        return FALSE;
    }

    if ((ShvH0EptTrapCount < 1) || (ShvH1RemapCount < 1))
    {
        return FALSE;
    }

    if ((ShvH0LastGuestPhysicalAddress & ~((UINT64)PAGE_SIZE - 1)) !=
        ShvH0TestPagePhysicalAddress)
    {
        return FALSE;
    }

    matched = RtlCompareMemory(g_H1TestPage, g_H1BackingPage, PAGE_SIZE);
    if (matched != PAGE_SIZE)
    {
        return FALSE;
    }

    //
    // Phase 2: deliberately write through the target GPA. The EPT leaf is
    // read-only after phase 1, so this causes a second controlled violation.
    // Root mode then overwrites and verifies the now-detached original HPA as
    // scratch space, proving that physical frame can be reused independently,
    // while the guest write is retried against the alternate backing HPA.
    //
    oldByte = ((PUCHAR)g_H1BackingPage)[H1_WRITE_OFFSET];
    newByte = oldByte ^ H1_WRITE_XOR;
    ((volatile PUCHAR)g_H1TestPage)[H1_WRITE_OFFSET] = newByte;

    if (((PUCHAR)g_H1TestPage)[H1_WRITE_OFFSET] != newByte ||
        ((PUCHAR)g_H1BackingPage)[H1_WRITE_OFFSET] != newByte)
    {
        return FALSE;
    }

    if ((ShvH0EptTrapCount < 2) ||
        (ShvH1WriteTrapCount < 1) ||
        (ShvH1DetachedFrameVerifiedCount < 1))
    {
        return FALSE;
    }

    matched = RtlCompareMemory(g_H1TestPage, g_H1BackingPage, PAGE_SIZE);
    if (matched != PAGE_SIZE)
    {
        return FALSE;
    }

    //
    // Restore the logical payload byte so unload sees the same logical data
    // pattern the test started with. The original HPA remains detached/scratch.
    //
    ((volatile PUCHAR)g_H1TestPage)[H1_WRITE_OFFSET] = oldByte;

    if (((PUCHAR)g_H1BackingPage)[H1_WRITE_OFFSET] != oldByte)
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
    // Unload the hypervisor before releasing the private H1-C pages.
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
    // H1-C owns exactly two private pages: a target GPA and an alternate
    // physical backing page. No application or ordinary Windows page is used.
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
    // H1-C performs both a transparent remap and a second write-fault phase
    // that reuses the detached original HPA as root-mode scratch space while
    // the guest continues using the alternate backing page.
    //
    if (ShvH1TriggerAndVerifyReclaim() == FALSE)
    {
        ShvUnload();
        ExUnregisterCallback(g_PowerCallbackRegistration);
        ShvH1FreePages();
        return STATUS_UNSUCCESSFUL;
    }

    ShvOsDebugPrint("H1-C PASS: traps=%ld remaps=%ld write_traps=%ld detached_verified=%ld GPA=0x%llX backing=0x%llX qualification=0x%llX\n",
                    ShvH0EptTrapCount,
                    ShvH1RemapCount,
                    ShvH1WriteTrapCount,
                    ShvH1DetachedFrameVerifiedCount,
                    ShvH0LastGuestPhysicalAddress,
                    ShvH1BackingPagePhysicalAddress,
                    ShvH0LastExitQualification);

    return STATUS_SUCCESS;
}

