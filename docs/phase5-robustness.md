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
