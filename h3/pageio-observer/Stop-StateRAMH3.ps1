$ErrorActionPreference = "Continue"
& fltmc detach StateRAMH3 D: 2>$null
& fltmc detach StateRAMH3 C: 2>$null
& fltmc unload StateRAMH3
Write-Host "StateRAMH3 stop sequence complete."
