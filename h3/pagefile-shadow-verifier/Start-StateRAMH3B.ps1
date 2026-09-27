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
    throw "StateRAMH3B is already loaded. Stop here so the manual-load test starts from a known state."
}

& fltmc load StateRAMH3B | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "fltmc load StateRAMH3B failed with exit code $LASTEXITCODE"
}

& fltmc attach StateRAMH3B C: | Out-Host
if ($LASTEXITCODE -ne 0) {
    & fltmc unload StateRAMH3B 2>$null | Out-Null
    throw "Failed to attach StateRAMH3B to C:"
}

& fltmc attach StateRAMH3B D: | Out-Host
if ($LASTEXITCODE -ne 0) {
    & fltmc detach StateRAMH3B C: 2>$null | Out-Null
    & fltmc unload StateRAMH3B 2>$null | Out-Null
    throw "Failed to attach StateRAMH3B to D:"
}

$instances = & fltmc instances -f StateRAMH3B
if ($LASTEXITCODE -ne 0) {
    & fltmc unload StateRAMH3B 2>$null | Out-Null
    throw "Could not verify StateRAMH3B instances after attachment."
}

$cAttached = [bool]($instances | Select-String -Pattern '^\s*C:\s')
$dAttached = [bool]($instances | Select-String -Pattern '^\s*D:\s')

if (-not ($cAttached -and $dAttached)) {
    $instances | Out-Host
    & fltmc unload StateRAMH3B 2>$null | Out-Null
    throw "Manual attachment verification failed. Expected both C: and D:."
}

$instances | Out-Host
Write-Host "StateRAMH3B manual load/attach verification: PASS"
