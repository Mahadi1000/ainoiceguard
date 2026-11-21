# Contributing to Ainoiceguard

Thanks for your interest in improving Ainoiceguard! Bug reports, fixes,
features, platform ports, measurements and documentation are all welcome.

By taking part you agree to follow our [Code of Conduct](CODE_OF_CONDUCT.md).
Contributions are accepted under the project's [MIT licence](LICENSE).

- [Ways to contribute](#ways-to-contribute)
- [Reporting a bug](#reporting-a-bug)
- [Suggesting a feature](#suggesting-a-feature)
- [Development setup](#development-setup)
- [Making a change](#making-a-change)
- [Pull request process](#pull-request-process)
- [Coding guidelines](#coding-guidelines)
- [Audio changes need numbers](#audio-changes-need-numbers)
- [Releases (maintainers)](#releases-maintainers)

## Ways to contribute

- **Report bugs** — especially device-specific ones (USB mics, headsets,
  Bluetooth, virtual cables). Audio bugs depend heavily on hardware, so your
  report may be the only way we can see a problem.
- **Fix an issue.** Issues labelled
  [`good first issue`](https://github.com/Mahadi1000/ainoiceguard/labels/good%20first%20issue)
  are a good starting point; [`help wanted`](https://github.com/Mahadi1000/ainoiceguard/labels/help%20wanted)
  are ones the maintainers would especially like help with.
- **Port platform features.** Several things exist only on Windows today:
  real-time thread priority (MMCSS), live device-change notifications and
  call-app detection. Linux (PipeWire/ALSA) and macOS (CoreAudio)
  equivalents are very welcome — see `docs/phase5-robustness.md`, "Still open".
- **Improve docs** — build guides, troubleshooting, translations of the UI.

## Reporting a bug

1. Search [existing issues](https://github.com/Mahadi1000/ainoiceguard/issues?q=is%3Aissue)
   (open *and* closed) first.
2. Open a new issue with the **Bug report** form and fill in every field you can:
   app version, OS, input/output devices, whether VB-Cable is used, and steps
   to reproduce.
3. If the problem is in the audio (dropouts, crackles, robotic voice, high
   latency, a device not coming back), attach the last lines of the engine
   session log. Each run of the engine adds one line with its counters:
   - Windows: `%APPDATA%\Ainoiceguard\logs\engine-sessions.jsonl`
   - macOS: `~/Library/Logs/Ainoiceguard/engine-sessions.jsonl`
   - Linux: `~/.config/Ainoiceguard/logs/engine-sessions.jsonl`

   It holds device names and counters only, no audio. Please **do not attach
   recordings of other people**.

**Security problems** must not go into public issues — see [SECURITY.md](SECURITY.md).

## Suggesting a feature

Open an issue with the **Feature request** form. Describe the problem first
(what you are trying to do and what gets in the way), then your idea.
For larger changes — a new suppression backend, a new dependency, a change to
the audio thread design — please wait for a maintainer to agree on the approach
before writing a lot of code. It saves everyone time.

## Development setup

You need Node.js 22+, Python 3, CMake 3.20+ and a C++17 compiler:

| OS | Compiler / extras |
|---|---|
| Windows 10/11 | Visual Studio Build Tools with "Desktop development with C++" and "C++ CMake tools for Windows" |
| Linux | `build-essential`, `libasound2-dev`, `pkg-config` |
| macOS 12+ | Xcode command line tools (`xcode-select --install`) |

```bash
git clone https://github.com/<your-username>/ainoiceguard.git
cd ainoiceguard
npm install

# Build the native addon (PortAudio + RNNoise are fetched and built once into deps/)
npm run build:native          # Windows
npm run build:native:unix     # Linux / macOS
npm run rebuild:electron      # always after build:native (Electron ABI)

npm start                     # run the app
npm test                      # JS unit tests (+ native tests if built, see below)
```

**Native tests and the golden regression test** (recommended for any change
under `native/`):

```bash
cmake -S native -B deps/bench-build -DCMAKE_BUILD_TYPE=Release -DNG_BUILD_BENCH=ON
cmake --build deps/bench-build --config Release --target ng_bench ng_engine_tests
npm test                      # now also runs test/golden.test.js and test/native-unit.test.js
```

The benchmark tools (`ng_bench`, `ng_loopback`, the Python scorers) are
described in [bench/README.md](bench/README.md). The design notes and
measurements behind the current engine are in [docs/](docs/).

## Making a change

1. **Fork** the repository and create a branch from `main`:
   `feat/short-description`, `fix/short-description` or `docs/short-description`.
2. Keep the change focused: one logical change per pull request.
3. Add or update tests:
   - pure logic (device choice, settings, policies, DSP blocks) → unit tests in
     `test/*.test.js` or `native/tests/engine_tests.cpp`;
   - output of the processing chain changes on purpose → regenerate the golden
     files with `bench/make_golden.py` and explain why in the PR.
4. Update the docs if behaviour, settings or build steps change
   (README, `docs/`, and `CHANGELOG.md` under **Unreleased**).
5. Write commit messages in [Conventional Commits](https://www.conventionalcommits.org/) form:

   | Prefix | Use for |
   |---|---|
   | `feat:` | a new feature |
   | `fix:` | a bug fix |
   | `perf:` | a performance improvement |
   | `refactor:` | restructuring without behaviour change |
   | `docs:` | documentation only |
   | `test:` | tests only |
   | `chore:` / `ci:` | build scripts, CI, tooling |

   An optional scope helps: `fix(engine): ...`, `feat(ui): ...`, `feat(devices): ...`.

## Pull request process

1. Make sure `npm test` passes and the app still builds and starts.
2. Open the pull request against `main` and fill in the template: what changed,
   why, how you tested it (which devices / OS), and any measurements.
3. Link the issue it solves with `Closes #123`.
4. CI runs the unit tests, the golden regression test and Windows/Linux/macOS
   builds. A PR is merged only when CI is green.
5. A maintainer reviews it — usually within a week. Expect questions; audio
   code gets a careful review because mistakes are audible. Push follow-up
   commits to the same branch; the maintainer squash-merges at the end.
6. If a PR is waiting on the author for 30 days without a reply, it may be
   closed. It can always be reopened.

## Coding guidelines

**C++ (`native/`)**
- C++17, 2-space indent, `PascalCase` classes, `camelCase` functions,
  `member_` with a trailing underscore for data members; keep the existing
  style of the file you edit.
- **The audio callbacks and the processing thread must not block:** no heap
  allocation, no locks, no file or console I/O, no waiting on the UI. Pass
  data through the lock-free ring buffers and atomics (`EngineStats`), and
  wake threads with `WakeEvent`.
- Platform code goes behind `#ifdef _WIN32` / `__APPLE__` / `__linux__` with a
  working (stub) path for the other platforms, so every OS keeps building.
- Pure logic goes in headers that can be unit-tested without audio devices
  (see `device_policy.h`, `drift.h`, `dynamics.h`).

**JavaScript (`electron/`)**
- 2-space indent, double quotes, semicolons.
- The renderer has no Node.js access: add new native calls to the addon,
  expose them in `main.js` through `ipcMain.handle`, then in `preload.js`.
- Put logic that can be tested without a window into small modules
  (`device-choice.js`, `settings.js`, `session-stats.js`) and test them.
- No new runtime dependencies without discussing it in an issue first.

## Audio changes need numbers

Changes to suppression, gain, timing or buffering should come with before/after
measurements, because "sounds better to me" is hard to review. Use the
existing tools where they fit:

- quality: `bench/run_bench.py` + `bench/compare.py` (SI-SDR, STOI, DNSMOS);
- loudness/AGC: `bench/agc_check.py`;
- latency and drift through a virtual cable: `ng_loopback`;
- live behaviour: the counters in `engine-sessions.jsonl` (xruns, drops,
  underruns, restarts).

The `docs/phase*.md` files show the kind of tables we use.

## Releases (maintainers)

1. Move the **Unreleased** entries in `CHANGELOG.md` under a new version
   heading and bump `version` in `package.json` (and the version label in
   `electron/index.html`).
2. Commit `chore(release): vX.Y.Z`, then tag and push:
   ```bash
   git tag -a vX.Y.Z -m "Ainoiceguard vX.Y.Z"
   git push origin main vX.Y.Z
   ```
3. CI builds Windows, Linux and macOS packages and publishes a GitHub Release
   for the tag, with that version's `CHANGELOG.md` section as the release notes.
