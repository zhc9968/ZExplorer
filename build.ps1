# ============================================================================
# build.ps1 -- one-shot build for ZExplorer.
#
# ASCII-ONLY ON PURPOSE: PowerShell 5.1 decodes a BOM-less script with non-ASCII
# content using the system ANSI code page and fails to parse it. A build entry
# point that cannot be parsed is useless exactly when it is needed.
#
# PIPELINE
#   1. BOM guard   : ensure UTF-8 BOM for .h/.cpp that contain non-ASCII
#   2. kill        : stop ZExplorer so the link step can replace the exe
#   3. build       : msbuild ZExplorer.vcxproj
#   4. locklab     : compile the standalone file-lock laboratory
#   5. tests       : compile+run the headless platform tests (tools/run_tests.ps1)
#
# USAGE
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   ... -Configuration Debug | -Clean | -SkipBomFix | -CheckBomOnly | -SkipTests
#   ... -NoKill  (alias: -FalseKill)
#
# Exit code: 0 = ok; non-zero = the failing step's exit code.
# ============================================================================
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [string]$VsRoot = '',
    [switch]$Clean,
    [switch]$SkipBomFix,
    [switch]$CheckBomOnly,
    [switch]$SkipTests,
    [Alias('FalseKill')]
    [switch]$NoKill
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$root = $PSScriptRoot
$project = Join-Path $root 'ZExplorer.vcxproj'

function Step([string]$t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }
function Ok([string]$t) { Write-Host $t -ForegroundColor Green }
function Bad([string]$t) { Write-Host $t -ForegroundColor Red }

