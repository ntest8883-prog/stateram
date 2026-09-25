$ErrorActionPreference = "Stop"

$hostName = "com.stateram.browser"
$extensionId = "joobkllejoacdkjggddlbgljgkokccge"
$exePath = Join-Path $PSScriptRoot "StateRAM_Browser_NativeHost.exe"
$manifestPath = Join-Path $PSScriptRoot "com.stateram.browser.json"

if (-not (Test-Path $exePath)) {
    throw "Missing native host executable: $exePath"
}

$manifest = [ordered]@{
    name = $hostName
    description = "StateRAM Browser Bridge native host"
    path = $exePath
    type = "stdio"
    allowed_origins = @(
        "chrome-extension://$extensionId/"
    )
}

$manifest |
    ConvertTo-Json -Depth 5 |
    Set-Content -Encoding UTF8 $manifestPath

$keys = @(
    "HKCU:\Software\Google\Chrome\NativeMessagingHosts\$hostName",
    "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\$hostName",
    "HKCU:\Software\Chromium\NativeMessagingHosts\$hostName"
)

foreach ($key in $keys) {
    New-Item -Path $key -Force | Out-Null
    Set-Item -Path $key -Value $manifestPath
}

Write-Host ""
Write-Host "StateRAM Browser Bridge native host registered for the current user."
Write-Host "No Administrator rights were used."
Write-Host ""
Write-Host "Extension ID: $extensionId"
Write-Host ""
Write-Host "Now load the unpacked extension folder named:"
Write-Host "  browser-extension"
Write-Host ""
Write-Host "Chrome: chrome://extensions"
Write-Host "Edge:   edge://extensions"
Write-Host ""
Write-Host "Turn on Developer mode, choose Load unpacked, and select browser-extension."
Write-Host ""
Write-Host "The StateRAM Runtime Host must also be running for the bridge to reclaim tabs."
