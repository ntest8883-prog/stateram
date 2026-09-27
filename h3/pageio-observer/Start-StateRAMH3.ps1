$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from Administrator PowerShell."
}

& fltmc load StateRAMH3
if ($LASTEXITCODE -ne 0) { throw "fltmc load StateRAMH3 failed with exit code $LASTEXITCODE" }

& fltmc attach StateRAMH3 C:
if ($LASTEXITCODE -ne 0) {
    & fltmc unload StateRAMH3 2>$null | Out-Null
    throw "Failed to attach StateRAMH3 to C:"
}

& fltmc attach StateRAMH3 D:
if ($LASTEXITCODE -ne 0) {
    & fltmc detach StateRAMH3 C: 2>$null | Out-Null
    & fltmc unload StateRAMH3 2>$null | Out-Null
    throw "Failed to attach StateRAMH3 to D:"
}

Write-Host "StateRAMH3 loaded and attached to C: and D:."
& fltmc instances StateRAMH3
