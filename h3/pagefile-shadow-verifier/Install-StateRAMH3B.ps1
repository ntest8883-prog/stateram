param(
    [string]$Altitude = "370006.5"
)

$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from Administrator PowerShell."
}

$source = Join-Path $PSScriptRoot "StateRAMH3B.sys"
$dest = "$env:SystemRoot\System32\drivers\StateRAMH3B.sys"

if (-not (Test-Path $source)) {
    throw "StateRAMH3B.sys not found beside this script."
}

Copy-Item $source $dest -Force

& sc.exe query StateRAMH3B *> $null
if ($LASTEXITCODE -eq 0) {
    & fltmc unload StateRAMH3B 2>$null | Out-Null
    & sc.exe delete StateRAMH3B | Out-Null
    Start-Sleep -Milliseconds 300
}

& sc.exe create StateRAMH3B type= filesys start= demand error= normal binPath= "\SystemRoot\System32\drivers\StateRAMH3B.sys" group= "FSFilter Activity Monitor" depend= FltMgr

if ($LASTEXITCODE -ne 0) {
    throw "sc.exe create failed with exit code $LASTEXITCODE"
}

$base = "HKLM:\SYSTEM\CurrentControlSet\Services\StateRAMH3B"

New-Item "$base\Instances" -Force | Out-Null
New-ItemProperty "$base\Instances" -Name "DefaultInstance" -PropertyType String -Value "StateRAMH3B Instance" -Force | Out-Null

New-Item "$base\Instances\StateRAMH3B Instance" -Force | Out-Null
New-ItemProperty "$base\Instances\StateRAMH3B Instance" -Name "Altitude" -PropertyType String -Value $Altitude -Force | Out-Null
New-ItemProperty "$base\Instances\StateRAMH3B Instance" -Name "Flags" -PropertyType DWord -Value 1 -Force | Out-Null

Write-Host "Installed StateRAMH3B as DEMAND_START."
Write-Host "Automatic attachment is suppressed for the first manual load."
Write-Host "Temporary local-lab altitude: $Altitude"
