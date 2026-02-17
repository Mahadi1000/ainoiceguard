# NoiseGuard

Real-time noise cancellation desktop app inspired by [Krisp](https://krisp.ai).
Built with Electron, PortAudio (WASAPI), and RNNoise.

## Architecture

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │                          Electron (UI)                             │
 │  ┌──────────┐  IPC   ┌──────────┐  require()  ┌────────────────┐  │
 │  │renderer.js├───────►│  main.js  ├────────────►│ noiseguard.node│  │
 │  │ (tray UI) │◄───────┤  (Node)   │◄────────────┤  (N-API addon) │  │
 │  └──────────┘        └──────────┘              └───────┬────────┘  │
 └────────────────────────────────────────────────────────┼────────────┘
                                                          │
 ┌────────────────────── Native C++ Layer ────────────────┼────────────┐
 │                                                        │            │
 │  ┌─────────────────────────────────────────────────────▼──────────┐ │
 │  │                       AudioEngine                              │ │
 │  │                                                                │ │
 │  │  [Mic] ──► CaptureCallback ──► captureRing_ (SPSC lock-free)  │ │
 │  │                                       │                        │ │
 │  │                              ProcessingThread                  │ │
 │  │                                       │                        │ │
 │  │                                  RNNoise (480 samples/frame)   │ │
 │  │                                       │                        │ │
 │  │  [Out] ◄── OutputCallback  ◄── outputRing_ (SPSC lock-free)   │ │
 │  └────────────────────────────────────────────────────────────────┘ │
 │                                                                     │
 │  PortAudio ──► WASAPI (exclusive mode) ──► Hardware / VB-Cable      │
 └─────────────────────────────────────────────────────────────────────┘
```

## Audio Pipeline

```
 Physical Mic ──► WASAPI Capture ──► Ring Buffer ──► RNNoise (10ms frames)
                                                          │
                                                          ▼
 Speaker / VB-Cable ◄── WASAPI Playback ◄── Ring Buffer ◄─┘
```

**Latency budget:**
- Capture buffer: ~10ms (480 samples @ 48kHz)
- RNNoise processing: ~1-2ms per frame
- Output buffer: ~10ms
- **Total: ~12ms end-to-end** (WASAPI exclusive mode)

## Prerequisites

### Required Software

| Tool | Version | Purpose |
|------|---------|---------|
| **Node.js** | 20+ | Runtime & npm |
| **Python** | 3.x | Required by node-gyp |
| **Visual Studio 2022 Build Tools** | 17.0+ | MSVC compiler |
| **CMake** | 3.20+ | Build PortAudio & RNNoise |
| **Git** | Any | Clone dependencies |

### Install Visual Studio Build Tools

1. Download [VS Build Tools 2022](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
2. Install the **"Desktop development with C++"** workload
3. Ensure these components are selected:
   - MSVC v143 C++ build tools
   - Windows 10/11 SDK
   - C++ CMake tools for Windows

### Install Node.js

Download from [nodejs.org](https://nodejs.org) (LTS recommended).
During installation, check **"Automatically install the necessary tools"** to get Python and build tools.

## Quick Start

```powershell
# 1. Clone the project
git clone <your-repo-url> noiseguard
cd noiseguard

# 2. Install Node dependencies
npm install

# 3. Build native dependencies (PortAudio + RNNoise + addon)
npm run build:native

# 4. Rebuild for Electron ABI
npm run rebuild:electron

# 5. Launch the app
npm start
```

## Build Steps (Detailed)

### Step 1: Install npm packages

```powershell
npm install
```

### Step 2: Build native addon

The build script fetches PortAudio and RNNoise source code, compiles them as
static libraries, then builds the N-API addon.

```powershell
npm run build:native
# or manually:
powershell -ExecutionPolicy Bypass -File ./scripts/build-native.ps1
```

This will:
1. Download PortAudio v19.7.0 and RNNoise via CMake FetchContent
2. Build both as static `.lib` files in `deps/install/lib/`
3. Copy headers to `deps/install/include/`
4. Run `node-gyp rebuild` to compile `noiseguard.node`
5. Copy the addon to `build/Release/`

### Step 3: Rebuild for Electron

Since Electron has a different ABI than system Node.js:

```powershell
npm run rebuild:electron
```

### Step 4: Run

```powershell
npm start
```

## Testing: Mic to Speaker

1. Launch the app (`npm start`)
2. Select your physical microphone as **Input Device**
3. Select your speakers/headphones as **Output Device**
4. Click the **ON** button
5. Speak into your mic -- you should hear your voice with noise removed

## VB-Cable Virtual Microphone Routing

To use NoiseGuard with apps like Discord, Zoom, or Teams:

### Setup

1. **Install VB-Cable** from [vb-audio.com/Cable](https://vb-audio.com/Cable/)
   - Download and run `VBCABLE_Setup_x64.exe` as Administrator
   - Reboot after installation

2. **Configure NoiseGuard:**
   - Input: Your physical microphone
   - Output: **CABLE Input (VB-Audio Virtual Cable)**

3. **Configure your voice app (Discord, Zoom, etc.):**
   - Input/Microphone: **CABLE Output (VB-Audio Virtual Cable)**

### Signal Flow

```
 Your Mic ──► NoiseGuard (RNNoise) ──► VB-Cable Input
                                            │
                                            ▼
 Discord/Zoom ◄─── reads from ◄─── VB-Cable Output
```

Now Discord/Zoom receives your clean, noise-suppressed audio.

## Project Structure

```
noiseguard/
├── electron/
│   ├── main.js           # Electron main process, loads addon, IPC
│   ├── preload.js        # contextBridge for secure IPC
│   ├── renderer.js       # UI logic (vanilla JS)
│   ├── tray.js           # System tray icon & menu
│   ├── index.html        # App HTML
│   └── styles.css        # Dark theme CSS
├── native/
│   ├── CMakeLists.txt    # Build PortAudio + RNNoise deps
│   ├── binding.gyp       # node-gyp config for .node addon
│   └── src/
│       ├── addon.cc      # N-API entry point
│       ├── audio.cpp/h   # PortAudio engine (WASAPI)
│       ├── rnnoise_wrapper.cpp/h  # RNNoise DSP wrapper
│       └── ringbuffer.h  # Lock-free SPSC ring buffer
├── scripts/
│   └── build-native.ps1  # Windows build script
├── deps/                  # Built libs (gitignored)
├── build/                 # Compiled addon (gitignored)
├── package.json
└── README.md
```

## npm Scripts

| Script | Command | Description |
|--------|---------|-------------|
| `npm start` | `electron .` | Launch NoiseGuard |
| `npm run dev` | `electron .` | Development launch |
| `npm run build:native` | PowerShell build script | Build all native deps + addon |
| `npm run rebuild:electron` | `electron-rebuild` | Rebuild addon for Electron ABI |

## Real-Time Audio Design Rules

These rules are enforced throughout the native codebase:

1. **No allocations in audio callbacks.** All buffers are pre-allocated at startup.
2. **No locks in the audio path.** The ring buffers use lock-free atomics only.
3. **No syscalls in callbacks.** No file I/O, no logging, no `new`/`delete`.
4. **Fixed-size frames.** RNNoise processes exactly 480 samples (10ms). No variable-length buffers.
5. **Graceful degradation.** If the ring buffer is full, samples are dropped rather than blocking.
6. **Separate streams.** Capture and output use independent PortAudio streams for robustness.
7. **Auto-restart.** Device disconnection triggers automatic reconnection with exponential backoff.

## Troubleshooting

### "Failed to load native addon"
- Run `npm run build:native` first
- Ensure Visual Studio Build Tools are installed
- Check that `build/Release/noiseguard.node` exists

### "No input device available"
- Check Windows Sound settings -- ensure mic is enabled
- Try unplugging and re-plugging USB microphone
- The app auto-detects devices on startup

### WASAPI exclusive mode fails
- Another app may be using the device exclusively
- Close other audio apps (DAW, OBS) and retry
- The engine automatically falls back to shared mode

### High latency or glitches
- Close CPU-heavy background apps
- Try WASAPI exclusive mode (default)
- Reduce buffer size if your hardware supports it

## TODO / Roadmap

- [ ] **DeepFilterNet integration** -- Alternative/complementary neural noise suppressor
- [ ] **macOS support** -- CoreAudio backend for PortAudio
- [ ] **Linux support** -- PulseAudio/PipeWire backend
- [ ] **AGC (Automatic Gain Control)** -- Normalize mic level after suppression
- [ ] **Noise gate** -- Hard cut below threshold
- [ ] **Preset profiles** -- Office, Outdoors, Music modes
- [ ] **Real latency measurement** -- Measure actual round-trip with timestamps
- [ ] **Audio level meters** -- Input/output VU meters in UI
- [ ] **Virtual audio driver** -- Eliminate VB-Cable dependency on Windows
- [ ] **GPU inference** -- ONNX Runtime for faster model execution
- [ ] **Installer** -- NSIS/electron-builder packaged installer
- [ ] **Auto-update** -- Electron auto-updater integration
- [ ] **Per-app routing** -- Process audio for specific apps only (requires driver)
- [ ] **Echo cancellation** -- AEC for speaker+mic setups
- [ ] **Model hot-swap** -- Switch between RNNoise / DeepFilterNet at runtime

## License

MIT

## Acknowledgments

- [RNNoise](https://github.com/xiph/rnnoise) by Jean-Marc Valin (Xiph.Org)
- [PortAudio](http://www.portaudio.com/) by Ross Bencina et al.
- [Electron](https://www.electronjs.org/)
- [VB-Cable](https://vb-audio.com/Cable/) by VB-Audio Software
