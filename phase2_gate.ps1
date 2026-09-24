param(
    [Parameter(Mandatory=$true)][string[]]$Files
)

$ErrorActionPreference = "Stop"
$all = @()
$ok = $true

foreach ($f in $Files) {
    $r = Get-Content $f -Raw | ConvertFrom-Json
    $all += $r
    if (-not $r.pass) { $ok = $false }
    if ($r.integrity_errors -ne 0) { $ok = $false }
    if ($r.commit_failures -ne 0) { $ok = $false }
    if ($r.evictions -le 0) { $ok = $false }
    if (-not $r.hard_limit_active) { $ok = $false }
}

$summary = [ordered]@{
    phase = "Windows Phase 2 user-mode engine"
    results = $all
    gate_pass = $ok
}

$summary | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 windows_phase2_summary.json
$summary | ConvertTo-Json -Depth 6

if ($ok) {
    Write-Host "WINDOWS_STATERAM_PHASE2_GATE=PASS"
    exit 0
} else {
    Write-Host "WINDOWS_STATERAM_PHASE2_GATE=FAIL"
    exit 10
}
