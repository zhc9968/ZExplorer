[CmdletBinding()]
param(
    [string]$Version = '',
    [ValidateSet('Release')]
    [string]$Configuration = 'Release',
    [string]$OutputDirectory = 'release'
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$root = Split-Path -Parent $PSScriptRoot
if (-not $Version) {
    $Version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
}
$Version = $Version.Trim().TrimStart('v')
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Invalid release version: $Version"
}

$rc = Get-Content -LiteralPath (Join-Path $root 'app.rc') -Raw
$match = [regex]::Match($rc, 'FILEVERSION\s+(\d+),(\d+),(\d+),(\d+)')
if (-not $match.Success) {
    throw 'Could not read FILEVERSION from app.rc.'
}
$rcVersion = "$($match.Groups[1].Value).$($match.Groups[2].Value).$($match.Groups[3].Value)"
if ($rcVersion -ne $Version) {
    throw "Version mismatch: VERSION=$Version app.rc=$rcVersion"
}

$releaseRoot = Join-Path $root $OutputDirectory
$stageName = "ZExplorer-$Version-win64"
$stage = Join-Path $releaseRoot $stageName
$zipName = "$stageName.zip"
$zip = Join-Path $releaseRoot $zipName

New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
if (Test-Path -LiteralPath $stage) {
    $resolvedRelease = [IO.Path]::GetFullPath($releaseRoot)
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    if (-not $resolvedStage.StartsWith($resolvedRelease, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove staging path outside release directory: $resolvedStage"
    }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
if (Test-Path -LiteralPath $zip) {
    [IO.File]::Delete($zip)
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$bin = Join-Path $root "x64\$Configuration"
$required = @(
    (Join-Path $bin 'ZExplorer.exe'),
    (Join-Path $bin 'FileLockLab.exe'),
    (Join-Path $bin 'CrossVolumeLinkLab.exe')
)
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Release binary is missing: $file"
    }
}

$diagnostics = Join-Path $stage 'diagnostics'
New-Item -ItemType Directory -Force -Path $diagnostics | Out-Null
Copy-Item -LiteralPath (Join-Path $bin 'ZExplorer.exe') -Destination $stage
Copy-Item -LiteralPath (Join-Path $bin 'FileLockLab.exe') -Destination $diagnostics
Copy-Item -LiteralPath (Join-Path $bin 'CrossVolumeLinkLab.exe') -Destination $diagnostics
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'CHANGELOG.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $stage

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText(
    (Join-Path $stage 'VERSION.txt'),
    "$Version`r`n",
    $utf8NoBom)

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$hashFile = "$zip.sha256"
[IO.File]::WriteAllText($hashFile, "$hash  $zipName`r`n", $utf8NoBom)

Write-Host "Release package: $zip"
Write-Host "SHA-256: $hash"
