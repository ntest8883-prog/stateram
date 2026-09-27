/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shvutil.c

Abstract:

    This module implements utility functions for the Simple Hyper Visor.

Author:

    Alex Ionescu (@aionescu) 16-Mar-2016 - Initial version

Environment:

    Kernel mode only.

--*/

#include "shv.h"

VOID
ShvUtilConvertGdtEntry (
    _In_ VOID* GdtBase,
    _In_ UINT16 Selector,
    _Out_ PVMX_GDTENTRY64 VmxGdtEntry
    )
{
    PKGDTENTRY64 gdtEntry;

    //
    // Reject LDT or NULL entries
    //
    if ((Selector == 0) ||
        (Selector & SELECTOR_TABLE_INDEX) != 0)
    {
        VmxGdtEntry->Limit = VmxGdtEntry->AccessRights = 0;
        VmxGdtEntry->Base = 0;
        VmxGdtEntry->Selector = 0;
        VmxGdtEntry->Bits.Unusable = TRUE;
        return;
    }

    //
    // Read the GDT entry at the given selector, masking out the RPL bits.
    //
    gdtEntry = (PKGDTENTRY64)((uintptr_t)GdtBase + (Selector & ~RPL_MASK));

    //
    // Write the selector directly 
    //
    VmxGdtEntry->Selector = Selector;

    //
    // Use the LSL intrinsic to read the segment limit
    //
    VmxGdtEntry->Limit = __segmentlimit(Selector);

    //
    // Build the full 64-bit effective address, keeping in mind that only when
    // the System bit is unset, should this be done.
    //
    // NOTE: The Windows definition of KGDTENTRY64 is WRONG. The "System" field
    // is incorrectly defined at the position of where the AVL bit should be.
    // The actual location of the SYSTEM bit is encoded as the highest bit in
    // the "Type" field.
    //
    VmxGdtEntry->Base = ((gdtEntry->Bytes.BaseHigh << 24) |
                         (gdtEntry->Bytes.BaseMiddle << 16) |
                         (gdtEntry->BaseLow)) & 0xFFFFFFFF;
    VmxGdtEntry->Base |= ((gdtEntry->Bits.Type & 0x10) == 0) ?
                         ((uintptr_t)gdtEntry->BaseUpper << 32) : 0;

    //
    // Load the access rights
    //
    VmxGdtEntry->AccessRights = 0;
    VmxGdtEntry->Bytes.Flags1 = gdtEntry->Bytes.Flags1;
    VmxGdtEntry->Bytes.Flags2 = gdtEntry->Bytes.Flags2;

    //
    // Finally, handle the VMX-specific bits
    //
    VmxGdtEntry->Bits.Reserved = 0;
    VmxGdtEntry->Bits.Unusable = !gdtEntry->Bits.Present;
}

UINT32
ShvUtilAdjustMsr (
    _In_ LARGE_INTEGER ControlValue,
    _In_ UINT32 DesiredValue
    )
{
    //
    // VMX feature/capability MSRs encode the "must be 0" bits in the high word
    // of their value, and the "must be 1" bits in the low word of their value.
    // Adjust any requested capability/feature based on these requirements.
    //
    DesiredValue &= ControlValue.HighPart;
    DesiredValue |= ControlValue.LowPart;
    return DesiredValue;
}

//
// H2-A uses a deliberately small, allocation-free PackBits-style codec.
// These helpers are safe to call from VMX root mode: they only perform
// bounded arithmetic and direct accesses to preallocated nonpaged memory.
//
static
UINT8
ShvH2StoreWriteByte (
    _In_ UINT32 Offset,
    _In_ UINT8 Value
    )
{
    UINT32 pageIndex;
    UINT32 pageOffset;
    UINT8* page;

    if (Offset >= (H2A_STORE_PAGE_COUNT * PAGE_SIZE))
    {
        return FALSE;
    }

    pageIndex = Offset / PAGE_SIZE;
    pageOffset = Offset & (PAGE_SIZE - 1);

    if (ShvH2StorePageVirtualAddresses[pageIndex] == 0)
    {
        return FALSE;
    }

    page = (UINT8*)(uintptr_t)
        ShvH2StorePageVirtualAddresses[pageIndex];
    page[pageOffset] = Value;
    return TRUE;
}

static
UINT8
ShvH2StoreReadByte (
    _In_ UINT32 Offset,
    _Out_ UINT8* Value
    )
{
    UINT32 pageIndex;
    UINT32 pageOffset;
    UINT8* page;

    if ((Value == NULL) ||
        (Offset >= (H2A_STORE_PAGE_COUNT * PAGE_SIZE)))
    {
        return FALSE;
    }

    pageIndex = Offset / PAGE_SIZE;
    pageOffset = Offset & (PAGE_SIZE - 1);

    if (ShvH2StorePageVirtualAddresses[pageIndex] == 0)
    {
        return FALSE;
    }

    page = (UINT8*)(uintptr_t)
        ShvH2StorePageVirtualAddresses[pageIndex];
    *Value = page[pageOffset];
    return TRUE;
}

