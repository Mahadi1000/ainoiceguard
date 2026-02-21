#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# NoiseGuard - Native Build Script (Linux / macOS)
#
# Prerequisites: cmake, build-essential (gcc/g++), node, python3.
# On Debian/Ubuntu: apt-get install build-essential cmake
# On macOS: xcode-select --install, brew install cmake
#
# Usage: ./scripts/build-native-linux.sh   (or run from Docker; see Dockerfile)
# ──────────────────────────────────────────────────────────────────────────────

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "============================================"
echo "  NoiseGuard Native Build (Linux/macOS)"
echo "============================================"
echo ""

# Ensure deps (needed when run in Docker with volume mount)
if [ ! -d node_modules/node-addon-api ]; then
  echo "Installing npm dependencies..."
  npm ci 2>/dev/null || npm install
fi

# Step 1: CMake for PortAudio + RNNoise
echo "[1/3] Building PortAudio + RNNoise via CMake..."
mkdir -p deps/build deps/install
(cd deps/build && cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$ROOT/deps/install" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  "$ROOT/native")
cmake --build deps/build --config Release
cmake --install deps/build --prefix "$ROOT/deps/install"
echo "[1/3] Done!"
echo ""

# Step 2: Verify deps
echo "[2/3] Verifying built dependencies..."
for f in deps/install/include/portaudio.h deps/install/include/rnnoise/rnnoise.h; do
  if [ -f "$f" ]; then echo "  OK: $f"; else echo "  MISSING: $f"; exit 1; fi
done
if ls deps/install/lib/libportaudio*.a deps/install/lib/librnnoise*.a 1>/dev/null 2>&1; then
  echo "  OK: static libs"
else
  echo "  WARNING: no static libs in deps/install/lib"
fi
echo "[2/3] Done!"
echo ""

# Step 3: node-gyp for .node addon
echo "[3/3] Building native addon with node-gyp..."
(cd native && npx node-gyp rebuild --release)
echo "[3/3] Done!"
echo ""

# Copy to project build/ for consistency with Windows
mkdir -p build/Release
if [ -f native/build/Release/ainoiceguard.node ]; then
  cp -f native/build/Release/ainoiceguard.node build/Release/
  echo "  Copied ainoiceguard.node to build/Release/"
fi

echo "============================================"
echo "  Build complete!"
echo "  Run 'npm start' to launch (after npm run rebuild:electron if using Electron)."
echo "============================================"
