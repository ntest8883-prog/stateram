param(
    [ValidateSet("query","reset","arm","disarm")]
    [string]$Command = "query",
    [switch]$SelfTest
)

$signature = @'
using System;
using System.Runtime.InteropServices;

public static class StateRAMH3BNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Command
    {
        public UInt32 Version;
        public UInt32 CommandId;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Counters
    {
        public UInt32 Version;
        public UInt32 Size;

        public Int64 PagefileReads;
        public Int64 PagefileReadBytes;
        public Int64 PagefileWrites;
        public Int64 PagefileWriteBytes;
        public Int64 PagingFileCreates;

        public Int64 ShadowWritePages;
        public Int64 ShadowReadPages;
        public Int64 ShadowMatches;
        public Int64 ShadowMismatches;
        public Int64 ShadowUntracked;
        public Int64 ShadowReplacements;
        public Int64 ShadowBufferUnavailable;
        public Int64 ShadowUnaligned;
        public Int64 ShadowHighIrqlSkips;
        public Int64 ShadowTableEntries;
        public Int64 ShadowTableCapacity;
        public Int64 ShadowPublishSkipped;
        public Int64 ShadowVerifyInvalidated;
        public Int64 HistoryExpired;
        public Int64 HistoryRecordDrops;
        public Int64 KnownPagefiles;
        public Int64 HistoryCapacity;
        public Int64 PagefileTableFull;
        public Int64 PagefileIdentities;
        public Int64 PagefileAliases;
        public Int64 ReadNewSystemBuffers;
        public Int64 CrossObjectComparisons;
        public Int64 CrossObjectMatches;
        public Int64 CrossObjectMismatches;
        public Int64 NewSystemBufferComparisons;
        public Int64 NewSystemBufferMismatches;
        public Int64 DynamicPagefileDiscoveries;
        public Int64 ConcurrentOverlapSkips;
        public Int64 DroppedInflightRecords;
        public Int64 DroppedInflightOutstanding;

        public Int64 PayloadWritePages;
        public Int64 PayloadReadPages;
        public Int64 PayloadMatches;
        public Int64 PayloadMismatches;
        public Int64 PayloadReplacements;
        public Int64 PayloadLookupMisses;
        public Int64 PayloadCaptureSkipped;
        public Int64 PayloadBufferUnavailable;
        public Int64 PayloadCapacity;

        public Int64 InterventionArmed;
        public Int64 InterventionEligible;
        public Int64 InterventionAttempts;
        public Int64 InterventionServedPages;
        public Int64 InterventionFallbacks;
        public Int64 InterventionGuardRejects;
        public Int64 InterventionPayloadMisses;
        public Int64 InterventionHashRejects;
        public Int64 InterventionCapacity;

        public Int64 DiagCaptured;
        public Int64 DiagIdentityIndex;
        public Int64 DiagReadBaseOffset;
        public Int64 DiagPageOffset;
        public Int64 DiagReadRequestedBytes;
        public Int64 DiagReadCompletedBytes;
        public Int64 DiagReadMdlBytes;
        public Int64 DiagReadPageOrdinal;
        public Int64 DiagReadIrpFlags;
        public Int64 DiagReadOperationFlags;
        public Int64 DiagReadDataFlags;
        public Int64 DiagWriteSequence;
        public Int64 DiagWriteIoLength;
        public Int64 DiagWriteMdlBytes;
        public Int64 DiagWritePageOrdinal;
        public Int64 DiagWriteIrpFlags;
        public Int64 DiagWriteOperationFlags;
        public Int64 DiagWriteDataFlags;
        public Int64 DiagExpectedHash1;
        public Int64 DiagExpectedHash2;
        public Int64 DiagActualHash1;
        public Int64 DiagActualHash2;
        public Int64 DiagWriterSameObject;
        public Int64 DiagGeneration;

        public Int64 TaggedWritePages;
        public Int64 TaggedReadPages;
        public Int64 TaggedFirstWriteOffset;
        public Int64 TaggedFirstReadOffset;
        public Int64 TaggedFirstWritePageIndex;
        public Int64 TaggedFirstReadPageIndex;
        public Int64 TaggedFirstWriteSequence;
        public Int64 TaggedFirstReadIdentityIndex;
    }

    [DllImport("fltlib.dll", CharSet = CharSet.Unicode)]
    public static extern int FilterConnectCommunicationPort(
        string lpPortName,
        UInt32 dwOptions,
        IntPtr lpContext,
        UInt16 wSizeOfContext,
        IntPtr lpSecurityAttributes,
        out IntPtr hPort);

    [DllImport("fltlib.dll")]
    public static extern int FilterSendMessage(
        IntPtr hPort,
        ref Command lpInBuffer,
        UInt32 dwInBufferSize,
        out Counters lpOutBuffer,
        UInt32 dwOutBufferSize,
        out UInt32 lpBytesReturned);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr hObject);
}
'@

