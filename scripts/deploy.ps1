[CmdletBinding()]
param([Parameter(Position = 0)][string]$GamePath)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

Import-Module (Join-Path $root 'cameraunlock-core/powershell/GamePathDetection.psm1') -Force

$asi = Join-Path $root 'build/Release/PacificDriveHeadTracking.asi'
if (-not (Test-Path $asi)) {
    Write-Host "ERROR: build output not found at $asi. Run 'pixi run build' first." -ForegroundColor Red
    exit 1
}

# Same resolution order install.cmd uses: an explicitly supplied path wins,
# otherwise Find-GamePath walks env var -> registry -> Steam libraries from
# the games.json entry.
if ($GamePath) {
    if (-not (Test-Path -LiteralPath $GamePath -PathType Container)) {
        Write-Host "ERROR: supplied game path is not a directory: $GamePath" -ForegroundColor Red
        exit 1
    }
} else {
    $GamePath = Find-GamePath -GameId 'pacific-drive'
}
if (-not $GamePath) {
    Write-Host "ERROR: Pacific Drive install not found. Set PACIFIC_DRIVE_PATH or pass the path as the first argument." -ForegroundColor Red
    exit 1
}

$cfg = Get-GameConfig -GameId 'pacific-drive'
$exe = Join-Path $GamePath $cfg.Executable
if (-not (Test-Path $exe)) {
    Write-Host "ERROR: game exe not found at $exe." -ForegroundColor Red
    exit 1
}
$exeDir = Split-Path $exe

# ASI_LOADER_NAME in install.cmd is winmm.dll; the vendored artifact ships as
# dinput8.dll and is renamed on deploy, exactly as the installer does it.
$loader = Join-Path $exeDir 'winmm.dll'
if (-not (Test-Path $loader)) {
    Copy-Item (Join-Path $root 'vendor/ultimate-asi-loader/dinput8.dll') $loader -Force
    Write-Host "Deployed Ultimate ASI Loader -> winmm.dll" -ForegroundColor Green
}

Copy-Item $asi (Join-Path $exeDir 'PacificDriveHeadTracking.asi') -Force
$iniDst = Join-Path $exeDir 'PacificDriveHeadTracking.ini'
if (-not (Test-Path $iniDst)) {
    Copy-Item (Join-Path $root 'PacificDriveHeadTracking.ini') $iniDst -Force
    Write-Host "Deployed default PacificDriveHeadTracking.ini" -ForegroundColor Green
}
Write-Host "Deployed PacificDriveHeadTracking.asi to $exeDir" -ForegroundColor Green
