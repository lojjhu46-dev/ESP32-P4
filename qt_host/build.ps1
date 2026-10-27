# qt_host one-click build (Qt 6.12 mingw_64 + MinGW 13.1 + CMake + Ninja)
# Usage: powershell -ExecutionPolicy Bypass -File qt_host\build.ps1 [-Clean] [-Run]
# NOTE: keep this file ASCII-only -- Windows PowerShell 5.1 reads .ps1 as ANSI,
#       UTF-8 Chinese text would break parsing (documented in HANDOVER).
param(
    [switch]$Clean,
    [switch]$Run,
    [switch]$PcTest
)

$ErrorActionPreference = 'Stop'

$QtRoot   = 'D:\qt\qt\Qt\6.12.0\mingw_64'
$QtTools  = 'D:\qt\qt\Qt\Tools'
$MingwBin = Join-Path $QtTools 'mingw1310_64\bin'
$NinjaDir = Join-Path $QtTools 'Ninja'
$CMake    = 'C:\Program Files\CMake\bin\cmake.exe'

$SrcDir   = $PSScriptRoot
# PcTest: 独立 build-pctest（匹配 .gitignore 的 build-*/），生产 build 不动
$BuildDir = if ($PcTest) { Join-Path $SrcDir 'build-pctest' } else { Join-Path $SrcDir 'build' }

foreach ($p in @($QtRoot, $MingwBin, $NinjaDir, $CMake)) {
    if (-not (Test-Path $p)) { Write-Error "missing path: $p" }
}

# Make cmake find the compiler and Qt (this process only)
$env:PATH = "$MingwBin;$NinjaDir;$QtRoot\bin;$env:PATH"

if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }

if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    Write-Host '=== configure ==='
    $CfgArgs = @('-S', $SrcDir, '-B', $BuildDir, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_PREFIX_PATH=$QtRoot",
        "-DCMAKE_C_COMPILER=$MingwBin\gcc.exe",
        "-DCMAKE_CXX_COMPILER=$MingwBin\g++.exe")
    if ($PcTest) { $CfgArgs += '-DQT_HOST_PC_TEST=ON' }
    & $CMake @CfgArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host '=== build ==='
& $CMake --build $BuildDir -j 8
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Deploy Qt runtime next to the exe (so it can run without PATH)
# NOTE: do NOT pass --no-compiler-runtime -- the app needs MinGW runtime
#       (libgcc_s_seh-1 / libstdc++-6 / libwinpthread-1), otherwise it dies
#       with 0xC0000135 (STATUS_DLL_NOT_FOUND) at startup.
$Deploy = Join-Path $QtRoot 'bin\windeployqt.exe'
$Exe = Join-Path $BuildDir 'qt_host.exe'
if ((Test-Path $Deploy) -and (Test-Path $Exe)) {
    Write-Host '=== windeployqt ==='
    $Env:PATH = "$QtRoot\bin;$env:PATH"
    & $Deploy --no-translations --no-system-d3d-compiler --no-opengl-sw $Exe | Out-Null
}

# Safety net: make sure the MinGW runtime DLLs sit next to the exe
foreach ($dll in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    $src = Join-Path $MingwBin $dll
    $dst = Join-Path $BuildDir $dll
    if ((Test-Path $src) -and -not (Test-Path $dst)) {
        Copy-Item $src $dst -Force
        Write-Host "copied $dll"
    }
}

Write-Host ''
Write-Host "BUILD OK: $Exe"
Write-Host "RUN:      $Exe"

if ($Run) { & $Exe }