UINT64
ShvH2HashBuffer (
    _In_reads_bytes_(Length) const UINT8* Buffer,
    _In_ UINT32 Length
    )
{
    UINT32 i;
    UINT64 hash;

    if (Buffer == NULL)
    {
        return 0;
    }

    hash = 1469598103934665603ULL;
    for (i = 0; i < Length; i++)
    {
        hash ^= Buffer[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

UINT8
ShvH2CompressPage (
    _In_ UINT32 PageIndex,
    _In_reads_bytes_(PAGE_SIZE) const UINT8* Source,
    _Out_ UINT32* CompressedLength
    )
{
    UINT32 input;
    UINT32 output;
    UINT32 run;
    UINT32 nextRun;
    UINT32 literalStart;
    UINT32 literalLength;
    UINT32 i;
    UINT32 slotBase;
    UINT8 header;

    if ((PageIndex >= H2A_PAGE_COUNT) ||
        (Source == NULL) ||
        (CompressedLength == NULL))
    {
        return FALSE;
    }

    slotBase = PageIndex * H2A_SLOT_SIZE;
    input = 0;
    output = 0;

    while (input < PAGE_SIZE)
    {
        run = 1;
        while (((input + run) < PAGE_SIZE) &&
               (run < 130) &&
               (Source[input + run] == Source[input]))
        {
            run++;
        }

        if (run >= 3)
        {
            if ((output + 2) > H2A_SLOT_SIZE)
            {
                return FALSE;
            }

            header = (UINT8)(0x80 | (run - 3));
            if ((ShvH2StoreWriteByte(slotBase + output, header) == FALSE) ||
                (ShvH2StoreWriteByte(slotBase + output + 1,
                                     Source[input]) == FALSE))
            {
                return FALSE;
            }

            output += 2;
            input += run;
        }
        else
        {
            literalStart = input;
            input += run;

            while ((input < PAGE_SIZE) &&
                   ((input - literalStart) < 128))
            {
                nextRun = 1;
                while (((input + nextRun) < PAGE_SIZE) &&
                       (nextRun < 130) &&
                       (Source[input + nextRun] == Source[input]))
                {
                    nextRun++;
                }

                if (nextRun >= 3)
                {
                    break;
                }

                if (((input - literalStart) + nextRun) > 128)
                {
                    input = literalStart + 128;
                    break;
                }

                input += nextRun;
            }

            literalLength = input - literalStart;
            if ((literalLength == 0) ||
                (literalLength > 128) ||
                ((output + 1 + literalLength) > H2A_SLOT_SIZE))
            {
                return FALSE;
            }

            header = (UINT8)(literalLength - 1);
            if (ShvH2StoreWriteByte(slotBase + output, header) == FALSE)
            {
                return FALSE;
            }

            output++;
            for (i = 0; i < literalLength; i++)
            {
                if (ShvH2StoreWriteByte(slotBase + output + i,
                                        Source[literalStart + i]) == FALSE)
                {
                    return FALSE;
                }
            }

            output += literalLength;
        }
    }

    *CompressedLength = output;
    return TRUE;
}

UINT8
ShvH2DecompressPage (
    _In_ UINT32 PageIndex,
    _Out_writes_bytes_(PAGE_SIZE) UINT8* Destination
    )
{
    UINT32 input;
    UINT32 output;
    UINT32 compressedLength;
    UINT32 count;
    UINT32 i;
    UINT32 slotBase;
    UINT8 header;
    UINT8 value;

    if ((PageIndex >= H2A_PAGE_COUNT) ||
        (Destination == NULL))
    {
        return FALSE;
    }

    compressedLength = ShvH2CompressedLength[PageIndex];
    if ((compressedLength == 0) ||
        (compressedLength > H2A_SLOT_SIZE))
    {
        return FALSE;
    }

    slotBase = PageIndex * H2A_SLOT_SIZE;
    input = 0;
    output = 0;

    while ((input < compressedLength) &&
           (output < PAGE_SIZE))
    {
        if (ShvH2StoreReadByte(slotBase + input, &header) == FALSE)
        {
            return FALSE;
        }
        input++;

        if ((header & 0x80) != 0)
        {
            count = (header & 0x7F) + 3;
            if ((input >= compressedLength) ||
                ((output + count) > PAGE_SIZE))
            {
                return FALSE;
            }

            if (ShvH2StoreReadByte(slotBase + input, &value) == FALSE)
            {
                return FALSE;
            }
            input++;

            for (i = 0; i < count; i++)
            {
                Destination[output + i] = value;
            }
            output += count;
        }
        else
        {
            count = (header & 0x7F) + 1;
            if (((input + count) > compressedLength) ||
                ((output + count) > PAGE_SIZE))
            {
                return FALSE;
            }

            for (i = 0; i < count; i++)
            {
                if (ShvH2StoreReadByte(slotBase + input + i,
                                       &Destination[output + i]) == FALSE)
                {
                    return FALSE;
                }
            }

            input += count;
            output += count;
        }
    }

    return (UINT8)((input == compressedLength) &&
                   (output == PAGE_SIZE));
}