# ---------- BOM guard ----------
# Add a UTF-8 BOM only when the file contains non-ASCII bytes and has no BOM.
# Pure-ASCII files are left untouched (no pointless diffs).
function Invoke-BomGuard([bool]$checkOnly) {
    $missing = @()
    $exts = @('.h', '.hpp', '.cpp', '.cxx')
    $skipDirs = @('third_party', 'x64', '.vs', '.git')
    $files = Get-ChildItem -Path $root -Recurse -File |
        Where-Object { $exts -contains $_.Extension.ToLower() } |
        Where-Object {
            $rel = $_.FullName.Substring($root.Length)
            -not ($skipDirs | Where-Object { $rel -match [regex]::Escape("\$_\" ) })
        }
    foreach ($f in $files) {
        $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
        if ($bytes.Length -eq 0) { continue }
        $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
        $nonAscii = $false
        for ($i = 0; $i -lt $bytes.Length; $i++) { if ($bytes[$i] -gt 0x7F) { $nonAscii = $true; break } }
        if ($nonAscii -and -not $hasBom) {
            $missing += $f.FullName
            if (-not $checkOnly) {
                $text = [System.Text.Encoding]::UTF8.GetString($bytes)
                $enc = New-Object System.Text.UTF8Encoding($true)
                [System.IO.File]::WriteAllText($f.FullName, $text, $enc)
            }
        }
    }
    if ($missing.Count -gt 0) {
        foreach ($m in $missing) { Write-Host ("  missing BOM: " + $m.Substring($root.Length)) -ForegroundColor Yellow }
        if ($checkOnly) { return 1 }
        Ok ("  fixed: " + $missing.Count + " file(s) re-saved with UTF-8 BOM")
    } else {
        Ok '  all files OK (no missing BOM)'
    }
    return 0
}

if ($CheckBomOnly) {
    Step 'BOM check'
    exit (Invoke-BomGuard $true)
}

if (-not $SkipBomFix) {
    Step '1/5 ensure UTF-8 BOM'
    $r = Invoke-BomGuard $false
    if ($r -ne 0) { Bad 'BOM step failed.'; exit $r }
} else {
    Step '1/5 BOM step skipped (-SkipBomFix)'
}

# ---------- locate MSBuild ----------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { Bad "vswhere not found: $vswhere"; exit 2 }
if ($VsRoot) {
    $vsPath = $VsRoot
} else {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath) { Bad 'Visual Studio with C++ tools not found.'; exit 2 }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) { Bad "MSBuild not found: $msbuild"; exit 2 }

# ---------- kill holders ----------
Step '2/5 stop processes holding output files'
if (-not $NoKill) {
    foreach ($name in @('ZExplorer', 'FileLockLab')) {
        $procs = Get-Process -Name $name -ErrorAction SilentlyContinue
        if ($procs) {
            try {
                $procs | Stop-Process -Force -ErrorAction Stop
                Write-Host "  stopped $name ($($procs.Count))"
            }
            catch {
                $brokerClient = 'C:\project\ZBroker\x64\Release\ZBrokerClient.exe'
                if (Test-Path -LiteralPath $brokerClient) {
                    foreach ($proc in $procs) {
                        & $brokerClient kill "--pid=$($proc.Id)" | Out-Host
                    }
                    Start-Sleep -Milliseconds 300
                }
                $remaining = Get-Process -Name $name -ErrorAction SilentlyContinue
                if ($remaining) {
                    Bad "  cannot stop $name ($($remaining.Count) remaining)."
                    Bad '  No stale executable should be used for a successful build.'
                    exit 2
                }
                Write-Host "  stopped $name via ZBroker"
            }
        }
    }
    Write-Host '  done (silent when nothing was running)'
}
else {
    Write-Host '  skipped by -NoKill; building to a side-by-side directory'
}

# ---------- build ----------
Step "3/5 build ZExplorer ($Configuration|$Platform)"
if (-not (Test-Path -LiteralPath $project)) { Bad "Project not found: $project"; exit 2 }

$outDir = Join-Path $root "x64\$Configuration"
$intDir = Join-Path $root "x64\obj\$Configuration"
if ($NoKill) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $outDir = Join-Path $root "tmp_fast\no-kill-$stamp"
    $intDir = Join-Path $outDir 'obj'
    New-Item -ItemType Directory -Force -Path $outDir, $intDir | Out-Null
}

$msbArgs = @(
    $project,
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:OutDir=$outDir\",
    "/p:IntDir=$intDir\",
    '/m', '/nologo', '/v:minimal'
)
if ($Clean) { $msbArgs = @($project, '/t:Rebuild') + $msbArgs[1..($msbArgs.Count - 1)] }

& $msbuild @msbArgs
if ($LASTEXITCODE -ne 0) { Bad "build failed (exit $LASTEXITCODE)."; exit $LASTEXITCODE }

# ---------- standalone lock laboratory ----------
Step "4/5 build FileLockLab ($Configuration|$Platform)"
$labProject = Join-Path $root 'FileLockLab.vcxproj'
if (-not (Test-Path -LiteralPath $labProject)) {
    Bad "Project not found: $labProject"
    exit 2
}
$labIntDir = Join-Path $intDir 'FileLockLab'
$labArgs = @(
    $labProject,
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:OutDir=$outDir\",
    "/p:IntDir=$labIntDir\",
    '/m', '/nologo', '/v:minimal'
)
& $msbuild @labArgs
if ($LASTEXITCODE -ne 0) {
    Bad "FileLockLab build failed (exit $LASTEXITCODE)."
    exit $LASTEXITCODE
}
$labExe = Join-Path $outDir 'FileLockLab.exe'
if (-not $NoKill -and (Test-Path -LiteralPath $labExe)) {
    Copy-Item -LiteralPath $labExe -Destination (Join-Path $root 'FileLockLab.exe') -Force
    Ok "  root launcher updated: $(Join-Path $root 'FileLockLab.exe')"
}


# ---------- tests ----------
if ($SkipTests) {
    Step '5/5 platform tests skipped (-SkipTests)'
}
else {
    Step '5/5 platform tests (headless: platform/ + fs/ only)'
    $testArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\run_tests.ps1'))
    if ($VsRoot) { $testArgs += @('-VsRoot', $VsRoot) }
    & powershell @testArgs
    if ($LASTEXITCODE -ne 0) { Bad "platform tests failed (exit $LASTEXITCODE)."; exit $LASTEXITCODE }
    Ok '  all assertions passed'
}

$exe = Join-Path $outDir 'ZExplorer.exe'
if (Test-Path -LiteralPath $exe) {
    $kb = [math]::Round((Get-Item -LiteralPath $exe).Length / 1KB, 0)
    Ok "`nOK -> $exe ($kb KB)"
} else {
    Ok "`nBuild succeeded (exe not found at expected path: $exe)"
}
exit 0
