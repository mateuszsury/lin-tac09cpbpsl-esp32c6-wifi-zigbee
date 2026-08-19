param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [string]$Firmware = "build\bridge-0.2.1\klima_wifi.bin",
    [string]$ExpectedSha256 = "77E6C524837521BCA5FBDF4E010BD9714A373716823A25B87F3513FE077B6489"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$image = (Resolve-Path -LiteralPath (Join-Path $root $Firmware)).Path
$actualSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $image).Hash

if ($ExpectedSha256 -and $actualSha256 -cne $ExpectedSha256) {
    throw "Firmware SHA-256 mismatch: $actualSha256 (expected $ExpectedSha256)"
}

Write-Host "Firmware image: $image"
Write-Host "SHA-256: $actualSha256"

Write-Host "WARNING: disconnect the air conditioner from 230 V mains power."
Write-Host "Disconnect the ESP from the air conditioner's GPIO4/5/6/7 and GND lines."
$confirmation = Read-Host "Enter DISCONNECTED to continue"
if ($confirmation -cne "DISCONNECTED") {
    throw "Cancelled without changes"
}

& py -3 -m esptool `
    --chip esp32c6 `
    --port $Port `
    --baud 460800 `
    --before default-reset `
    --after hard-reset `
    write-flash 0x10000 $image
if ($LASTEXITCODE -ne 0) {
    throw "esptool exited with code $LASTEXITCODE"
}

Write-Host "Flashed $image"
Write-Host "Wait for http://klima-wifi.local/api/status, then disconnect USB before reconnecting the air conditioner."
