$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from Administrator PowerShell."
}

& fltmc load StateRAMH3B
if ($LASTEXITCODE -ne 0) {
    throw "fltmc load StateRAMH3B failed with exit code $LASTEXITCODE"
}

& fltmc attach StateRAMH3B C:
if ($LASTEXITCODE -ne 0) {
    & fltmc unload StateRAMH3B 2>$null | Out-Null
    throw "Failed to attach StateRAMH3B to C:"
}

& fltmc attach StateRAMH3B D:
if ($LASTEXITCODE -ne 0) {
    & fltmc detach StateRAMH3B C: 2>$null | Out-Null
    & fltmc unload StateRAMH3B 2>$null | Out-Null
    throw "Failed to attach StateRAMH3B to D:"
}

Write-Host "StateRAMH3B loaded and attached to C: and D:."
& fltmc instances StateRAMH3B
