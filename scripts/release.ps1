[CmdletBinding()]
param(
    # NOT Mandatory: PowerShell satisfies a missing mandatory parameter by
    # reading stdin, and `pixi run release` allocates no TTY - that prompt
    # dies with "IOException: The handle is invalid" instead of printing a
    # usage line. Validate it ourselves and fail fast.
    [Parameter(Position=0)][string]$Version,
    # Ship a release even when there are no user-facing commits since the
    # last tag (writes a maintenance changelog entry instead of aborting).
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

if (-not $Version) {
    Write-Host "Usage: pixi run release <major|minor|patch|nightly|X.Y.Z> [-Force]" -ForegroundColor Red
    exit 1
}

if ($Version -eq 'nightly') {
    & (Join-Path $PSScriptRoot 'release-nightly.ps1')
    exit $LASTEXITCODE
}

Import-Module (Join-Path $root 'cameraunlock-core/powershell/ReleaseWorkflow.psm1') -Force

# Windows PowerShell 5.1's `-Encoding utf8` means UTF-8 WITH a BOM, and pixi
# rejects a pixi.toml that starts with one ("Missing table in manifest"). The
# release then aborts inside `pixi run package`, after the version has already
# been stamped into five files and before the tag exists - a half-bumped tree
# with no way forward. Write raw text through .NET instead, which also leaves
# the file's existing line endings alone (Get-Content/Set-Content round-trips
# every file to CRLF).
function Set-TextFileNoBom {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding $false))
}

function Update-VersionInFile {
    param([string]$Path, [string]$Pattern, [string]$Replacement)
    $full = Join-Path $root $Path
    $text = [System.IO.File]::ReadAllText($full)
    $updated = $text -replace $Pattern, $Replacement
    if ($updated -eq $text) { throw "Version stamp did not match anything in $Path" }
    Set-TextFileNoBom -Path $full -Text $updated
}

# Mirrors New-ChangelogFromCommits' insertion so a -Force maintenance entry
# lands in the same place with the same shape.
function Add-MaintenanceChangelogEntry {
    param([string]$Path, [string]$NewVersion)
    $date = Get-Date -Format 'yyyy-MM-dd'
    $entry = "## [$NewVersion] - $date`n`n### Changed`n`n- Maintenance release (no user-facing changes).`n`n"
    $changelog = Get-Content $Path -Raw
    if ($changelog -match '(?s)(# Changelog.*?)(## \[)') {
        $changelog = $changelog -replace '(?s)(# Changelog.*?\n\n)', "`$1$entry"
    } else {
        $changelog = $changelog -replace '(?s)(# Changelog.*?\n)', "`$1$entry"
    }
    $changelog = $changelog.TrimEnd() + "`n"
    Set-TextFileNoBom -Path (Join-Path $root $Path) -Text $changelog
}

Push-Location $root
try {
    $verLine = Select-String -Path 'src/version.h' -Pattern 'kVersion\s*=\s*"([^"]+)"'
    $current = $verLine.Matches[0].Groups[1].Value
    $new = Resolve-ReleaseVersion -Argument $Version -CurrentVersion $current
    if (-not $new) { throw "Usage: pixi run release <major|minor|patch|nightly|X.Y.Z>" }

    # New-ReleaseTag pushes to `main`, so releasing from any other branch would
    # push commits the branch does not contain. Gate before anything mutates.
    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    if ($branch -ne 'main') { throw "Releases cut from 'main' only; currently on '$branch'." }
    if (-not (Test-CleanGitStatus)) { throw "Working tree is dirty - commit or stash first." }
    if (Test-GitTagExists -Tag "v$new") { throw "Tag v$new already exists." }

    # Generate CHANGELOG from commits since last tag. This is the gate that
    # aborts when there are no user-facing commits, so run it BEFORE mutating
    # any version files or building - a failure here then leaves a clean tree
    # instead of stranding a half-applied version bump with no tag.
    # New-ChangelogFromCommits handles the no-tags case itself (it ranges over
    # HEAD instead of lastTag..HEAD), so it runs whether or not this is the
    # first release. Skipping it on an untagged repo - which is what a
    # `git tag -l` guard did - shipped v0.1.0 with a CHANGELOG whose newest
    # entry still said 0.0.0, and bypassed the all-noise abort for that release.
    if (-not (Test-Path 'CHANGELOG.md')) {
        Set-TextFileNoBom -Path (Join-Path $root 'CHANGELOG.md') -Text "# Changelog`n`n"
    }
    try {
        New-ChangelogFromCommits -ChangelogPath 'CHANGELOG.md' -Version $new | Out-Null
    } catch {
        if (-not $Force) {
            Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host "No user-facing changes to release. Re-run with -Force for a maintenance release." -ForegroundColor Yellow
            exit 1
        }
        Write-Host "No user-facing commits since last tag - writing maintenance entry (-Force)." -ForegroundColor Yellow
        Add-MaintenanceChangelogEntry -Path 'CHANGELOG.md' -NewVersion $new
    }

    # Stamp the new version into every place it lives. src/version.h is
    # canonical; the rest are hand-kept copies and drift silently if skipped
    # (install.cmd prints MOD_VERSION to the user, the manifest is the
    # launcher's contract).
    $versionFiles = @('src/version.h','CMakeLists.txt','pixi.toml','launcher-manifest.json','scripts/install.cmd','CHANGELOG.md')
    Update-VersionInFile -Path 'src/version.h' -Pattern 'kVersion\s*=\s*"[^"]+"' -Replacement "kVersion = `"$new`""
    Update-VersionInFile -Path 'CMakeLists.txt' -Pattern 'project\(PacificDriveHeadTracking VERSION [0-9.]+' -Replacement "project(PacificDriveHeadTracking VERSION $new"
    Update-VersionInFile -Path 'pixi.toml' -Pattern '(?m)^version = "[0-9.]+"' -Replacement "version = `"$new`""
    # Targeted replace, not Update-ManifestVersion: that round-trips through
    # ConvertTo-Json and reformats the whole launcher contract file.
    $manifestText = [System.IO.File]::ReadAllText((Join-Path $root 'launcher-manifest.json'))
    $manifestText = $manifestText -replace '("version":\s*")\d+\.\d+\.\d+(")', "`${1}$new`$2"
    [System.IO.File]::WriteAllText((Join-Path $root 'launcher-manifest.json'), $manifestText, (New-Object System.Text.UTF8Encoding $false))
    # install.cmd is CRLF-sensitive; -replace on the raw text and a byte write
    # keep the line endings the file already has.
    $installCmd = [System.IO.File]::ReadAllText((Join-Path $root 'scripts/install.cmd'))
    $installCmd = $installCmd -replace '(?m)^set "MOD_VERSION=[0-9.]+"', "set `"MOD_VERSION=$new`""
    [System.IO.File]::WriteAllText((Join-Path $root 'scripts/install.cmd'), $installCmd, (New-Object System.Text.UTF8Encoding $false))

    # Build and package through the same pixi chain CI runs (configure ->
    # build -> package), not a bare `cmake --build`, which fails outright on a
    # checkout where build/ was never configured.
    & pixi run package
    if ($LASTEXITCODE -ne 0) { throw 'Build/packaging failed' }
    & pixi run validate
    if ($LASTEXITCODE -ne 0) { throw 'launcher-manifest.json does not describe the installer ZIP' }

    Invoke-VersionCommit -Version $new -Files $versionFiles | Out-Null
    New-ReleaseTag -Version $new -Message "Release v$new"
    Write-Host "Released v$new" -ForegroundColor Green
} finally {
    Pop-Location
}
