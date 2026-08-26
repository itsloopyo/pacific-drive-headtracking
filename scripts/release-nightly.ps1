[CmdletBinding()]
param([switch]$AllowDirty)
$ErrorActionPreference = 'Stop'
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
Import-Module (Join-Path $ProjectRoot 'cameraunlock-core\powershell\NightlyRelease.psm1') -Force

$versionFile = Join-Path $ProjectRoot 'src\version.h'
$versionMatch = Select-String -Path $versionFile -Pattern 'kVersion\s*=\s*"([^"]+)"'
if (-not $versionMatch) {
    throw "Could not extract version from $versionFile"
}
$version = $versionMatch.Matches[0].Groups[1].Value

Publish-NightlyBuild `
    -ModId 'pacific-drive' `
    -ModName 'PacificDriveHeadTracking' `
    -Version $version `
    -ProjectRoot $ProjectRoot `
    -AllowDirty:$AllowDirty
