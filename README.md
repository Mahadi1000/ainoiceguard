# Ainoiceguard

A real-time noise cancellation desktop app for Windows, Linux, and macOS, built with Electron + a native C++ addon. It captures audio from your microphone, runs it through the [RNNoise](https://github.com/xiph/rnnoise) neural network, and routes the clean output to a virtual cable (e.g. VB-Cable) that other apps can use as a microphone.

> Inspired by Krisp. Fully open-source.

---

## Table of Contents

- [Features](#features)
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
- PortAudio backend (WASAPI on Windows, CoreAudio on macOS, ALSA/PipeWire on Linux)
- Lock-free SPSC ring buffer between capture and processing threads
- System tray UI — device selector, suppression slider, on/off toggle
- Auto-restart on device disconnect with exponential backoff
- Zero-allocation audio callbacks

---

## Architecture

```
Physical Mic
    │
    ▼
PortAudio WASAPI Capture
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
PortAudio WASAPI Output
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
    ├── AudioEngine  (audio.cpp  — PortAudio + threading)
    ├── RNNoiseWrapper (rnnoise_wrapper.cpp)
    └── RingBuffer<float> (ringbuffer.h — header-only)
```

---

## Prerequisites

| OS                             | Version | Notes                                                                  |
| ------------------------------ | ------- | ---------------------------------------------------------------------- |
| Windows                        | 10 / 11 | Build native addon with `npm run build:native` (PowerShell + VS 2022) |
| Linux                          | Modern distro | Build native addon with `npm run build:native:unix`              |
| macOS                          | 12+     | Build native addon with `npm run build:native:unix` (Xcode CLI tools) |
| Node.js                        | 20 LTS+ | [nodejs.org](https://nodejs.org)                                       |
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

### 3. Build the native addon (host OS)

This step fetches PortAudio and RNNoise via CMake, compiles them as static libs, then compiles the `.node` addon with node-gyp.

**Windows**
```powershell
npm run build:native
```

**Linux / macOS**
```bash
npm run build:native:unix
```

### 4. Rebuild for Electron ABI (required after native build)

```bash
npm run rebuild:electron
```

### 5. Build distributables by OS (separate output folders)

```bash
# Windows -> dist/win
npm run dist:win

# Linux -> dist/linux
npm run dist:linux

# macOS -> dist/mac
npm run dist:mac

# Host-aware wrapper (runs only compatible target on your current OS)
npm run dist:all
```

Convenience scripts:

```bash
# Build native + package for Windows
npm run dist:full

# Build native + package for Linux
npm run dist:full:unix

# Build native + package for macOS
npm run dist:full:mac
```

Output folders are separated per OS under `dist/win`, `dist/linux`, and `dist/mac`.

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

## Contributing

Contributions are welcome — bug fixes, new features, platform ports, documentation improvements. Here is how to get started:

### Getting started

1. **Fork** the repository on GitHub.
2. **Clone** your fork locally:
   ```bash
   git clone https://github.com/<your-username>/ainoiceguard.git
   cd ainoiceguard
   ```
3. **Create a branch** for your change. Use a descriptive name:

   ```bash
   # For a feature
   git checkout -b feat/deepfilter-integration

   # For a bug fix
   git checkout -b fix/wasapi-exclusive-mode-crash

   # For documentation
   git checkout -b docs/macos-build-guide
   ```

4. **Make your changes**, then commit using a clear message:
   ```bash
   git add .
   git commit -m "feat: add DeepFilterNet as optional DSP backend"
   ```
5. **Push** to your fork:
   ```bash
   git push origin feat/deepfilter-integration
   ```
6. **Open a Pull Request** against the `main` branch (see below).

### Commit message style

Use the [Conventional Commits](https://www.conventionalcommits.org/) format:

| Prefix      | When to use                          |
| ----------- | ------------------------------------ |
| `feat:`     | New feature                          |
| `fix:`      | Bug fix                              |
| `perf:`     | Performance improvement              |
| `refactor:` | Code restructure, no behavior change |
| `docs:`     | Documentation only                   |
| `test:`     | Tests                                |
| `chore:`    | Build scripts, CI, tooling           |

### Code style

- **C++**: Follow the existing style (4-space indent, `snake_case` for locals, `PascalCase` for classes). Keep audio callbacks allocation-free.
- **JavaScript**: 2-space indent, single quotes, no semicolons are fine if consistent with surrounding code.
- Do not add new runtime dependencies without discussion in an issue first.

### Pull Request guidelines

When you open a PR:

- Fill in the PR template (title, what changed, why, how to test).
- Link the related issue with `Closes #<issue-number>` in the description if applicable.
- Keep PRs focused — one logical change per PR. Large PRs are hard to review.
- Make sure the project still builds (`npm run build:native && npm run rebuild:electron && npm start` should work).
- Describe any new configuration, environment variable, or dependency you added.

PRs are reviewed on a best-effort basis. Feedback will be left as review comments.

---

## Reporting Issues

Use [GitHub Issues](https://github.com/Mahadi1000/ainoiceguard/issues) to report bugs or request features.

### Bug reports

Please include:

- **OS version** (e.g. Windows 11 23H2)
- **Node.js version** (`node --version`)
- **Steps to reproduce** — what you did, what you expected, what actually happened
- **Error output** — paste the full terminal output or Electron DevTools console log
- **Audio setup** — input device, output device, sample rate if known

### Feature requests

Open an issue with the `enhancement` label. Describe:

- The problem you are trying to solve
- Your proposed solution or idea
- Any alternatives you considered

### Before opening an issue

- Search [existing issues](https://github.com/Mahadi1000/ainoiceguard/issues) to avoid duplicates.
- If you found a security vulnerability, please **do not** open a public issue. Email the maintainer directly.

---

## Roadmap

Planned improvements (contributions welcome):

- [ ] **DeepFilterNet** as an optional higher-quality DSP backend
- [x] **macOS support** packaging via CoreAudio-capable build flow
- [x] **Linux support** packaging via ALSA / PipeWire-capable build flow
- [ ] **AGC / limiter** post-processing stage
- [ ] **Noise profiles / presets** (office, street, keyboard, etc.)
- [ ] **Latency measurement** display in UI
- [ ] **GPU-accelerated inference** (ONNX Runtime or TFLite)
- [ ] **Installer packaging** (NSIS / WiX via electron-builder)
- [ ] **Auto-update** via electron-updater

---

## License

[MIT](LICENSE) — free to use, modify, and distribute.
