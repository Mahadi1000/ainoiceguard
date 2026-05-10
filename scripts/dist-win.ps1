# dist-win.ps1
#
# Builds the Windows installer via electron-builder, using a temp directory
# for the actual packaging step. This avoids a Windows file-lock bug where
# any tool that has the project directory open (IDEs, file watchers, Claude
# Code, VS Code) holds app.asar and causes electron-builder to fail with:
#   "remove win-unpacked\resources\app.asar: The process cannot access the
#    file because it is being used by another process."
#
# Output:
#   dist/win/Ainoiceguard Setup *.exe   (installer)
#   dist/win/latest.yml                 (auto-update manifest)
#   dist/win/*.exe.blockmap             (block map for delta updates)

$ErrorActionPreference = "Stop"

$ROOT = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$TEMP_OUT = Join-Path $env:TEMP "noiseguard-dist-win"
$FINAL_OUT = Join-Path $ROOT "dist\win"

# Clean temp build dir
if (Test-Path $TEMP_OUT) {
    Remove-Item -Recurse -Force $TEMP_OUT
}
New-Item -ItemType Directory -Force $TEMP_OUT | Out-Null

Write-Host "Building Windows installer to temp dir..." -ForegroundColor Cyan
Write-Host "  Temp: $TEMP_OUT" -ForegroundColor Gray

Set-Location $ROOT
npx electron-builder --win --x64 --publish never "--config.directories.output=$TEMP_OUT"
if ($LASTEXITCODE -ne 0) {
    Write-Host "electron-builder failed." -ForegroundColor Red
    exit 1
}

# Copy installer files into project dist/win.
# We do NOT delete dist/win first because IDEs or file watchers may hold
# app.asar inside win-unpacked from a previous run.  Instead we copy only
# the distributable files (installer, blockmap, manifest) and the unpacked
# resources — skipping any file that is currently locked.
Write-Host ""
Write-Host "Copying installer to dist/win..." -ForegroundColor Cyan

New-Item -ItemType Directory -Force $FINAL_OUT | Out-Null

# Copy top-level distributable files (always unlocked — they live in TEMP)
Get-ChildItem $TEMP_OUT -File | ForEach-Object {
    Copy-Item $_.FullName -Destination $FINAL_OUT -Force
    Write-Host "  Copied: $($_.Name)" -ForegroundColor Gray
}

# Copy win-unpacked so verify-dist.js can check the .node file.
# Use robocopy which skips locked files gracefully (exit 0/1 = OK).
$unpackedSrc = Join-Path $TEMP_OUT "win-unpacked"
$unpackedDst = Join-Path $FINAL_OUT "win-unpacked"
if (Test-Path $unpackedSrc) {
    $rc = robocopy $unpackedSrc $unpackedDst /E /NFL /NDL /NJH /NJS /nc /ns /np 2>&1
    # robocopy exit codes 0-7 are success (8+ are real errors)
    if ($LASTEXITCODE -gt 7) {
        Write-Host "  Warning: robocopy exited $LASTEXITCODE when syncing win-unpacked" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Done! Installer at:" -ForegroundColor Green
Get-ChildItem $FINAL_OUT -Filter "*.exe" | ForEach-Object {
    Write-Host "  $($_.FullName)" -ForegroundColor Green
}
