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
    throw "Nieprawidlowy SHA-256 obrazu: $actualSha256 (oczekiwano $ExpectedSha256)"
}

Write-Host "Obraz: $image"
Write-Host "SHA-256: $actualSha256"

Write-Host "UWAGA: klimatyzator musi byc odlaczony od 230 V."
Write-Host "ESP musi byc odlaczone od GPIO4/5/6/7 i GND klimatyzatora."
$confirmation = Read-Host "Wpisz ODLACZONE, aby kontynuowac"
if ($confirmation -cne "ODLACZONE") {
    throw "Przerwano bez zmian"
}

& py -3 -m esptool `
    --chip esp32c6 `
    --port $Port `
    --baud 460800 `
    --before default-reset `
    --after hard-reset `
    write-flash 0x10000 $image
if ($LASTEXITCODE -ne 0) {
    throw "esptool zakonczyl sie kodem $LASTEXITCODE"
}

Write-Host "Wgrano $image"
Write-Host "Poczekaj na http://klima-wifi.local/api/status, a potem odlacz USB przed ponownym polaczeniem z klimatyzatorem."
