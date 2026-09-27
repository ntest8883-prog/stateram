param(
    [string]$Altitude = "370005.5"
)

$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from Administrator PowerShell."
}

$source = Join-Path $PSScriptRoot "StateRAMH3.sys"
$dest = "$env:SystemRoot\System32\drivers\StateRAMH3.sys"

if (-not (Test-Path $source)) {
    throw "StateRAMH3.sys not found beside this script."
}

Copy-Item $source $dest -Force

& sc.exe query StateRAMH3 *> $null
if ($LASTEXITCODE -eq 0) {
    & fltmc unload StateRAMH3 2>$null | Out-Null
    & sc.exe delete StateRAMH3 | Out-Null
    Start-Sleep -Milliseconds 300
}

& sc.exe create StateRAMH3 type= filesys start= demand error= normal binPath= "\SystemRoot\System32\drivers\StateRAMH3.sys" group= "FSFilter Activity Monitor"

if ($LASTEXITCODE -ne 0) {
    throw "sc.exe create failed with exit code $LASTEXITCODE"
}

$base = "HKLM:\SYSTEM\CurrentControlSet\Services\StateRAMH3"
New-Item "$base\Instances" -Force | Out-Null
New-ItemProperty "$base\Instances" -Name "DefaultInstance" -PropertyType String -Value "StateRAMH3 Instance" -Force | Out-Null

New-Item "$base\Instances\StateRAMH3 Instance" -Force | Out-Null
New-ItemProperty "$base\Instances\StateRAMH3 Instance" -Name "Altitude" -PropertyType String -Value $Altitude -Force | Out-Null
New-ItemProperty "$base\Instances\StateRAMH3 Instance" -Name "Flags" -PropertyType DWord -Value 1 -Force | Out-Null

Write-Host "Installed StateRAMH3 as DEMAND_START with suppressed automatic attachment."
Write-Host "Temporary local-lab altitude: $Altitude"
Write-Host "Do not load it yet unless instructed."
