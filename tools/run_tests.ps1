# ============================================================================
# tools/run_tests.ps1 -- compile and run the headless platform tests.
#
# ASCII-ONLY ON PURPOSE (see build.ps1 header).
#
# The tests only touch src/platform and src/fs, which by rule do not include
# ZufyUI. They are therefore built WITHOUT any PCH and WITHOUT the UI library,
# which is also a standing proof that the layering rule holds.
#
# Exit code: 0 = all assertions passed.
# ============================================================================
[CmdletBinding()]
param(
    [string]$VsRoot = '',
    [string]$SecondVolumeRoot = '',
    [switch]$KeepBinary
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $env:TEMP 'zexplorer_tests'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ---------- locate the toolchain ----------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { Write-Host "vswhere not found" -ForegroundColor Red; exit 2 }

if ($VsRoot) { $vsPath = $VsRoot }
else {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath) { Write-Host "Visual Studio with C++ tools not found" -ForegroundColor Red; exit 2 }

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { Write-Host "vcvars64.bat not found: $vcvars" -ForegroundColor Red; exit 2 }

# ---------- compile + run ----------
$bat = Join-Path $env:TEMP 'zexplorer_run_tests.bat'
$secondVolumeArg = ''
if ($SecondVolumeRoot) {
    $secondVolumeArg = $SecondVolumeRoot.TrimEnd('\', '/') + '/'
}
$lines = @(
    '@echo off',
    ('call "' + $vcvars + '" >nul 2>&1'),
    ('cd /d "' + $root + '"'),
    ('cl /nologo /std:c++20 /EHsc /W3 /permissive- /utf-8 /D_WINDOWS /DUNICODE /D_UNICODE ' +
     '/I"' + $root + '\src" ' +
     'tests\platform_tests.cpp src\platform\PathUtil.cpp src\platform\ShellActions.cpp src\platform\Shortcuts.cpp ' +
     'src\platform\FileOps.cpp src\platform\FileAttributes.cpp ' +
     'src\platform\NtfsStreams.cpp src\platform\FileLock.cpp ' +
     'src\platform\DirectoryWatcher.cpp ' +
     'src\fs\DirectoryModel.cpp src\fs\ShellIcons.cpp ' +
     '/Fe"' + $outDir + '\platform_tests.exe" ' +
     # NOTE: a trailing backslash before the closing quote escapes it, so the
     # object directory is passed with forward slashes.
     '/Fo"' + ($outDir -replace '\\', '/') + '/" ' +
     '/link /SUBSYSTEM:CONSOLE shlwapi.lib shell32.lib ole32.lib user32.lib rstrtmgr.lib'),
    'if errorlevel 1 exit /b 1',
    ('cd /d "' + $root + '"'),
    ('"' + $outDir + '\platform_tests.exe"' +
        $(if ($secondVolumeArg) { ' "' + $secondVolumeArg + '"' } else { '' }))
)
Set-Content -Path $bat -Value $lines -Encoding ASCII

& cmd /c "`"$bat`" 2>&1"
$code = $LASTEXITCODE

if (-not $KeepBinary) { Remove-Item $bat -Force -ErrorAction SilentlyContinue }
exit $code
