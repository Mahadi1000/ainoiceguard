# ──────────────────────────────────────────────────────────────────────────────
# Ainoiceguard - build DeepFilterNet libDF (C API) and ng_bench with the dfn
# backend. Dev/benchmark only (Phase 1B); the app does not use DFN yet.
#
# Prerequisites: Rust (rustup, x86_64-pc-windows-msvc), Visual Studio Build
# Tools with Desktop C++, CMake 3.20+, git.
#
# Steps:
#   1. Clone DeepFilterNet at tag v0.5.6 into deps/downloads/deepfilternet-src
#   2. Apply native/bench/libdf-v0.5.6-*.patch:
#        cargo-lock  -> time 0.3.36 (0.3.28 does not compile on Rust >= 1.80)
#        capi-thresholds -> CLI default stage thresholds (-15/35/35)
#   3. cargo rustc ... --crate-type staticlib with +crt-static (matches /MT)
#   4. Configure + build deps/bench-build-dfn with -DNG_WITH_DFN=ON
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File ./scripts/build-libdf.ps1
# ──────────────────────────────────────────────────────────────────────────────

$ErrorActionPreference = "Stop"
$ROOT = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$SRC = Join-Path $ROOT "deps\downloads\deepfilternet-src"
$TAG = "v0.5.6"
$COMMIT = "978576aa8400552a4ce9730838c635aa30db5e61"
$TRIPLE = "x86_64-pc-windows-msvc"
$PATCHES = @(
    (Join-Path $ROOT "native\bench\libdf-v0.5.6-cargo-lock.patch"),
    (Join-Path $ROOT "native\bench\libdf-v0.5.6-capi-thresholds.patch")
)

if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    $env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"
}
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: cargo not found. Install Rust from https://rustup.rs" -ForegroundColor Red
    exit 1
}

# ── 1. Source ────────────────────────────────────────────────────────────────
if (-not (Test-Path $SRC)) {
    Write-Host "[1/4] Cloning DeepFilterNet $TAG..." -ForegroundColor Yellow
    git clone --depth 1 --branch $TAG https://github.com/Rikorose/DeepFilterNet.git $SRC
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
$head = (git -C $SRC rev-parse HEAD).Trim()
if ($head -ne $COMMIT) {
    Write-Host "ERROR: $SRC is at $head, expected $COMMIT ($TAG)" -ForegroundColor Red
    exit 1
}
Write-Host "[1/4] Source OK at $TAG ($COMMIT)" -ForegroundColor Green

# ── 2. Patches (idempotent) ─────────────────────────────────────────────────
foreach ($p in $PATCHES) {
    git -C $SRC apply --reverse --check $p 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  already applied: $(Split-Path -Leaf $p)" -ForegroundColor Gray
        continue
    }
    git -C $SRC apply $p
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: failed to apply $p" -ForegroundColor Red; exit 1 }
    Write-Host "  applied: $(Split-Path -Leaf $p)" -ForegroundColor Green
}
Write-Host "[2/4] Patches OK" -ForegroundColor Green

# ── 3. libDF static library ─────────────────────────────────────────────────
Write-Host "[3/4] Building libDF (cargo, release, staticlib, +crt-static)..." -ForegroundColor Yellow
$env:RUSTFLAGS = "-C target-feature=+crt-static"
Push-Location $SRC
# cargo reports progress on stderr; Windows PowerShell 5.1 would turn that into
# terminating errors under "Stop", so rely on the exit code instead.
$ErrorActionPreference = "Continue"
try {
    $out = cargo rustc --locked -p deep_filter --release --lib --features capi `
        --target $TRIPLE --crate-type staticlib -- --print native-static-libs 2>&1 |
        ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { $out | Select-Object -Last 30; exit 1 }
} finally {
    $ErrorActionPreference = "Stop"
    Pop-Location
    Remove-Item Env:\RUSTFLAGS -ErrorAction SilentlyContinue
}
$lib = Join-Path $SRC "target\$TRIPLE\release\df.lib"
if (-not (Test-Path $lib)) { Write-Host "ERROR: $lib not produced" -ForegroundColor Red; exit 1 }

# System libraries reported by rustc. windows.0.48.5.lib lives in the cargo
# registry, not the Windows SDK, so resolve it to a full path.
$line = ($out | Select-String "native-static-libs:" | Select-Object -Last 1).Line
$sys = @()
foreach ($l in ($line -replace '.*native-static-libs:\s*', '').Split(' ', [StringSplitOptions]::RemoveEmptyEntries)) {
    if ($l -like '/defaultlib:*') { continue }          # static CRT already selected via /MT
    if ($l -like 'windows.*.lib') {
        $found = Get-ChildItem "$env:USERPROFILE\.cargo\registry\src" -Recurse -Filter $l -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -like '*windows_x86_64_msvc*' } | Select-Object -First 1
        if (-not $found) { Write-Host "ERROR: $l not found in cargo registry" -ForegroundColor Red; exit 1 }
        $sys += "`"$($found.FullName)`""
    } elseif ($sys -notcontains $l) {
        $sys += $l
    }
}
Write-Host "[3/4] libDF OK: $lib" -ForegroundColor Green

# ── 4. ng_bench with dfn backend ────────────────────────────────────────────
Write-Host "[4/4] Building ng_bench with NG_WITH_DFN=ON..." -ForegroundColor Yellow
$build = Join-Path $ROOT "deps\bench-build-dfn"
cmake -S (Join-Path $ROOT "native") -B $build -A x64 -DNG_BUILD_BENCH=ON -DNG_WITH_DFN=ON `
    "-DNG_DFN_LIB=$lib" "-DNG_DFN_SYSLIBS=$($sys -join ' ')"
if ($LASTEXITCODE -ne 0) { exit 1 }
cmake --build $build --config Release --target ng_bench
if ($LASTEXITCODE -ne 0) { exit 1 }

$model = Join-Path $SRC "models\DeepFilterNet3_onnx.tar.gz"
Write-Host ""
Write-Host "Done. Example:" -ForegroundColor Cyan
Write-Host "  python bench/run_bench.py --bench deps/bench-build-dfn/Release/ng_bench.exe --dfn-model `"$model`" --backends rnnoise-chain dfn --fixtures bench/fixtures/voicebank --priority high" -ForegroundColor Cyan
