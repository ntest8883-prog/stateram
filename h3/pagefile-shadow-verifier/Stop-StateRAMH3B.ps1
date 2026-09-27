$ErrorActionPreference = "Continue"

& fltmc detach StateRAMH3B D: 2>$null
& fltmc detach StateRAMH3B C: 2>$null
& fltmc unload StateRAMH3B

Write-Host "StateRAMH3B stop sequence complete."
