$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from Administrator PowerShell."
}

& fltmc unload StateRAMH3B 2>$null | Out-Null

$instance = "HKLM:\SYSTEM\CurrentControlSet\Services\StateRAMH3B\Instances\StateRAMH3B Instance"
Set-ItemProperty $instance -Name Flags -Value 0

& sc.exe config StateRAMH3B start= boot depend= FltMgr
if ($LASTEXITCODE -ne 0) {
    throw "Failed to configure StateRAMH3B as BOOT_START."
}

Write-Host "StateRAMH3B is prepared for ONE boot-time test."
Write-Host "Use Startup Settings option 7/F7 to disable signature enforcement for that boot."
