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

$cfg = Get-GameConfig -GameId 'pacific-drive'

# Every locally installed copy is a target. Steam and Xbox / Game Pass ship
# different binaries under different subtrees, and deploying to whichever one
# detection happened to return first is how a fix gets debugged twice: the
# build lands in the Steam copy, the shortcut opens the Game Pass one, and
# nothing errors.
if ($GamePath) {
    if (-not (Test-Path -LiteralPath $GamePath -PathType Container)) {
        Write-Host "ERROR: supplied game path is not a directory: $GamePath" -ForegroundColor Red
        exit 1
    }
    $targets = @($GamePath)
} else {
    $targets = @(Find-AllGamePaths -GameId 'pacific-drive')
}
if ($targets.Count -eq 0) {
    Write-Host "ERROR: no Pacific Drive install found. Set PACIFIC_DRIVE_PATH or pass the path as the first argument." -ForegroundColor Red
    exit 1
}

$deployed = 0
foreach ($target in $targets) {
    $isXbox = Test-IsXboxPath -Config $cfg -Path $target
    $store = if ($isXbox) { 'Xbox / Game Pass' } else { 'Steam' }
    $exeRelPath = if ($isXbox -and $cfg.ContainsKey('XboxExecutable') -and $cfg.XboxExecutable) {
        $cfg.XboxExecutable
    } else {
        $cfg.Executable
    }

    $exe = Join-Path $target $exeRelPath
    if (-not (Test-Path $exe)) {
        Write-Host "SKIP  $store" -ForegroundColor Yellow
        Write-Host "      game exe not found at $exe." -ForegroundColor Yellow
        continue
    }
    $exeDir = Split-Path $exe
    Write-Host "$store" -ForegroundColor Cyan
    Write-Host "      $exeDir"

    # ASI_LOADER_NAME in install.cmd is winmm.dll; the vendored artifact ships as
    # dinput8.dll and is renamed on deploy, exactly as the installer does it.
    $loader = Join-Path $exeDir 'winmm.dll'
    if (-not (Test-Path $loader)) {
        Copy-Item (Join-Path $root 'vendor/ultimate-asi-loader/dinput8.dll') $loader -Force
        Write-Host "      deployed Ultimate ASI Loader -> winmm.dll" -ForegroundColor Green
    }

    Copy-Item $asi (Join-Path $exeDir 'PacificDriveHeadTracking.asi') -Force
    $iniDst = Join-Path $exeDir 'PacificDriveHeadTracking.ini'
    if (-not (Test-Path $iniDst)) {
        Copy-Item (Join-Path $root 'PacificDriveHeadTracking.ini') $iniDst -Force
        Write-Host "      deployed default PacificDriveHeadTracking.ini" -ForegroundColor Green
    }
    Write-Host "      deployed PacificDriveHeadTracking.asi" -ForegroundColor Green
    $deployed++
}

if ($deployed -eq 0) {
    Write-Host "ERROR: found $($targets.Count) install(s) but deployed to none of them." -ForegroundColor Red
    exit 1
}
Write-Host "Deployed PacificDriveHeadTracking.asi to $deployed of $($targets.Count) install(s)." -ForegroundColor Green
