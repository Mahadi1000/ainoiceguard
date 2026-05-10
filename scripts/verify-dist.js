#!/usr/bin/env node
/**
 * Verifies that a dist directory contains a valid installer and unpacked
 * native addon. Run after dist:win/dist:linux/dist:mac.
 *
 * Usage: node scripts/verify-dist.js <dist-dir>
 *   e.g. node scripts/verify-dist.js dist/win
 */

const fs = require("fs");
const path = require("path");

const distDir = process.argv[2];
if (!distDir) {
  console.error("Usage: node scripts/verify-dist.js <dist-dir>");
  process.exit(1);
}

const abs = (p) => path.resolve(distDir, p);

let failed = false;

function check(filePath, description) {
  const full = abs(filePath);
  if (fs.existsSync(full)) {
    console.log(`  OK  ${description}`);
  } else {
    console.error(`  MISSING  ${description}`);
    console.error(`           Expected: ${full}`);
    failed = true;
  }
}

function checkGlob(dir, ext, description) {
  const full = abs(dir);
  if (!fs.existsSync(full)) {
    console.error(`  MISSING  ${description} (dir not found: ${full})`);
    failed = true;
    return;
  }
  const match = fs.readdirSync(full).find((f) => f.endsWith(ext));
  if (match) {
    console.log(`  OK  ${description}: ${match}`);
  } else {
    console.error(`  MISSING  ${description} (*${ext} not found in ${full})`);
    failed = true;
  }
}

console.log(`\nVerifying dist: ${path.resolve(distDir)}\n`);

const platform = (() => {
  if (distDir.includes("win")) return "win";
  if (distDir.includes("linux")) return "linux";
  if (distDir.includes("mac")) return "mac";
  return "unknown";
})();

if (platform === "win") {
  checkGlob(".", ".exe", "NSIS installer");
  check("win-unpacked/Ainoiceguard.exe", "Main executable");
  check(
    "win-unpacked/resources/app.asar",
    "Electron app bundle (asar)"
  );
  check(
    "win-unpacked/resources/app.asar.unpacked/build/Release/ainoiceguard.node",
    "Native addon (.node)"
  );
  checkGlob(".", "latest.yml", "electron-updater manifest");
} else if (platform === "linux") {
  checkGlob(".", ".AppImage", "AppImage");
  checkGlob(".", ".deb", "Debian package");
} else if (platform === "mac") {
  checkGlob(".", ".dmg", "macOS disk image");
} else {
  checkGlob(".", ".exe", "Installer (unknown platform)");
}

if (failed) {
  console.error("\nVerification FAILED — dist is incomplete.\n");
  process.exit(1);
} else {
  console.log("\nVerification passed.\n");
}
