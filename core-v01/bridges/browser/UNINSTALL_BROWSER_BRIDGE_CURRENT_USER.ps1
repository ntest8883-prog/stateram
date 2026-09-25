$ErrorActionPreference = "Stop"

$hostName = "com.stateram.browser"
$manifestPath = Join-Path $PSScriptRoot "com.stateram.browser.json"

$keys = @(
    "HKCU:\Software\Google\Chrome\NativeMessagingHosts\$hostName",
    "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\$hostName",
    "HKCU:\Software\Chromium\NativeMessagingHosts\$hostName"
)

foreach ($key in $keys) {
    if (Test-Path $key) {
        Remove-Item -Path $key -Recurse -Force
    }
}

if (Test-Path $manifestPath) {
    Remove-Item -Path $manifestPath -Force
}

Write-Host "StateRAM Browser Bridge native host registration removed."
Write-Host "Remove the unpacked extension from the browser extensions page separately."
