$ErrorActionPreference = "Stop"

function Test-StateRAMH3BLoaded {
    $lines = & fltmc filters 2>$null
    return [bool]($lines | Select-String -Pattern '^\s*StateRAMH3B\s')
}

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from Administrator PowerShell."
}

$base = "HKLM:\SYSTEM\CurrentControlSet\Services\StateRAMH3B"
$instance = "$base\Instances\StateRAMH3B Instance"

# Make the NEXT boot safe first, even if unloading the currently running copy fails.
& sc.exe config StateRAMH3B start= demand depend= FltMgr | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Could not restore StateRAMH3B to DEMAND_START."
}

Set-ItemProperty $instance -Name Flags -Value 1

$svc = Get-ItemProperty $base
$inst = Get-ItemProperty $instance

if ([int]$svc.Start -ne 3 -or [int]$inst.Flags -ne 1) {
    throw "Restore verification failed. Expected Start=3 and Flags=1."
}

if (Test-StateRAMH3BLoaded) {
    & fltmc unload StateRAMH3B | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Future boot state is safe, but the currently loaded StateRAMH3B copy could not be unloaded."
    }

    Start-Sleep -Milliseconds 300
    if (Test-StateRAMH3BLoaded) {
        throw "Future boot state is safe, but StateRAMH3B still appears loaded."
    }
}

Write-Host "StateRAMH3B restored to DEMAND_START with automatic attachment suppressed."
Write-Host "Restore verification: PASS"
