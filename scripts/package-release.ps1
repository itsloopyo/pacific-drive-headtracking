[CmdletBinding()]
param([switch]$NoNexus)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

Import-Module (Join-Path $root "cameraunlock-core\powershell\ReleaseWorkflow.psm1") -Force

# Version from src/version.h (kVersion).
$verLine = Select-String -Path (Join-Path $root 'src/version.h') -Pattern 'kVersion\s*=\s*"([^"]+)"'
if (-not $verLine) { throw 'Could not read kVersion from src/version.h' }
$version = $verLine.Matches[0].Groups[1].Value

$asi = Join-Path $root 'build/Release/PacificDriveHeadTracking.asi'
if (-not (Test-Path $asi)) { throw "Build output missing: $asi. Run 'pixi run build' first." }

# The manifest's loader.seed is a base64 copy of PacificDriveHeadTracking.ini,
# and it is what a launcher-deployed user actually gets. Hand-maintained, it
# drifted: the shipped seed was two revisions behind, missing every documented
# range and naming a log file that does not exist. Regenerate it here so the two
# cannot disagree, and say so when it moves - release.ps1 commits the manifest
# with the version bump, so a change lands in the same commit.
$manifestPath = Join-Path $root 'launcher-manifest.json'
$iniBytes = [System.IO.File]::ReadAllBytes((Join-Path $root 'PacificDriveHeadTracking.ini'))
$seed = [System.Convert]::ToBase64String($iniBytes)
$manifestText = [System.IO.File]::ReadAllText($manifestPath)
$updated = $manifestText -replace '("content_b64":\s*")[^"]*(")', "`${1}$seed`$2"
if ($updated -eq $manifestText) {
    if ($manifestText -notmatch '"content_b64"') { throw 'launcher-manifest.json has no content_b64 seed to refresh' }
} else {
    [System.IO.File]::WriteAllText($manifestPath, $updated, (New-Object System.Text.UTF8Encoding $false))
    Write-Host 'Refreshed launcher-manifest.json seed from PacificDriveHeadTracking.ini' -ForegroundColor Yellow
}

$rel = Join-Path $root 'release'
# No -ErrorAction SilentlyContinue: a locked release/ (an open Explorer window,
# a running AV scan, a ZIP still held) would otherwise be swallowed, staging
# would proceed into a directory still holding the previous build's files, and
# Compress-Archive would package the lot as a new release.
if (Test-Path $rel) { Remove-Item -Recurse -Force $rel }
$stage = Join-Path $rel 'installer'
$plugins = Join-Path $stage 'plugins'
$vendorDst = Join-Path $stage 'vendor/ultimate-asi-loader'
New-Item -ItemType Directory -Force $plugins, $vendorDst | Out-Null

# Plugin payload
Copy-Item $asi $plugins -Force
Copy-Item (Join-Path $root 'PacificDriveHeadTracking.ini') $plugins -Force

# Installer scripts + game-detection shim. Copy-SharedBundle stages the whole
# shim set (find-game.ps1, GamePathDetection.psm1, games.json); hand-copying
# only find-game.ps1 shipped an installer that aborted with "ZIP is corrupt"
# because the module it imports was missing from shared/.
Copy-Item (Join-Path $root 'scripts/install.cmd') $stage -Force
Copy-Item (Join-Path $root 'scripts/uninstall.cmd') $stage -Force
Copy-SharedBundle -StagingDir $stage

# Vendored loader (dll + LICENSE + README only; no scripts)
Copy-Item (Join-Path $root 'vendor/ultimate-asi-loader/dinput8.dll') $vendorDst -Force
Copy-Item (Join-Path $root 'vendor/ultimate-asi-loader/LICENSE') $vendorDst -Force
Copy-Item (Join-Path $root 'vendor/ultimate-asi-loader/README.md') $vendorDst -Force

# Docs
foreach ($f in 'README.md','LICENSE','CHANGELOG.md','THIRD-PARTY-NOTICES.md') {
    Copy-Item (Join-Path $root $f) $stage -Force
}

# Canonical launcher manifest. The launcher reads launcher-manifest.json from
# the ZIP root to deploy the package; there is no mod.json. Stamp the version
# from the build so the shipped manifest can never disagree with the built .asi.
$manifestPath = Join-Path $root 'launcher-manifest.json'
if (-not (Test-Path $manifestPath)) { throw "launcher-manifest.json not found at: $manifestPath" }
$manifestText = Get-Content $manifestPath -Raw
$manifestText = $manifestText -replace '("version":\s*")\d+\.\d+\.\d+(")', "`${1}$version`$2"
[System.IO.File]::WriteAllText((Join-Path $stage 'launcher-manifest.json'), $manifestText, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Staged: launcher-manifest.json (version $version)" -ForegroundColor Green

$installerZip = Join-Path $rel "PacificDriveHeadTracking-v$version-installer.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $installerZip -Force
Write-Host "Installer: $installerZip" -ForegroundColor Green

if (-not $NoNexus) {
    $nexusStage = Join-Path $rel 'nexus'
    New-Item -ItemType Directory -Force $nexusStage | Out-Null
    Copy-Item $asi $nexusStage -Force
    Copy-Item (Join-Path $root 'PacificDriveHeadTracking.ini') $nexusStage -Force
    $nexusZip = Join-Path $rel "PacificDriveHeadTracking-v$version-nexus.zip"
    # The Nexus ZIP is a binary distribution too: the licences of everything
    # compiled into or bundled with the payload require their notices to travel
    # with it, so LICENSE and THIRD-PARTY-NOTICES.md ship at its root.
    foreach ($noticeDoc in @('LICENSE', 'THIRD-PARTY-NOTICES.md', 'README.md')) {
        $noticeSrc = Join-Path $root $noticeDoc
        if (-not (Test-Path $noticeSrc)) {
            throw "Required notice file not found: $noticeDoc. Every published ZIP is a binary distribution and must carry it."
        }
        Copy-Item $noticeSrc -Destination $nexusStage -Force
        Write-Host "  $noticeDoc" -ForegroundColor Green
    }
    Compress-Archive -Path (Join-Path $nexusStage '*') -DestinationPath $nexusZip -Force
    Write-Host "Nexus: $nexusZip" -ForegroundColor Green
}
