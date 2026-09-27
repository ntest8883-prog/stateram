param(
    [ValidateSet("query","reset")]
    [string]$Command = "query"
)

$signature = @'
using System;
using System.Runtime.InteropServices;

public static class StateRAMH3Native
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
        public Int64 TotalReads;
        public Int64 TotalReadBytes;
        public Int64 TotalWrites;
        public Int64 TotalWriteBytes;
        public Int64 PagingReads;
        public Int64 PagingReadBytes;
        public Int64 PagingWrites;
        public Int64 PagingWriteBytes;
        public Int64 PagefileReads;
        public Int64 PagefileReadBytes;
        public Int64 PagefileWrites;
        public Int64 PagefileWriteBytes;
        public Int64 PagingFileCreates;
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

if (-not ("StateRAMH3Native" -as [type])) {
    Add-Type -TypeDefinition $signature
}

$port = [IntPtr]::Zero
$hr = [StateRAMH3Native]::FilterConnectCommunicationPort(
    "\StateRAMH3Port",
    0,
    [IntPtr]::Zero,
    0,
    [IntPtr]::Zero,
    [ref]$port)

if ($hr -ne 0) {
    throw ("FilterConnectCommunicationPort failed: 0x{0:X8}" -f ([uint32]$hr))
}

try {
    $cmd = New-Object StateRAMH3Native+Command
    $cmd.Version = 1
    $cmd.CommandId = if ($Command -eq "reset") { 2 } else { 1 }

    $reply = New-Object StateRAMH3Native+Counters
    [uint32]$returned = 0

    $hr = [StateRAMH3Native]::FilterSendMessage(
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
        Version              = $reply.Version
        TotalReads           = $reply.TotalReads
        TotalReadMiB         = [math]::Round($reply.TotalReadBytes / 1MB, 3)
        TotalWrites          = $reply.TotalWrites
        TotalWriteMiB        = [math]::Round($reply.TotalWriteBytes / 1MB, 3)
        PagingReads          = $reply.PagingReads
        PagingReadMiB        = [math]::Round($reply.PagingReadBytes / 1MB, 3)
        PagingWrites         = $reply.PagingWrites
        PagingWriteMiB       = [math]::Round($reply.PagingWriteBytes / 1MB, 3)
        PagefileReads        = $reply.PagefileReads
        PagefileReadMiB      = [math]::Round($reply.PagefileReadBytes / 1MB, 3)
        PagefileWrites       = $reply.PagefileWrites
        PagefileWriteMiB     = [math]::Round($reply.PagefileWriteBytes / 1MB, 3)
        PagingFileCreates    = $reply.PagingFileCreates
    } | Format-List
}
finally {
    if ($port -ne [IntPtr]::Zero) {
        [void][StateRAMH3Native]::CloseHandle($port)
    }
}