if (-not ("StateRAMH3BNative" -as [type])) {
    Add-Type -TypeDefinition $signature
}

if ($SelfTest) {
    $commandSize = [Runtime.InteropServices.Marshal]::SizeOf([type]"StateRAMH3BNative+Command")
    $counterSize = [Runtime.InteropServices.Marshal]::SizeOf([type]"StateRAMH3BNative+Counters")

    if ($commandSize -ne 8) {
        throw "Protocol self-test failed: Command size is $commandSize, expected 8."
    }

    if ($counterSize -ne 688) {
        throw "Protocol self-test failed: Counters size is $counterSize, expected 688."
    }

    Write-Host "StateRAMH3B query protocol self-test: PASS (Command=8, Counters=688)"
    exit 0
}

$port = [IntPtr]::Zero
$hr = [StateRAMH3BNative]::FilterConnectCommunicationPort(
    "\StateRAMH3BPort",
    0,
    [IntPtr]::Zero,
    0,
    [IntPtr]::Zero,
    [ref]$port)

if ($hr -ne 0) {
    throw ("FilterConnectCommunicationPort failed: 0x{0:X8}" -f ([uint32]$hr))
}

try {
    $cmd = New-Object StateRAMH3BNative+Command
    $cmd.Version = 7
    $cmd.CommandId = switch ($Command) {
        "query"  { 1 }
        "reset"  { 2 }
        "arm"    { 3 }
        "disarm" { 4 }
    }

    $reply = New-Object StateRAMH3BNative+Counters
    [uint32]$returned = 0

    $hr = [StateRAMH3BNative]::FilterSendMessage(
        $port,
        [ref]$cmd,
        [Runtime.InteropServices.Marshal]::SizeOf($cmd),
        [ref]$reply,
        [Runtime.InteropServices.Marshal]::SizeOf($reply),
        [ref]$returned)

    if ($hr -ne 0) {
        throw ("FilterSendMessage failed: 0x{0:X8}" -f ([uint32]$hr))
    }

    [pscustomobject]@{
        Version                 = $reply.Version
        PagefileReads           = $reply.PagefileReads
        PagefileReadMiB         = [math]::Round($reply.PagefileReadBytes / 1MB, 3)
        PagefileWrites          = $reply.PagefileWrites
        PagefileWriteMiB        = [math]::Round($reply.PagefileWriteBytes / 1MB, 3)
        PagingFileCreates       = $reply.PagingFileCreates

        ShadowWritePages        = $reply.ShadowWritePages
        ShadowReadPages         = $reply.ShadowReadPages
        ShadowMatches           = $reply.ShadowMatches
        ShadowMismatches        = $reply.ShadowMismatches
        ShadowUntracked         = $reply.ShadowUntracked
        ShadowReplacements      = $reply.ShadowReplacements
        ShadowBufferUnavailable = $reply.ShadowBufferUnavailable
        ShadowUnaligned         = $reply.ShadowUnaligned
        ShadowHighIrqlSkips     = $reply.ShadowHighIrqlSkips
        ShadowTableEntries      = $reply.ShadowTableEntries
        ShadowTableCapacity     = $reply.ShadowTableCapacity
        ShadowPublishSkipped    = $reply.ShadowPublishSkipped
        ShadowVerifyInvalidated = $reply.ShadowVerifyInvalidated
        HistoryExpired          = $reply.HistoryExpired
        HistoryRecordDrops      = $reply.HistoryRecordDrops
        KnownPagefileObjects    = $reply.KnownPagefiles
        HistoryCapacity         = $reply.HistoryCapacity
        PagefileTableFull       = $reply.PagefileTableFull
        PagefileIdentities      = $reply.PagefileIdentities
        PagefileAliases         = $reply.PagefileAliases
        ReadNewSystemBuffers    = $reply.ReadNewSystemBuffers
        CrossObjectComparisons  = $reply.CrossObjectComparisons
        CrossObjectMatches      = $reply.CrossObjectMatches
        CrossObjectMismatches   = $reply.CrossObjectMismatches
        NewSystemBufComparisons = $reply.NewSystemBufferComparisons
        NewSystemBufMismatches  = $reply.NewSystemBufferMismatches
        DynamicPagefileDiscovery = $reply.DynamicPagefileDiscoveries
        ConcurrentOverlapSkips   = $reply.ConcurrentOverlapSkips
        DroppedInflightRecords   = $reply.DroppedInflightRecords
        DroppedInflightOutstanding = $reply.DroppedInflightOutstanding

        PayloadWritePages         = $reply.PayloadWritePages
        PayloadReadPages          = $reply.PayloadReadPages
        PayloadMatches            = $reply.PayloadMatches
        PayloadMismatches         = $reply.PayloadMismatches
        PayloadReplacements       = $reply.PayloadReplacements
        PayloadLookupMisses       = $reply.PayloadLookupMisses
        PayloadCaptureSkipped     = $reply.PayloadCaptureSkipped
        PayloadBufferUnavailable  = $reply.PayloadBufferUnavailable
        PayloadCapacity           = $reply.PayloadCapacity

        InterventionArmed          = $reply.InterventionArmed
        InterventionEligible       = $reply.InterventionEligible
        InterventionAttempts       = $reply.InterventionAttempts
        InterventionServedPages    = $reply.InterventionServedPages
        InterventionFallbacks      = $reply.InterventionFallbacks
        InterventionGuardRejects   = $reply.InterventionGuardRejects
        InterventionPayloadMisses  = $reply.InterventionPayloadMisses
        InterventionHashRejects    = $reply.InterventionHashRejects
        InterventionCapacity       = $reply.InterventionCapacity

        DiagCaptured               = $reply.DiagCaptured
        DiagIdentityIndex          = $reply.DiagIdentityIndex
        DiagReadBaseOffset         = $reply.DiagReadBaseOffset
        DiagPageOffset             = $reply.DiagPageOffset
        DiagReadRequestedBytes     = $reply.DiagReadRequestedBytes
        DiagReadCompletedBytes     = $reply.DiagReadCompletedBytes
        DiagReadMdlBytes           = $reply.DiagReadMdlBytes
        DiagReadPageOrdinal        = $reply.DiagReadPageOrdinal
        DiagReadIrpFlagsHex        = ("0x{0:X}" -f [uint64]$reply.DiagReadIrpFlags)
        DiagReadOperationFlagsHex  = ("0x{0:X}" -f [uint64]$reply.DiagReadOperationFlags)
        DiagReadDataFlagsHex       = ("0x{0:X}" -f [uint64]$reply.DiagReadDataFlags)
        DiagWriteSequence          = $reply.DiagWriteSequence
        DiagWriteIoLength          = $reply.DiagWriteIoLength
        DiagWriteMdlBytes          = $reply.DiagWriteMdlBytes
        DiagWritePageOrdinal       = $reply.DiagWritePageOrdinal
        DiagWriteIrpFlagsHex       = ("0x{0:X}" -f [uint64]$reply.DiagWriteIrpFlags)
        DiagWriteOperationFlagsHex = ("0x{0:X}" -f [uint64]$reply.DiagWriteOperationFlags)
        DiagWriteDataFlagsHex      = ("0x{0:X}" -f [uint64]$reply.DiagWriteDataFlags)
        DiagExpectedHash1Hex       = ("0x{0:X16}" -f [uint64]$reply.DiagExpectedHash1)
        DiagExpectedHash2Hex       = ("0x{0:X16}" -f [uint64]$reply.DiagExpectedHash2)
        DiagActualHash1Hex         = ("0x{0:X16}" -f [uint64]$reply.DiagActualHash1)
        DiagActualHash2Hex         = ("0x{0:X16}" -f [uint64]$reply.DiagActualHash2)
        DiagWriterSameObject       = $reply.DiagWriterSameObject
        DiagGeneration             = $reply.DiagGeneration

        TaggedWritePages            = $reply.TaggedWritePages
        TaggedReadPages             = $reply.TaggedReadPages
        TaggedFirstWriteOffset      = $reply.TaggedFirstWriteOffset
        TaggedFirstReadOffset       = $reply.TaggedFirstReadOffset
        TaggedFirstWritePageIndex   = $reply.TaggedFirstWritePageIndex
        TaggedFirstReadPageIndex    = $reply.TaggedFirstReadPageIndex
        TaggedFirstWriteSequence    = $reply.TaggedFirstWriteSequence
        TaggedFirstReadIdentityIndex = $reply.TaggedFirstReadIdentityIndex
    } | Format-List
}
finally {
    if ($port -ne [IntPtr]::Zero) {
        [void][StateRAMH3BNative]::CloseHandle($port)
    }
}
