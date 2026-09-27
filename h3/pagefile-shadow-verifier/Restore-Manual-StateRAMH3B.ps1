$ErrorActionPreference = "Continue"

& fltmc unload StateRAMH3B 2>$null

& sc.exe config StateRAMH3B start= demand depend= FltMgr | Out-Host

$instance = "HKLM:\SYSTEM\CurrentControlSet\Services\StateRAMH3B\Instances\StateRAMH3B Instance"
Set-ItemProperty $instance -Name Flags -Value 1

Write-Host "StateRAMH3B restored to DEMAND_START with automatic attachment suppressed."
