# ──────────────────────────────────────────────────────────────────────────────
# NoiseGuard – Linux native addon build (Docker)
#
# Provides a fixed Linux build environment (CMake, gcc, Node). Run the
# container with your project mounted to get a Linux .node without
# installing tools on the host.
#
# Build image:
#   docker build -t noiseguard-build .
#
# Run build (output appears in your project dir):
#   Windows (PowerShell): docker run --rm -v "${PWD}:/app" noiseguard-build
#   Linux/macOS:         docker run --rm -v "$(pwd):/app" noiseguard-build
#
# Result: build/Release/ainoiceguard.node and deps/install/ for Linux.
# Use the same Node/Electron version when running the app.
# ──────────────────────────────────────────────────────────────────────────────

FROM node:20-bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    python3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy project (mount will override at run time)
COPY . .

# Install npm deps so node-gyp has node-addon-api
RUN npm ci 2>/dev/null || npm install

# Run Linux build when container starts (expects project mounted at /app)
ENTRYPOINT ["/bin/bash", "/app/scripts/build-native-linux.sh"]
