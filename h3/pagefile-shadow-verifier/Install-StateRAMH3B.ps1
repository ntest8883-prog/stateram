param(
    [string]$Altitude = "370006.5"
)

$ErrorActionPreference = "Stop"

function Test-StateRAMH3BLoaded {
    $lines = & fltmc filters 2>$null
    return [bool]($lines | Select-String -Pattern '^\s*StateRAMH3B\s')
}

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from Administrator PowerShell."
}

$source = Join-Path $PSScriptRoot "StateRAMH3B.sys"
$dest = "$env:SystemRoot\System32\drivers\StateRAMH3B.sys"

if (-not (Test-Path $source)) {
    throw "StateRAMH3B.sys not found beside this script."
}

# Never overwrite the on-disk driver while an older copy is still loaded.
if (Test-StateRAMH3BLoaded) {
    & fltmc unload StateRAMH3B | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Could not unload the existing StateRAMH3B filter."
    }

    Start-Sleep -Milliseconds 300
    if (Test-StateRAMH3BLoaded) {
        throw "StateRAMH3B is still loaded after fltmc unload. Stop here; do not replace the driver."
    }
}

& sc.exe query StateRAMH3B *> $null
if ($LASTEXITCODE -eq 0) {
    & sc.exe delete StateRAMH3B | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Could not delete the existing StateRAMH3B service."
    }

    $deleted = $false
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 250
        & sc.exe query StateRAMH3B *> $null
        if ($LASTEXITCODE -ne 0) {
            $deleted = $true
            break
        }
    }

    if (-not $deleted) {
        throw "The old StateRAMH3B service is still present after 5 seconds. Stop here rather than installing over uncertain state."
    }
}

Copy-Item $source $dest -Force

$sourceHash = (Get-FileHash $source -Algorithm SHA256).Hash
$destHash = (Get-FileHash $dest -Algorithm SHA256).Hash

if ($sourceHash -ne $destHash) {
    throw "Driver copy hash mismatch. Source=$sourceHash Destination=$destHash"
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

$svc = Get-ItemProperty $base
$inst = Get-ItemProperty "$base\Instances\StateRAMH3B Instance"

if ([int]$svc.Start -ne 3) {
    throw "Install verification failed: expected DEMAND_START (3), got $($svc.Start)."
}

if ([int]$inst.Flags -ne 1) {
    throw "Install verification failed: expected instance Flags=1, got $($inst.Flags)."
}

Write-Host "Installed StateRAMH3B as DEMAND_START."
Write-Host "Automatic attachment is suppressed for the first manual load."
Write-Host "Temporary local-lab altitude: $Altitude"
Write-Host "Driver SHA256: $sourceHash"
Write-Host "Install verification: PASS"
