# Ainoiceguard

A real-time noise cancellation desktop app for Windows, Linux, and macOS, built with Electron + a native C++ addon. It captures audio from your microphone, runs it through the [RNNoise](https://github.com/xiph/rnnoise) neural network, and routes the clean output to a virtual cable (e.g. VB-Cable) that other apps can use as a microphone.

> Inspired by Krisp. Fully open-source ([MIT](LICENSE)).

[![CI](https://github.com/Mahadi1000/ainoiceguard/actions/workflows/ci.yml/badge.svg)](https://github.com/Mahadi1000/ainoiceguard/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/Mahadi1000/ainoiceguard)](https://github.com/Mahadi1000/ainoiceguard/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

**[Download the latest release](https://github.com/Mahadi1000/ainoiceguard/releases/latest)** · [Changelog](CHANGELOG.md) · [Contributing](CONTRIBUTING.md) · [Report a bug](https://github.com/Mahadi1000/ainoiceguard/issues/new/choose)

---

## Table of Contents

- [Features](#features)
- [Download](#download)
- [Architecture](#architecture)
- [Prerequisites](#prerequisites)
- [Build](#build)
- [Run](#run)
- [VB-Cable Setup](#vb-cable-setup)
- [Contributing](#contributing)
- [Reporting Issues](#reporting-issues)
- [Roadmap](#roadmap)
- [License](#license)

---

## Features

- Real-time RNNoise-based noise suppression (neural network, 480-sample frames @ 48 kHz)
- Speech-only **auto level (AGC)** and a −1 dBFS **peak limiter**
- **VB-Cable** integration: detects the cable, warns about feedback loops, and shows
  which apps are using the cleaned microphone ("In use by: Discord")
- **Devices can come and go** while running: automatic fallback, return to your
  device when it is plugged back in, and following the Windows default
- **Clock-drift compensation** between microphone and output (no slow build-up of delay)
- Real-time audio thread priority on Windows (MMCSS), event-driven processing,
  zero-allocation audio callbacks, lock-free ring buffers
- Live meters, latency and suppression/voice-gate/AGC controls; settings are remembered
- PortAudio backend (WASAPI on Windows, CoreAudio on macOS, ALSA/PipeWire on Linux)

> Some features are Windows-only for now (MMCSS priority, live device notifications,
> "In use by"). Help porting them is very welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

---

## Download

Get the installer for your system from the
**[latest release](https://github.com/Mahadi1000/ainoiceguard/releases/latest)**:

| OS | File |
|---|---|
| Windows 10/11 | `Ainoiceguard Setup <version>.exe` |
| Linux | `.AppImage`, `.deb` or `.tar.gz` |
| macOS | `.dmg` |

The installers are not code-signed yet: on Windows, SmartScreen may show
"Windows protected your PC" — choose **More info → Run anyway**. Then follow the
[VB-Cable setup](#vb-cable-setup). What changed in each version is in the [changelog](CHANGELOG.md).

---

## Architecture

```
Physical Mic
    │
    ▼
PortAudio Capture (host backend)
    │  (raw float32, 480 samples)
    ▼
SPSC Ring Buffer (lock-free)
    │
    ▼
Processing Thread ──► RNNoise (rnnoise_process_frame)
    │  (denoised float32)
    ▼
Output Ring Buffer
    │
    ▼
PortAudio Output (host backend)
    │
    ├──► VB-Cable Input  ──► Discord / Zoom / Teams (as virtual mic)
    └──► Speaker / Headphones (monitor)
```

**Process boundary:**

```
Electron Renderer (UI)
    │  IPC (contextBridge)
    ▼
Electron Main (main.js)
    │  require()
    ▼
ainoiceguard.node  (N-API addon)
    │
    ├── AudioEngine     (audio.cpp — PortAudio, threads, watchdog, drift control)
    ├── RNNoiseWrapper  (rnnoise_wrapper.cpp — HPF → RNNoise → AGC → gate → limiter)
    ├── DeviceWatcher   (device_watch.cpp — hot-plug + "In use by", Windows)
    └── header-only     (ringbuffer.h, drift.h, dynamics.h, device_policy.h, ...)
```

---

## Prerequisites

| OS                             | Version | Notes                                                                  |
| ------------------------------ | ------- | ---------------------------------------------------------------------- |
| Windows                        | 10 / 11 | Build native addon with `npm run build:native` (PowerShell + VS 2022) |
| Linux                          | Modern distro | Build native addon with `npm run build:native:unix`              |
| macOS                          | 12+     | Build native addon with `npm run build:native:unix` (Xcode CLI tools) |
| Node.js                        | 22 LTS+ | [nodejs.org](https://nodejs.org)                                       |
| npm                            | 10+     | Bundled with Node.js                                                   |
| Python                         | 3.x     | Required by node-gyp                                                   |
| CMake                          | 3.20+   | [cmake.org](https://cmake.org/download/)                               |

> **Windows tip:** install Visual Studio Build Tools with **"Desktop development with C++"** and **"C++ CMake tools for Windows"**.
>
> **Linux tip:** install `build-essential` and ALSA development headers (`libasound2-dev`).
>
> **macOS tip:** install Xcode command line tools (`xcode-select --install`).

---

## Build

### 1. Clone the repository

```bash
git clone https://github.com/Mahadi1000/ainoiceguard.git
cd ainoiceguard
```

### 2. Install Node dependencies

```bash
npm install
```

### 3. Build and package (one command per OS)

These scripts handle everything: build native deps → compile `.node` → rebuild for Electron ABI → package installer.

**Windows** (produces `dist/win/Ainoiceguard Setup *.exe`)
```powershell
npm run dist:full
```

**Linux** (produces `dist/linux/*.AppImage`, `*.deb`, `*.tar.gz`)
```bash
npm run dist:full:unix
```

**macOS** (produces `dist/mac/*.dmg`, `*.zip`)
```bash
npm run dist:full:mac
```

If you only want to build the native addon (without packaging):

```powershell
# Windows
npm run build:native
npm run rebuild:electron   # ← always run this after build:native

# Linux / macOS
npm run build:native:unix
npm run rebuild:electron
```

> **Important:** Always run `npm run rebuild:electron` after building the native addon.
> Skipping it causes a "NODE_MODULE_VERSION mismatch" error when Electron tries to load the addon.

Output folders are separated per OS under `dist/win`, `dist/linux`, and `dist/mac`.
Default `npm run dist` now calls the host-aware wrapper (`dist:all`).

### Docker (Linux build from any host)

The Windows build relies on **CLI tools and paths** (CMake, Visual Studio, vswhere). On Linux or macOS the toolchain is different (gcc, make, ALSA/CoreAudio), so the same script would not work. **Docker** gives you a single, fixed Linux environment so you can build the **Linux** native addon from Windows, Mac, or Linux without installing CMake/gcc on the host.

| Goal | How |
|------|-----|
| **Linux** `.node` from any OS | Use the provided Docker image; run with your project mounted. |
| **Windows** `.node` | Use the host: run `npm run build:native` on Windows (PowerShell + VS + CMake). |
| **macOS** `.node` | Use the host: install Xcode CLI tools + CMake, then run `./scripts/build-native-linux.sh` on a Mac (same script as Linux). |

**Build the Linux addon with Docker:**

```bash
# Build the image (once)
docker build -t noiseguard-build .

# Run the build (project dir = current directory; output appears in ./build and ./deps)
docker run --rm -v "$(pwd):/app" noiseguard-build
```

On **Windows (PowerShell)** use: `docker run --rm -v "${PWD}:/app" noiseguard-build`

Result: `build/Release/ainoiceguard.node` and `deps/install/` for **Linux**. Use the same Node/Electron version when running the app. Docker does **not** produce a Windows or macOS binary; for those, build on the target OS (or use an OS matrix in CI).

---

## Run

```bash
npm start
```

The app starts in the system tray with the window hidden by default. Click the tray icon or use the tray menu to open the window.

> If the tray icon does not appear, check that Electron is finding `build/Release/ainoiceguard.node`. Run `npm run rebuild:electron` if you get a "wrong ABI" error.

---

## VB-Cable Setup

Ainoiceguard outputs processed audio to a virtual cable, which other apps see as a clean microphone.

1. **Download and install** [VB-Cable](https://vb-audio.com/Cable/) (free).
2. **Open Ainoiceguard** from the tray.
3. Set **Input** to your physical microphone.
4. Set **Output** to **"CABLE Input (VB-Audio Virtual Cable)"**.
5. In **Discord / Zoom / Teams**, set the microphone to **"CABLE Output (VB-Audio Virtual Cable)"**.

```
Physical Mic  →  Ainoiceguard  →  CABLE Input  →  CABLE Output  →  Discord/Zoom
```

---


## CI Build & Release

GitHub Actions runs a matrix build on **Ubuntu, macOS, and Windows** using Node.js 22 and `npm ci`.
Each platform builds the native addon, packages OS-specific Electron artifacts, and uploads them as workflow artifacts.

To publish release assets automatically:

1. Add the version's section to [CHANGELOG.md](CHANGELOG.md) and bump `package.json`.
2. Create and push a semantic version tag:
   ```bash
   git tag -a v0.2.0 -m "Ainoiceguard v0.2.0"
   git push origin v0.2.0
   ```
3. The `release` job collects uploaded artifacts from all build jobs.
4. A GitHub Release is created for that tag with attached Windows/Linux/macOS assets
   and that version's changelog section as release notes (`scripts/release-notes.js`).

For non-tag pushes and PRs, artifacts are still available from the workflow run summary.

---

## Contributing

Contributions are welcome — bug fixes, features, platform ports, measurements
and documentation. Please read **[CONTRIBUTING.md](CONTRIBUTING.md)** for the
development setup, coding guidelines and the pull request process, and follow
our [Code of Conduct](CODE_OF_CONDUCT.md).

Short version: fork → branch from `main` → make a focused change with tests →
`npm test` → open a pull request using the template.

---

## Reporting Issues

[Open an issue](https://github.com/Mahadi1000/ainoiceguard/issues/new/choose) and
pick **Bug report** or **Feature request** — the forms ask for the details we need
(version, OS, devices, and for audio problems the `engine-sessions.jsonl` log).
Please search [existing issues](https://github.com/Mahadi1000/ainoiceguard/issues?q=is%3Aissue) first.

**Security vulnerabilities:** do not open a public issue — see [SECURITY.md](SECURITY.md).

---

## Roadmap

Planned improvements (contributions welcome):

- [x] **macOS support** packaging via CoreAudio-capable build flow
- [x] **Linux support** packaging via ALSA / PipeWire-capable build flow
- [x] **AGC / limiter** post-processing stage (v0.2.0)
- [x] **Latency measurement** display in UI (v0.2.0)
- [x] **Installer packaging** (NSIS via electron-builder)
- [x] **Device hot-plug handling** and **call-app detection** on Windows (v0.2.0)
- [ ] **DeepFilterNet** as an optional higher-quality backend (benchmarked in
      [docs/phase1b-results.md](docs/phase1b-results.md); not integrated yet)
- [ ] **Linux / macOS** real-time priority, device notifications and call-app detection
- [ ] **Optional auto-start** when a call app starts using the cable
- [ ] **Noise profiles / presets** (office, street, keyboard, etc.)
- [ ] **Code-signed installers**
- [ ] **Auto-update** via electron-updater

---

## License

[MIT](LICENSE) — free to use, modify, and distribute.

Ainoiceguard includes third-party software (RNNoise, BSD-3-Clause; PortAudio;
Electron) under their own licences — see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
