# ──────────────────────────────────────────────────────────────────────────────
# Ainoiceguard - Native Build Script (Windows / PowerShell)
#
# Prerequisites:
#   - Visual Studio 2022 Build Tools (or full VS) with "Desktop C++" workload
#   - CMake 3.20+ (included with VS or install separately)
#   - Node.js 20+ with npm
#   - Python 3.x (for node-gyp)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File ./scripts/build-native.ps1
# ──────────────────────────────────────────────────────────────────────────────

$ErrorActionPreference = "Stop"

$ROOT = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$DEPS_BUILD = Join-Path (Join-Path $ROOT "deps") "build"
$DEPS_INSTALL = Join-Path (Join-Path $ROOT "deps") "install"

# Find CMake (PATH, then Visual Studio, then standalone install)
$CMAKE_CMD = $null
if (Get-Command cmake -ErrorAction SilentlyContinue) {
    $CMAKE_CMD = "cmake"
} else {
    $cmakeCandidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\CMake\bin\cmake.exe"
    )
    foreach ($c in $cmakeCandidates) {
        if (Test-Path $c) {
            $CMAKE_CMD = $c
            Write-Host "Using CMake: $CMAKE_CMD" -ForegroundColor Gray
            break
        }
    }
}

if (-not $CMAKE_CMD) {
    Write-Host "ERROR: CMake not found." -ForegroundColor Red
    Write-Host "Install Visual Studio 2022 with Desktop C++ workload or CMake from https://cmake.org/download/." -ForegroundColor Yellow
    exit 1
}

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Ainoiceguard Native Build" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# ── Step 1: Build C dependencies with CMake ──────────────────────────────────
Write-Host "[1/3] Building PortAudio + RNNoise via CMake..." -ForegroundColor Yellow

if (Test-Path $DEPS_BUILD) {
    Write-Host "Removing stale CMake build directory: $DEPS_BUILD" -ForegroundColor Gray
    Remove-Item -Recurse -Force $DEPS_BUILD
}
New-Item -ItemType Directory -Path $DEPS_BUILD -Force | Out-Null
New-Item -ItemType Directory -Path $DEPS_INSTALL -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DEPS_INSTALL "lib") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DEPS_INSTALL "include") -Force | Out-Null

$cmakeSource = Join-Path $ROOT "native"
& $CMAKE_CMD -S $cmakeSource -B $DEPS_BUILD -G "Visual Studio 17 2022" -A x64 -DCMAKE_INSTALL_PREFIX=$DEPS_INSTALL
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configure failed. Install Visual Studio 2022 Build Tools + Desktop C++ workload." -ForegroundColor Red
    exit 1
}

& $CMAKE_CMD --build $DEPS_BUILD --config Release
if ($LASTEXITCODE -ne 0) { Write-Host "CMake build failed!" -ForegroundColor Red; exit 1 }

& $CMAKE_CMD --install $DEPS_BUILD --config Release
if ($LASTEXITCODE -ne 0) { Write-Host "CMake install failed!" -ForegroundColor Red; exit 1 }

Write-Host "[1/3] Done!" -ForegroundColor Green

# ── Step 2: Verify dependencies ─────────────────────────────────────────────
Write-Host ""
Write-Host "[2/3] Verifying built dependencies..." -ForegroundColor Yellow

$requiredFiles = @(
    (Join-Path (Join-Path $DEPS_INSTALL "include") "portaudio.h"),
    (Join-Path (Join-Path (Join-Path $DEPS_INSTALL "include") "rnnoise") "rnnoise.h")
)

foreach ($f in $requiredFiles) {
    if (Test-Path $f) {
        Write-Host "  OK: $f" -ForegroundColor Green
    } else {
        Write-Host "  MISSING: $f" -ForegroundColor Red
        exit 1
    }
}

Write-Host "[2/3] Done!" -ForegroundColor Green

# ── Step 3: Build Node native addon ─────────────────────────────────────────
Write-Host ""
Write-Host "[3/3] Building native addon with node-gyp..." -ForegroundColor Yellow

$nativeDir = Join-Path $ROOT "native"
Push-Location $nativeDir
try {
    npx node-gyp rebuild --release --msvs_version=2022
    if ($LASTEXITCODE -ne 0) {
        Write-Host "node-gyp build failed!" -ForegroundColor Red
        Write-Host "Ensure Visual Studio 2022 Build Tools with Desktop C++ workload is installed." -ForegroundColor Yellow
        exit 1
    }
} finally {
    Pop-Location
}

$buildDir = Join-Path (Join-Path $ROOT "build") "Release"
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

$nodeFile = Join-Path (Join-Path (Join-Path (Join-Path $ROOT "native") "build") "Release") "ainoiceguard.node"
if (Test-Path $nodeFile) {
    Copy-Item $nodeFile -Destination $buildDir -Force
    Write-Host "  Copied ainoiceguard.node to build/Release/" -ForegroundColor Green
}

Write-Host "[3/3] Done!" -ForegroundColor Green
Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Build complete!" -ForegroundColor Cyan
Write-Host "  Run 'npm start' to launch Ainoiceguard." -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
