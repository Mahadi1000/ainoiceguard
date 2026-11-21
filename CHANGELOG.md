# Changelog

All notable changes to Ainoiceguard are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.2.0] - 2026-11-21

A big reliability and quality release: steadier audio with no dropouts,
automatic level control, devices that can come and go while the app runs, and a
much better VB-Cable experience. Details and measurements are in [`docs/`](https://github.com/Mahadi1000/ainoiceguard/tree/main/docs).

### Added
- **Auto level (AGC)** keeps your voice at a steady level (target −18 dBFS),
  with an on/off switch and the live gain in the window. It only reacts to
  speech, so background noise is not pumped up.
- **Peak limiter** at −1 dBFS so loud sounds never clip in the call.
- **Devices plugged in or out while running** (Windows): the device lists
  refresh by themselves, the app falls back to another microphone in ~0.2 s
  when yours disappears, returns to it when it comes back, and follows the
  Windows default when "System Default" is selected.
- **"In use by: Discord, Zoom"** in the VB-Cable banner (Windows): shows which
  apps are recording from CABLE Output, so you can see that the call app is
  really using the cleaned microphone.
- **Live latency** in the status strip.
- **Settings are remembered**: microphone, output, suppression level, voice
  gate and AGC survive a restart. Devices are stored by a stable id, so they
  survive other devices being plugged in.
- **Clock-drift compensation** between the microphone and the output device
  (adaptive resampling), so long calls do not slowly build up delay or crackle.
- **New app icon** for the installer, the app, the window and the tray (green
  while noise cancellation is on, grey when off).
- Warning when the selected microphone is the cable's own output (feedback loop).
- MIT licence, third-party notices, contributing guide, code of conduct,
  security policy, and issue / pull-request templates.
- Developer tools: offline benchmark (`ng_bench`, SI-SDR / STOI / DNSMOS
  scoring), VB-Cable latency/drift meter (`ng_loopback`), native unit tests,
  and a golden-output regression test in CI.

### Changed
- **Real-time audio thread priority** on Windows (MMCSS "Pro Audio"):
  dropped samples under load went from 5,505 to 0 in testing.
- Processing thread is woken by an event instead of sleeping and polling.
- Windows uses WASAPI (shared mode) by default: ~32 ms vs ~42 ms with MME.
- VB-Cable is detected by both of its ends, and the banner notes that the
  cable adds its own ~100 ms of buffering on top of the latency shown.
- The device watchdog waits up to 3 s for a slow microphone's first audio
  before treating it as lost (some USB cameras need more than 1 s).

### Fixed
- An audible tick at the start of every session (an output underrun in the
  first ~20 ms): the output now waits for a 20 ms cushion before playing.
- Devices being mixed up when another device was plugged in (selection by
  index replaced by stable ids).
- Restarts after a lost device counting bogus dropouts.
- Two restarts racing each other in the window ("Engine already running").
- The app executable used Electron's default icon.

### Security
- Removed malicious code that had been injected into `scripts/dist-all.js` and
  `scripts/verify-dist.js` on GitHub. Release builds are made only from
  reviewed commits.

## [0.1.3] - 2026-05-11

### Added
- VB-Cable onboarding in the app (detection banner, install link, refresh) and
  an optional NSIS installer hook for bundling VB-Cable.

## [0.1.2] - 2026-05-11

### Changed
- Overhauled the noise-cancellation chain for better voice quality
  (high-pass filter, wet/dry blend, voice gate, comfort noise).

## [0.1.1] - 2026-05-11

### Fixed
- The Windows installer now works on clean machines without a developer toolchain.
- CI: ES-module test syntax and Linux build dependencies.

[Unreleased]: https://github.com/Mahadi1000/ainoiceguard/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/Mahadi1000/ainoiceguard/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/Mahadi1000/ainoiceguard/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/Mahadi1000/ainoiceguard/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/Mahadi1000/ainoiceguard/releases/tag/v0.1.1
