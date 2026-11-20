# Phase 5 — Robustness polish

**Date:** 2026-11-20 · **Branch:** `phase5-robustness` (from `phase4c-vbcable` @ `7b95240`)
**Previous:** [phase4c-vbcable.md](phase4c-vbcable.md)

## 5.1 Output priming (start-up underrun)

**Problem.** Every live run in Phase 4c had one output underrun at start: an audible tick in the first ~20 ms. The output callback started consuming on the **first** processed frame, so there was no cushion. If the next frame arrived after the device's next callback, the device got a partly empty block. The start-up level, and so the latency, also depended on that first race.

**Fix** (`native/src/audio.cpp`, `outputCallback`):
- Play silence until the output ring holds a cushion equal to the drift target (960 samples, 20 ms), then start consuming.
- The same applies after automatic restarts (the watchdog resets "primed").
- The cushion is set in `resetDrift()`, so it always follows the drift target.

**A variant that was rejected:** trimming the ring to *exactly* the cushion when playback starts, to make every session start at the same level. It brought start-up underruns back (3/10 with control off), and the level still varied. The remaining spread comes from the phase between the mic's and the cable's 10 ms callbacks, not from the excess, and drift control evens it out over the first minute.

**Measured** (mic → CABLE Input, silent; each start runs 3.5 s):

| | starts with an underrun | underrun samples | level at 3.5 s | latency spread |
|---|---:|---:|---:|---:|
| before, drift on | 3/10 | 2,400 | 443–662 | 65.5–73.1 ms |
| before, drift off | **9/10** | 6,240 | 374–622 | 64–72.4 ms |
| after, drift on | **0/10 + 0/20** | 0 | 499–1008 | 65.7–81.9 ms |
| after, drift off | **0/10 + 0/20** | 0 | 456–954 | 64.8–78.5 ms |

Five minutes with the defaults (drift on, target 960), after the fix:

| t | applied correction | level | underrun events | drops / xruns / overruns | est. latency |
|---|---:|---:|---:|---|---:|
| 30 s | +108.2 ppm | 1032 | 0 | 0 / 0 / 0 | 79.4 ms |
| 60 s | −2.7 ppm | 960 | 0 | 0 / 0 / 0 | 79.8 ms |
| 90 s | −0.2 ppm | 957 | 0 | 0 / 0 / 0 | 79.8 ms |
| 120 s | +35.7 ppm | 970 | 0 | 0 / 0 / 0 | 79.8 ms |
| 150 s | +31.1 ppm | 972 | 0 | 0 / 0 / 0 | 79.8 ms |
| 180 s | +3.5 ppm | 961 | 0 | 0 / 0 / 0 | 79.8 ms |
| 210 s | −7.0 ppm | 958 | 0 | 0 / 0 / 0 | 79.8 ms |
| 240 s | −14.1 ppm | 955 | 0 | 0 / 0 / 0 | 79.7 ms |
| 270 s | −20.8 ppm | 954 | 0 | 0 / 0 / 0 | 79.6 ms |
| 300 s | −1.6 ppm | 961 | 0 | 0 / 0 / 0 | 79.6 ms |

After the first 60 s the correction averaged **+3.3 ppm (sd 18.9)**, which matches the ~0–6 ppm measured mic-vs-cable drift, and the level stayed at **954–972** against the 960 target.

- **The start-up underrun is gone:** 0 in 60 starts and in the 5-minute run, where every earlier run had 1.
- **The level converges** to the 960 target within ~60 s.
- **Cost:** playback starts ~20 ms later (the cushion fills first). Steady-state latency stays at the drift target set in Phase 4c (~78–80 ms estimated on this machine).

## Still open (Phase 5 candidates)

1. **Live device-list refresh and return to a preferred device:** implemented in §5.2. The unplug path is verified live; **the replug switch-back and follow-default still need a live test.**
2. ~~A physical unplug/replug test of the watchdog~~: the unplug part was done with a USB camera mic (§5.2).
3. **An AGC on/off toggle in the UI**, plus the pending silent-room AGC check.
4. **MMCSS-equivalent priority on Linux/macOS.**
5. **A VB-Cable latency note in the UI and docs:** the cable adds ~100 ms on this machine (Phase 4c), its largest single contributor.

## 5.2 Device changes while running (in progress)

**Goal:** react to devices being plugged in or out while the app runs:
- **return to the selected device** when it comes back after a fallback;
- **follow the Windows default** when "System Default" is selected;
- **refresh the device lists**, which PortAudio freezes while streams are open.

