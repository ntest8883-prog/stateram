param(
    [ValidateSet("query","reset")]
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

    if ($counterSize -ne 192) {
        throw "Protocol self-test failed: Counters size is $counterSize, expected 192."
    }

    Write-Host "StateRAMH3B query protocol self-test: PASS (Command=8, Counters=192)"
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
    $cmd.Version = 2
    $cmd.CommandId = if ($Command -eq "reset") { 2 } else { 1 }

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
        KnownPagefiles          = $reply.KnownPagefiles
        HistoryCapacity         = $reply.HistoryCapacity
        PagefileTableFull       = $reply.PagefileTableFull
    } | Format-List
}
finally {
    if ($port -ne [IntPtr]::Zero) {
        [void][StateRAMH3BNative]::CloseHandle($port)
    }
}
