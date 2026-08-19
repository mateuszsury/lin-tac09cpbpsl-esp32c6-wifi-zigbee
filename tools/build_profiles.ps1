[CmdletBinding()]
param(
    [ValidateSet('SNIFFER','PASSIVE','PANEL_DIAG','PANEL_BENCH','BRIDGE','MITM_NTS')]
    [string]$Profile = 'SNIFFER',
    [ValidateSet('MQTT','ZIGBEE','NONE')]
    [string]$Transport = 'MQTT',
    [string]$BuildRoot = 'build\release',
    [string]$WslIdfExport = $env:KLIMA_WSL_IDF_EXPORT,
    [switch]$Clean,
    [switch]$Manifest
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$profileSlug = if ($Profile -eq 'MITM_NTS') { 'mitm-nts' } else { $Profile.ToLowerInvariant() -replace '_', '-' }
$buildDir = Join-Path $repo (Join-Path $BuildRoot ("{0}-{1}" -f $profileSlug, $Transport.ToLowerInvariant()))

# Only remove the explicitly selected build directory.  Never clean the
# workspace root or another profile's artifacts.
if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

Push-Location $repo
try {
    # The supported toolchain is ESP-IDF in WSL.  Keep a native idf.py path
    # for developers who already exported IDF in PowerShell, but never assume
    # that a Windows Python environment contains ESP-IDF.
    $idfCommand = Get-Command idf.py -ErrorAction SilentlyContinue
    if ($idfCommand) {
        & idf.py -B $buildDir "-DKLIMA_BRIDGE_MODE=$Profile" "-DKLIMA_HA_TRANSPORT=$Transport" build
    } else {
        # wsl.exe argument conversion strips Windows backslashes before
        # wslpath sees them, so map the already-resolved drive explicitly.
        $wslRepo = "/mnt/$($repo.Substring(0, 1).ToLower())" + (($repo.Substring(2)) -replace '\\', '/')
        $wslBuild = "$wslRepo/build/release/$profileSlug-$($Transport.ToLowerInvariant())"
        if (-not $WslIdfExport) {
            throw 'Set KLIMA_WSL_IDF_EXPORT to the WSL path of ESP-IDF export.sh, or export idf.py in PowerShell.'
        }
        if ($WslIdfExport.Contains("'")) { throw 'KLIMA_WSL_IDF_EXPORT must not contain a single quote.' }
        $wslCmd = "source '$WslIdfExport' >/dev/null && cd '$wslRepo' && idf.py -B '$wslBuild' -DKLIMA_BRIDGE_MODE=$Profile -DKLIMA_HA_TRANSPORT=$Transport build"
        & wsl.exe bash -lc $wslCmd
    }
    if ($LASTEXITCODE -ne 0) { throw "idf.py build failed ($LASTEXITCODE)" }
    if ($Manifest) {
        & py -3 -B tools\make_release_manifest.py --build $buildDir --profile $Profile --transport $Transport
        if ($LASTEXITCODE -ne 0) { throw "manifest generation failed ($LASTEXITCODE)" }
    }
} finally {
    Pop-Location
}
