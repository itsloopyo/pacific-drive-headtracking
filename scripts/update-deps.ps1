#!/usr/bin/env pwsh
#Requires -Version 5.1
# Bump vendored Ultimate ASI Loader (dinput8.dll) to the latest upstream
# within the pinned range. Manual; commit the result. CI never refreshes.
#
# Special case: Ultimate-ASI-Loader ships a DLL inside a release zip, not as a
# standalone asset, so Update-VendoredLoader (which stores the asset verbatim)
# is not used. We extract dinput8.dll and vendor it bare, matching what
# install.cmd copies into the game.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

$module = Join-Path $projectDir 'cameraunlock-core/powershell/ModLoaderSetup.psm1'
if (-not (Test-Path $module)) {
    throw "ModLoaderSetup.psm1 not found at $module. Run 'git submodule update --init --recursive'."
}
Import-Module $module -Force

$vendorAsiDir = Join-Path $projectDir 'vendor/ultimate-asi-loader'
$vendorAsiDll = Join-Path $vendorAsiDir 'dinput8.dll'
if (-not (Test-Path $vendorAsiDir)) {
    New-Item -ItemType Directory -Path $vendorAsiDir -Force | Out-Null
}

$tempZip = Join-Path $env:TEMP ("asi-update-" + [IO.Path]::GetRandomFileName() + ".zip")
try {
    Write-Host "Refreshing vendor/ultimate-asi-loader from upstream..." -ForegroundColor Cyan
    $meta = Invoke-FetchLatestLoader `
        -OutputPath $tempZip `
        -Owner 'ThirteenAG' -Repo 'Ultimate-ASI-Loader' `
        -VersionPrefix 'v9.' `
        -AssetPattern '^Ultimate-ASI-Loader_x64\.zip$'

    # Stage the extracted DLL, then only touch the vendor tree if it actually
    # differs. Rewriting unconditionally dirties README.md with a fresh
    # "Fetched at" on every run even when upstream has not moved, which reads
    # as a loader bump in review when nothing was bumped.
    $stagedDll = Join-Path $env:TEMP ("asi-dll-" + [IO.Path]::GetRandomFileName())
    $stagedLicense = Join-Path $env:TEMP ("asi-license-" + [IO.Path]::GetRandomFileName())
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($tempZip)
    try {
        $dllEntry = $zip.Entries | Where-Object { $_.Name -eq 'dinput8.dll' } | Select-Object -First 1
        if (-not $dllEntry) { throw "Upstream zip $($meta.AssetName) does not contain dinput8.dll." }
        $out = [System.IO.File]::Create($stagedDll)
        try { $in = $dllEntry.Open(); try { $in.CopyTo($out) } finally { $in.Dispose() } } finally { $out.Dispose() }

        $licenseEntry = $zip.Entries | Where-Object { $_.Name -match '^(license|LICENSE)(\..+)?$' -and $_.FullName -notmatch '/.+/' } | Select-Object -First 1
        if ($licenseEntry) {
            $out = [System.IO.File]::Create($stagedLicense)
            try { $in = $licenseEntry.Open(); try { $in.CopyTo($out) } finally { $in.Dispose() } } finally { $out.Dispose() }
        }
    } finally { $zip.Dispose() }

    $stagedSha = (Get-FileHash -Path $stagedDll -Algorithm SHA256).Hash.ToLower()
    $vendorReadme = Join-Path $vendorAsiDir 'README.md'
    $vendorLicense = Join-Path $vendorAsiDir 'LICENSE'
    $unchanged = (Test-Path $vendorAsiDll) -and (Test-Path $vendorReadme) -and (Test-Path $vendorLicense) -and
                 ((Get-FileHash -Path $vendorAsiDll -Algorithm SHA256).Hash.ToLower() -eq $stagedSha)
    if ($unchanged) {
        Remove-Item $stagedDll, $stagedLicense -Force -ErrorAction SilentlyContinue
        Write-Host "  no change (tag=$($meta.Tag) sha256=$($stagedSha.Substring(0,12))... matches vendored copy)" -ForegroundColor DarkGray
        return
    }

    Move-Item -LiteralPath $stagedDll -Destination $vendorAsiDll -Force
    if (Test-Path $stagedLicense) { Move-Item -LiteralPath $stagedLicense -Destination $vendorLicense -Force }

    if (-not (Test-Path (Join-Path $vendorAsiDir 'LICENSE'))) {
        $licenseUrl = "https://raw.githubusercontent.com/ThirteenAG/Ultimate-ASI-Loader/$($meta.Tag)/license"
        Invoke-WebRequest -Uri $licenseUrl -OutFile (Join-Path $vendorAsiDir 'LICENSE') -UseBasicParsing -TimeoutSec 30 -Headers @{ "User-Agent" = "CameraUnlock-HeadTracking" }
    }

    $dllSha = $stagedSha
    $readme = @(
        '# Ultimate ASI Loader (vendored)',
        '',
        'Bundled copy of Ultimate ASI Loader, the install-time source of truth.',
        'Refresh manually with `pixi run update-deps`, then commit.',
        '',
        '## Snapshot',
        '',
        '- Upstream: https://github.com/ThirteenAG/Ultimate-ASI-Loader',
        "- Tag: ``$($meta.Tag)``",
        "- Commit: ``$($meta.CommitSha)``",
        "- Asset: ``$($meta.AssetName)``",
        "- Upstream URL: $($meta.AssetUrl)",
        "- dinput8.dll SHA-256: ``$dllSha``",
        "- Fetched at: $($meta.FetchedAt)",
        '',
        '`dinput8.dll` is extracted from the upstream asset untouched. install.cmd copies it to',
        '`winmm.dll` next to the game exe - `PenDriverPro/Binaries/Win64/` on Steam,',
        '`PenDriverPro/Binaries/WinGDK/` on Xbox Game Pass - as the ASI hook slot (winmm is a',
        'static import of the UE4 shipping exe, so the loader gets in without a dinput8',
        'dependency).'
    ) -join "`n"
    Set-Content -Path (Join-Path $vendorAsiDir 'README.md') -Value $readme -Encoding UTF8

    Write-Host "  tag=$($meta.Tag) sha256=$($dllSha.Substring(0,12))..." -ForegroundColor DarkGray
} finally {
    Remove-Item $tempZip -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "vendor/ultimate-asi-loader refreshed. Review and commit." -ForegroundColor Green
