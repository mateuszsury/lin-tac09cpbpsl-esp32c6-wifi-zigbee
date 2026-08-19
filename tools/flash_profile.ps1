[CmdletBinding()]
param(
    [ValidateSet('SNIFFER','PASSIVE','PANEL_DIAG','PANEL_BENCH','BRIDGE','MITM_NTS')]
    [string]$Profile = 'SNIFFER',
    [ValidateSet('MQTT','ZIGBEE','NONE')]
    [string]$Transport = 'MQTT',
    [string]$Port,
    [string]$WslIdfExport = $env:KLIMA_WSL_IDF_EXPORT,
    [switch]$Build,
    [switch]$ForceActive
)

$ErrorActionPreference = 'Stop'
if ($Profile -eq 'MITM_NTS' -and -not $ForceActive) {
    throw 'MITM_NTS is an explicit active artifact. Re-run with -ForceActive only after the physical HIL gate is signed off.'
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$profileSlug = if ($Profile -eq 'MITM_NTS') { 'mitm-nts' } else { $Profile.ToLowerInvariant() }
$buildDir = Join-Path $repo (Join-Path 'build\release' ("{0}-{1}" -f $profileSlug, $Transport.ToLowerInvariant()))
Push-Location $repo
try {
    if ($Build -or -not (Test-Path -LiteralPath (Join-Path $buildDir 'klima_wifi.bin'))) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_profiles.ps1 -Profile $Profile -Transport $Transport -WslIdfExport $WslIdfExport -Manifest
        if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
    }
    $args = @('-B', $buildDir, 'flash')
    if ($Port) { $args += @('-p', $Port) }
    $idfCommand = Get-Command idf.py -ErrorAction SilentlyContinue
    if ($idfCommand) {
        & idf.py @args
    } else {
        $wslRepo = "/mnt/$($repo.Substring(0, 1).ToLower())" + (($repo.Substring(2)) -replace '\\', '/')
        $wslBuild = "$wslRepo/build/release/$profileSlug-$($Transport.ToLowerInvariant())"
        $portArg = if ($Port) { " -p '$Port'" } else { '' }
        if (-not $WslIdfExport) {
            throw 'Set KLIMA_WSL_IDF_EXPORT to the WSL path of ESP-IDF export.sh, or export idf.py in PowerShell.'
        }
        if ($WslIdfExport.Contains("'")) { throw 'KLIMA_WSL_IDF_EXPORT must not contain a single quote.' }
        $wslCmd = "source '$WslIdfExport' >/dev/null && cd '$wslRepo' && idf.py -B '$wslBuild' flash$portArg"
        & wsl.exe bash -lc $wslCmd
    }
    if ($LASTEXITCODE -ne 0) { throw "flash failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
