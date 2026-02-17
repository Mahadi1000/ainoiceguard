# ──────────────────────────────────────────────────────────────────────────────
# NoiseGuard - Native Build Script (Windows / PowerShell)
#
# Prerequisites:
#   - Visual Studio 2022 Build Tools (or full VS) with "Desktop C++" workload
#   - CMake 3.20+ (included with VS or install separately)
#   - Node.js 20+ with npm
#   - Python 3.x (for node-gyp)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File ./scripts/build-native.ps1
#
# What it does:
#   1. Runs CMake to fetch & build PortAudio and RNNoise as static libs
#   2. Installs headers and libs to deps/install/
#   3. Runs node-gyp rebuild to compile the .node addon
# ──────────────────────────────────────────────────────────────────────────────

$ErrorActionPreference = "Stop"

$ROOT = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$DEPS_BUILD = Join-Path $ROOT "deps" "build"
$DEPS_INSTALL = Join-Path $ROOT "deps" "install"

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  NoiseGuard Native Build" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# ── Step 1: Build C dependencies with CMake ──────────────────────────────────
Write-Host "[1/3] Building PortAudio + RNNoise via CMake..." -ForegroundColor Yellow

# Create build and install directories
New-Item -ItemType Directory -Path $DEPS_BUILD -Force | Out-Null
New-Item -ItemType Directory -Path $DEPS_INSTALL -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DEPS_INSTALL "lib") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DEPS_INSTALL "include") -Force | Out-Null

$cmakeSource = Join-Path $ROOT "native"

# Configure
cmake -S $cmakeSource -B $DEPS_BUILD `
  -DCMAKE_BUILD_TYPE=Release `
  -DINSTALL_PREFIX="$DEPS_INSTALL" `
  -G "Visual Studio 17 2022" -A x64

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configure failed!" -ForegroundColor Red
    exit 1
}

# Build
cmake --build $DEPS_BUILD --config Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake build failed!" -ForegroundColor Red
    exit 1
}

# Install (copies libs and headers to deps/install)
cmake --install $DEPS_BUILD --config Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake install failed!" -ForegroundColor Red
    exit 1
}

Write-Host "[1/3] Done!" -ForegroundColor Green

# ── Step 2: Verify dependencies ─────────────────────────────────────────────
Write-Host ""
Write-Host "[2/3] Verifying built dependencies..." -ForegroundColor Yellow

$requiredFiles = @(
    (Join-Path $DEPS_INSTALL "include" "portaudio" "portaudio.h"),
    (Join-Path $DEPS_INSTALL "include" "rnnoise" "rnnoise.h")
)

foreach ($f in $requiredFiles) {
    if (Test-Path $f) {
        Write-Host "  OK: $f" -ForegroundColor Green
    } else {
        Write-Host "  MISSING: $f" -ForegroundColor Red
        Write-Host "Build may have failed. Check CMake output above." -ForegroundColor Red
    }
}

# Check for library files (name may vary)
$libDir = Join-Path $DEPS_INSTALL "lib"
$libs = Get-ChildItem -Path $libDir -Filter "*.lib" -ErrorAction SilentlyContinue
if ($libs) {
    foreach ($lib in $libs) {
        Write-Host "  OK: $($lib.FullName)" -ForegroundColor Green
    }
} else {
    Write-Host "  WARNING: No .lib files found in $libDir" -ForegroundColor Yellow
    Write-Host "  Checking for .a files (MinGW)..." -ForegroundColor Yellow
    $aLibs = Get-ChildItem -Path $libDir -Filter "*.a" -ErrorAction SilentlyContinue
    if ($aLibs) {
        foreach ($lib in $aLibs) {
            Write-Host "  OK: $($lib.FullName)" -ForegroundColor Green
        }
    }
}

Write-Host "[2/3] Done!" -ForegroundColor Green

# ── Step 3: Build Node native addon ─────────────────────────────────────────
Write-Host ""
Write-Host "[3/3] Building native addon with node-gyp..." -ForegroundColor Yellow

Push-Location (Join-Path $ROOT "native")

try {
    npx node-gyp rebuild --release

    if ($LASTEXITCODE -ne 0) {
        Write-Host "node-gyp build failed!" -ForegroundColor Red
        exit 1
    }

    # Copy the built .node file to project root build/ for easy access
    $buildDir = Join-Path $ROOT "build" "Release"
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

    $nodeFile = Join-Path $ROOT "native" "build" "Release" "noiseguard.node"
    if (Test-Path $nodeFile) {
        Copy-Item $nodeFile -Destination $buildDir -Force
        Write-Host "  Copied noiseguard.node to build/Release/" -ForegroundColor Green
    }
} finally {
    Pop-Location
}

Write-Host "[3/3] Done!" -ForegroundColor Green
Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Build complete!" -ForegroundColor Cyan
Write-Host "  Run 'npm start' to launch NoiseGuard." -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
