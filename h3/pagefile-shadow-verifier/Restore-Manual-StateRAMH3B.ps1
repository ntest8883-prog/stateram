$ErrorActionPreference = "Stop"

function Test-StateRAMH3BLoaded {
    $lines = & fltmc filters 2>$null
    return [bool]($lines | Select-String -Pattern '^\s*StateRAMH3B\s')
}

if (Test-StateRAMH3BLoaded) {
    & fltmc unload StateRAMH3B | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Could not unload StateRAMH3B while restoring manual state."
    }
}

$base = "HKLM:\SYSTEM\CurrentControlSet\Services\StateRAMH3B"
$instance = "$base\Instances\StateRAMH3B Instance"

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

Write-Host "StateRAMH3B restored to DEMAND_START with automatic attachment suppressed."
Write-Host "Restore verification: PASS"
