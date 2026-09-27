$ErrorActionPreference = "Stop"

function Test-StateRAMH3BLoaded {
    $lines = & fltmc filters 2>$null
    return [bool]($lines | Select-String -Pattern '^\s*StateRAMH3B\s')
}

if (-not (Test-StateRAMH3BLoaded)) {
    Write-Host "StateRAMH3B is already unloaded."
    exit 0
}

& fltmc detach StateRAMH3B D: 2>$null | Out-Null
& fltmc detach StateRAMH3B C: 2>$null | Out-Null
& fltmc unload StateRAMH3B | Out-Host

if ($LASTEXITCODE -ne 0) {
    throw "StateRAMH3B unload failed with exit code $LASTEXITCODE"
}

Start-Sleep -Milliseconds 300

if (Test-StateRAMH3BLoaded) {
    throw "StateRAMH3B still appears in fltmc after unload."
}

Write-Host "StateRAMH3B stop verification: PASS"