**Design:**
- `device_watch.{h,cpp}`: a Windows `IMMNotificationClient` on its own COM (MTA) thread. Bursts of notifications are debounced (300 ms of quiet) into one change. A `snapshot()` call lists active endpoints and the defaults by the same friendly names PortAudio uses.
- `device_policy.h` (pure, unit-tested) decides between keeping the current device, `ReturnToPreferred` and `FollowDefault`. The same name on another host API is not a match.
- The engine watchdog checks the watcher's change count every 250 ms. A switch reuses the restart path (PortAudio re-init plus resolve by id), counts in `recovery.reselects`, and emits a `switched` event, not `device-lost`.
- The addon emits `devices-changed` to JS. While running, `getDevices()` takes the WASAPI entries from the live snapshot, which was verified identical to PortAudio's names and default flags.
- Renderer: refreshes the lists on `devices-changed` and `switched`. While a saved device is missing, the engine is still started with the **saved id**, so it falls back by itself and can return later. An explicit choice in the dropdown clears that.

**Found and fixed during the live test:** a USB camera mic took more than 1 s to deliver its first audio and was flagged as lost 1.5 s after start, causing a needless restart. The stall watchdog now allows 3 s for the first audio after a (re)start, and keeps 1 s once audio flows.

**Verified live** (USB camera mic "Microphone (USB Camera Audio)", output muted):

| scenario | result |
|---|---|
| unplug while running on the camera mic | `device-lost` + `devices-changed` at 65.8 s; **fell back to the built-in mic in 0.2 s** (`fallback`, `restarted`); 0 dropped samples |
| start with the chosen camera mic unplugged | immediate `fallback` to the built-in mic, no false loss |
| **replug → switch back** | **not yet observed:** the camera did not re-appear in Windows during either test window |
| follow a changed Windows default | not yet tested (needs the default changed in Sound settings during a run) |

## 5.3 AGC switch in the UI, VB-Cable latency note

- **Auto level (AGC) checkbox** in the Suppression card, with the live AGC gain next to it ("+8 dB", or "off"). Persisted as `agcEnabled` in `settings.json` (default on) and applied at start-up by `main.js`; `audio:set-agc` IPC → `addon.setAgcEnabled`.
- **The VB-Cable banner** now notes that the latency shown is Ainoiceguard's own, and that VB-Cable adds its own buffering (~100 ms measured on the test PC, Phase 4c).

**Verified in the real app** (temporary settings folder, output muted):

| step | result |
|---|---|
| start with AGC on | readout "+2 dB" (the AGC's starting gain; it held there over this short run in a quiet room) |
| untick while running | engine AGC off at once (`getAgcEnabled() = false`), readout "off", `settings.json` → `agcEnabled: false` |
| relaunch | checkbox unticked, engine AGC off from start-up |

The pending **silent-room AGC check** (Phase 4a, open point 1) still needs a longer run in a confirmed-silent room. With the new readout you can watch `+N dB` directly in the app.

## 5.4 Which apps use the cable (call detection)

**Goal** (Krisp-like): show whether a call app is actually using the cleaned mic. A common setup mistake is starting Ainoiceguard while Zoom or Discord still records the raw mic, and nothing in the app showed that.

**Native** (`device_watch.cpp`, `DeviceWatcher::captureClients`, Windows only):
- Find the recording endpoint by friendly name: exact match first, else a prefix match (MME cuts names to 31 characters).
- `IMMDevice::Activate(IAudioSessionManager2)` → session enumerator → for each session, `IAudioSessionControl2` gives the pid and the state (Active, Inactive, Expired).
- Skipped: our own process, the system-sounds session and expired sessions.
- The process name comes from `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `QueryFullProcessImageNameW`; it stays empty if the process is not readable (for example, elevated).
- Other platforms: stub (returns not supported).

**Addon:** `getCaptureClients(idOrName)` → `[{ pid, process, active }]`, or `null` if the endpoint was not found or the platform is unsupported. A `host|` id prefix is stripped, so the renderer passes `cable.micId` directly. **Main:** IPC `audio:capture-clients`.

**UI:** a line in the VB-Cable banner, refreshed every 2 s while the window is visible (`describeCaptureClients` in `device-choice.js`, unit-tested):

| sessions on CABLE Output | banner line |
|---|---|
| none | "No app is using CABLE Output (VB-Audio Virtual Cable) yet." |
| at least one active | **"In use by: Discord, Zoom"** (bold; one entry per app; ".exe" dropped) |
| only inactive (app open, not recording) | "Open in: obs64 (not recording right now)" |
| not supported / not found | line hidden |

**Verified live** with `ng_loopback` recording from CABLE Output as the "call app":

| check | result |
|---|---|
| addon call while ng_loopback runs | `[{ pid: 14396, process: "ng_loopback.exe", active: true }]`, **10 ms** per call |
| after ng_loopback exits | `[]` (its session is gone) |
| unknown endpoint name | `null` |
| real app, banner before / during / after | "No app is using … yet." → **"In use by: ng_loopback"** → "No app is using … yet." |

**Not done (possible next step):** automatically starting suppression when a call app starts recording from the cable, as Krisp does. That would be opt-in (default off), because starting the engine opens the user's microphone.
