$ErrorActionPreference = "Stop"

function Test-StateRAMH3BLoaded {
    $lines = & fltmc filters 2>$null
    return [bool]($lines | Select-String -Pattern '^\s*StateRAMH3B\s')
}

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from Administrator PowerShell."
}

if (Test-StateRAMH3BLoaded) {
    & fltmc unload StateRAMH3B | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Could not unload StateRAMH3B before preparing the boot test."
    }

    Start-Sleep -Milliseconds 300
    if (Test-StateRAMH3BLoaded) {
        throw "StateRAMH3B is still loaded. Stop here; boot configuration was not changed."
    }
}

$base = "HKLM:\SYSTEM\CurrentControlSet\Services\StateRAMH3B"
$instance = "$base\Instances\StateRAMH3B Instance"

if (-not (Test-Path $instance)) {
    throw "StateRAMH3B instance registry key is missing. Reinstall the package instead of preparing an uncertain boot state."
}

Set-ItemProperty $instance -Name Flags -Value 0

& sc.exe config StateRAMH3B start= boot depend= FltMgr | Out-Host
if ($LASTEXITCODE -ne 0) {
    Set-ItemProperty $instance -Name Flags -Value 1
    throw "Failed to configure StateRAMH3B as BOOT_START."
}

try {
    $svc = Get-ItemProperty $base
    $inst = Get-ItemProperty $instance
    $deps = @($svc.DependOnService)

    if ([int]$svc.Start -ne 0) {
        throw "Boot preparation verification failed: expected Start=0, got $($svc.Start)."
    }

    if ([int]$inst.Flags -ne 0) {
        throw "Boot preparation verification failed: expected Flags=0, got $($inst.Flags)."
    }

    if (-not ($deps -contains "FltMgr")) {
        throw "Boot preparation verification failed: FltMgr dependency is missing."
    }

    if ([string]::IsNullOrWhiteSpace([string]$inst.Altitude)) {
        throw "Boot preparation verification failed: altitude is missing."
    }
}
catch {
    & sc.exe config StateRAMH3B start= demand depend= FltMgr *> $null
    Set-ItemProperty $instance -Name Flags -Value 1 -ErrorAction SilentlyContinue
    throw
}

Write-Host "StateRAMH3B is prepared for ONE boot-time test."
Write-Host "Verified: BOOT_START, FltMgr dependency, Altitude=$($inst.Altitude), Flags=0"
Write-Host "Use Startup Settings option 7/F7 to disable signature enforcement for that boot."
