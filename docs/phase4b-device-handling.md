# Phase 4b — Reliable device handling

**Date:** 2026-11-06 · **Branch:** `phase4b-device-handling` (from `phase4-scheduling-agc` @ `aa9b831`)
**Plan:** [implementation-plan.md](implementation-plan.md) §3 Phase 4 item 2 · **Previous:** [phase4a-scheduling-agc.md](phase4a-scheduling-agc.md)

## Problems addressed

| # | Problem | Evidence | Fix |
|---|---|---|---|
| L12 | Devices addressed by **PortAudio index** | Starting Krisp shifted index 5 from the mic to "Krisp Speaker" (Phase 4a) | Stable id `"<hostApi>\|<name>"`, resolved at every open |
| L7 | **"System Default" resolved through MME** (global default host API); the list mixed MME and WASAPI duplicates with truncated MME names | Phase 3: MME default ~42 ms vs WASAPI ~33 ms | Default and the list use the preferred host API (WASAPI on Windows) |
| L7 | **WASAPI exclusive tried first**, never obtained; WASAPI options were even attached to MME devices | Phase 3 `inputExclusive: false` | Shared by default (+ `AutoConvert` for non-48 kHz devices); exclusive is opt-in (`settings.exclusive`) |
| — | **A restart reopened the stale index:** PortAudio caches its device list until `Pa_Terminate`, so a replugged device could never be found | code | A restart re-initialises PortAudio (under a mutex) and re-resolves by id, falling back to the default if the device is gone |
| L11 | **Status messages never reached the UI** | Phase 3 | `addon.onStatus` (N-API `ThreadSafeFunction`) → main → renderer |
| — | **No settings persisted**; the VB-Cable auto-select overrode the user's output choice | code | `<userData>/settings.json` (devices by id, level, VAD threshold, exclusive) |
| — | **Latency label hard-coded to "~12 ms"** | live estimate ~32 ms | Live `engine.latency.estimatedTotalMs` |
| — | **Concurrent restarts:** two quick device changes failed with "Engine already running" (found by the E2E test) | E2E | Restarts are serialised; the toggle waits for them |

## Changes

**Native:**
- `device_select.h`: pure resolution rules, unit-tested. In order:
  - an exact id;
  - the same name on another host API is **not** a match;
  - a missing device falls back to the preferred host API's default and reports `fellBack`;
  - otherwise any host default, then the first device with channels.
- `audio.{h,cpp}`:
  - `AudioConfig` carries `DeviceSpec`s, and `tryExclusiveMode` defaults to false.
  - A process-wide mutex serialises `Pa_Initialize`/`Pa_Terminate` and device-list access between the UI thread and the restart path.
  - A restart stops the failed streams immediately (an earlier version counted 1,185 bogus "dropped" samples during the backoff), then re-initialises PortAudio and re-resolves the devices.
  - Status events are sent once per outage (`device-lost`), plus `restarted`, `restart-failed` (first round only) and `fallback`.
- `addon.cc`:
  - `getDevices()` → `{ inputs, outputs, preferredHostApi }`, where each device has `id`, `hostApi`, `preferred` and `isDefault`.
  - `start(input, output, { exclusive })` accepts ids, `""`/`"mute"`, or legacy numbers.
  - New `onStatus(cb)`.
  - `getMetrics().engine.streams.inputId`/`outputId`, read without calling PortAudio from the UI thread.
  - A test hook `_debugForceDeviceLoss()`.
  - An environment cleanup hook stops audio and releases the event queue.

**Electron:**
- `settings.js`: atomic writes, validation, defaults on a missing or corrupt file.
- `device-choice.js`: visible devices, and choosing a selection from a saved id.
- `main.js`: forwards status events, restores level and VAD at start-up, and adds `settings:get`/`settings:set`.
- `preload.js`: `getSettings`, `setSettings`, `onStatus`.
- `renderer.js`: selects by id, saves on change, shows a warning when a saved device is missing (the saved choice is kept), displays status events, shows the live latency, serialises restarts.
- The UI layout is unchanged.

## Verification

**Live engine** (Electron runtime, mic only, output muted; `scratchpad` harness):

| scenario | result |
|---|---|
| Default input | WASAPI "External Microphone (Synaptics HD Audio)", shared, est. **32 ms** (MME was 41.6 ms) |
| By id | same device; 0 drops |
| Stale id ("Unplugged Headset") | `fallback` event; opened the default |
| Forced device loss while running | `device-lost` → `restarted`; same device re-resolved after the PortAudio re-init; callbacks continue; **0 dropped samples**; latency 32 ms |
| Input `"mute"` | rejected: "An input device is required" |

**Real app, end to end** (`tools/e2e-device-handling.js`: real `main.js`, window and renderer, temporary `userData`):

| step | UI / files |
|---|---|
| First launch | list shows only WASAPI devices (no MME duplicates); defaults selected |
| Choose mic, mute, level 80 %, start | "Active"; `settings.json` = mic id, `"mute"`, 0.8 |
| Forced device loss | "Reconnecting…" + message → "Active", message cleared |
| Two output changes while running, then stop | no error (previously "Engine already running"); sessions logged in order |
| Relaunch | mic, mute, level 80 % and VAD restored |
| Saved mic not connected (USB headset) | "Saved microphone “USB Headset (Jabra Evolve 40)” not connected — using System Default"; saved id kept for when it returns |
| `engine-sessions.jsonl` | one line per session: WASAPI, shared, 32 ms, restarts counted, 0 dropped |

**Automated:** 24/24 tests pass, including:
- `ng_engine_tests` device-select cases (Krisp index shift, MME-first lists, cross-host-API names, missing devices);
- settings tests: defaults, a corrupt file, atomic merge, sanitising;
- device-choice tests.

## Not covered / follow-ups

1. **A physical unplug/replug hasn't been tested.** The forced-loss hook exercises the same restart path, but the watchdog's detection of a real removal (a stream stopped by WASAPI, or the 1 s callback stall) still needs a USB headset test. Steps: start, unplug, wait for "Reconnecting…", replug, and expect "Active" on the same device.
2. **There's no automatic switch back** when a preferred device reappears while running on the fallback; the user has to re-select it. Krisp keeps a priority queue for this (report §3).
3. **The device list doesn't refresh on its own** (hot-plug events update it only on restart or the refresh link). Windows `IMMNotificationClient` would make this live.
4. **Drift control** (L8) between the mic and VB-Cable clocks still needs an output device to measure; it isn't installed here.
5. **Exclusive mode has no UI**; it's settings-only (`"exclusive": true` in `settings.json`).
6. The plan's virtual-microphone item (Phase 4c) is next.
