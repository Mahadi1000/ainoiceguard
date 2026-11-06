/**
 * Ainoiceguard - Renderer Process (Vanilla JS)
 *
 * Handles UI interaction and communicates with main process via the
 * preload-exposed `window.ainoiceguard` bridge.
 */

/* ── DOM References ──────────────────────────────────────────────────────── */

const toggleBtn = document.getElementById("toggleBtn");
const toggleHint = document.getElementById("toggleHint");
const statusDot = document.getElementById("statusDot");
const inputSelect = document.getElementById("inputSelect");
const outputSelect = document.getElementById("outputSelect");
const levelSlider = document.getElementById("levelSlider");
const levelValue = document.getElementById("levelValue");
const vadThreshSlider = document.getElementById("vadThreshSlider");
const vadThreshValue = document.getElementById("vadThreshValue");
const statusText = document.getElementById("statusText");
const latencyText = document.getElementById("latencyText");
const framesText = document.getElementById("framesText");
const gateText = document.getElementById("gateText");
const errorBar = document.getElementById("errorBar");

const inputMeter = document.getElementById("inputMeter");
const outputMeter = document.getElementById("outputMeter");
const inputDb = document.getElementById("inputDb");
const outputDb = document.getElementById("outputDb");
const vadBar = document.getElementById("vadBar");
const vadValue = document.getElementById("vadValue");
const meterSection = document.getElementById("meterSection");
const meterHint = document.getElementById("meterHint");

const setupGuide = document.getElementById("setupGuide");
const vbCableFound = document.getElementById("vbCableFound");
const vbCableMissing = document.getElementById("vbCableMissing");
const vbCableLink = document.getElementById("vbCableLink");
const vbCableRefresh = document.getElementById("vbCableRefresh");
const vbCableMicName = document.getElementById("vbCableMicName");

/* ── State ───────────────────────────────────────────────────────────────── */

let isRunning = false;
let metricsInterval = null;
let noInputPollCount = 0;
let settings = null; /* persisted settings (see electron/settings.js) */
let cable = null;    /* detected virtual cable (see device-choice.js findVirtualCable) */

const MUTE = "mute"; /* output value: no output */
const choice = window.deviceChoice;

/* ── Utility Functions ───────────────────────────────────────────────────── */

function rmsToPercent(rms) {
  if (rms <= 0.001) return 0;
  const db = 20 * Math.log10(rms);
  return Math.max(0, Math.min(100, ((db + 60) / 60) * 100));
}

function rmsToDb(rms) {
  if (rms <= 0.0001) return "-\u221E";
  const db = 20 * Math.log10(rms);
  return db.toFixed(0) + "dB";
}

function formatFrameCount(n) {
  if (n >= 1000000) return (n / 1000000).toFixed(1) + "M";
  if (n >= 1000) return (n / 1000).toFixed(1) + "K";
  return String(n);
}

/* ── Bridge Check ────────────────────────────────────────────────────────── */

const bridge = window.ainoiceguard;
if (!bridge) {
  document.addEventListener("DOMContentLoaded", () => {
    showError(
      "Native bridge failed to load. Restart the app. " +
        "If the problem persists, run: npm run build:native && npm start",
    );
  });
}

/* ── Initialize ──────────────────────────────────────────────────────────── */

async function init() {
  if (!bridge) return;
  try {
    settings = await bridge.getSettings();
  } catch (err) {
    settings = null;
  }
  applySavedSliders();
  if (bridge.onStatus) bridge.onStatus(handleEngineStatus);
  await loadDevices();
  await syncStatus();
  setInterval(syncStatus, 2000);
}

/** Restore saved suppression level / VAD threshold into the sliders. */
function applySavedSliders() {
  if (!settings) return;
  const level = Math.round(settings.level * 100);
  levelSlider.value = level;
  levelValue.textContent = level + "%";
  const vad = Math.round(settings.vadThreshold * 100);
  vadThreshSlider.value = vad;
  vadThreshValue.textContent = vad + "%";
}

function saveSettings(patch) {
  if (!bridge || !bridge.setSettings) return;
  bridge.setSettings(patch).then((s) => { settings = s; }).catch(() => {});
}

async function loadDevices() {
  try {
    const devices = await bridge.getDevices();

    if (devices.error) {
      showError(devices.error);
      return;
    }

    const inputs = choice.visibleDevices(devices.inputs);
    const outputs = choice.visibleDevices(devices.outputs);
    populateSelect(inputSelect, inputs, "input");
    populateSelect(outputSelect, outputs, "output");

    /* Restore saved devices by id; on first run suggest VB-Cable as output. */
    const cableId = detectVBCable(inputs, outputs);
    const inSel = choice.chooseSelection(inputs.map((d) => d.id), settings ? settings.inputId : "", null);
    const outSel = choice.chooseSelection(outputs.map((d) => d.id), settings ? settings.outputId : null, cableId);
    inputSelect.value = inSel.value;
    outputSelect.value = outSel.value;

    const missing = [];
    if (inSel.missing) missing.push("microphone “" + choice.nameFromId(settings.inputId) + "”");
    if (outSel.missing) missing.push("output “" + choice.nameFromId(settings.outputId) + "”");
    if (missing.length) {
      showError("Saved " + missing.join(" and ") + " not connected — using System Default.");
    } else {
      hideError();
    }
  } catch (err) {
    showError("Failed to load audio devices: " + err.message);
  }
}

function populateSelect(select, devices, type) {
  select.innerHTML = '<option value="">System Default</option>';

  if (type === "output") {
    select.innerHTML += '<option value="' + MUTE + '">No Output (Mute)</option>';
  }

  for (const d of devices) {
    const opt = document.createElement("option");
    opt.value = d.id; /* stable id; PortAudio indices change with devices */
    opt.textContent = d.name;

    if (d.name.toLowerCase().includes("cable")) {
      opt.textContent += " [VB-Cable]";
    }

    select.appendChild(opt);
  }
}

/**
 * Recording from the cable's recording end while writing into its playback
 * end loops the app's own output back into its input (runaway echo).
 * Shows an error and returns true if that combination is selected.
 */
function feedbackLoopSelected() {
  if (!choice.isFeedbackLoop(inputSelect.value, outputSelect.value, cable)) return false;
  showError(
    "“" + cable.micName + "” is for other apps. Choose your real microphone here " +
      "and keep “" + cable.outputName + "” as Output.",
  );
  statusText.textContent = "Idle";
  return true;
}

/** Engine status events from the audio thread (via main process). */
function handleEngineStatus(ev) {
  if (!ev || !ev.type) return;
  if (ev.type === "device-lost") {
    statusText.textContent = "Reconnecting...";
    showError(ev.message);
  } else if (ev.type === "restarted") {
    statusText.textContent = "Active";
    hideError();
  } else if (ev.type === "restart-failed") {
    statusText.textContent = "Device lost";
    showError(ev.message);
  } else if (ev.type === "fallback") {
    showError(ev.message);
  }
}

async function syncStatus() {
  try {
    if (!bridge) return;
    const status = await bridge.getStatus();
    updateUI(status.running, status.level);
  } catch (err) {
    /* Silently ignore polling errors. */
  }
}

/* ── Toggle Noise Cancellation ───────────────────────────────────────────── */

toggleBtn.addEventListener("click", async () => {
  if (!bridge) {
    showError("Native bridge not available. Restart the app.");
    return;
  }

  toggleBtn.disabled = true;

  /* Let any device-change restart finish first (see restartIfRunning). */
  await restartQueue.catch(() => {});

  try {
    if (isRunning) {
      const result = await bridge.stop();
      if (result.success) {
        updateUI(false);
      } else {
        showError(result.error || "Failed to stop");
      }
    } else {
      if (feedbackLoopSelected()) return;
      statusText.textContent = "Starting...";
      const result = await bridge.start(inputSelect.value, outputSelect.value);

      if (result.success) {
        updateUI(true);
        hideError();
      } else {
        showError(result.error || "Failed to start");
        statusText.textContent = "Error";
      }
    }
  } catch (err) {
    showError(err.message);
  } finally {
    toggleBtn.disabled = false;
  }
});

/* ── Suppression Level Slider ────────────────────────────────────────────── */

levelSlider.addEventListener("input", () => {
  const pct = parseInt(levelSlider.value, 10);
  levelValue.textContent = pct + "%";
});

levelSlider.addEventListener("change", async () => {
  const level = parseInt(levelSlider.value, 10) / 100.0;
  saveSettings({ level });
  try {
    if (bridge) await bridge.setLevel(level);
  } catch (err) {
    /* Non-critical */
  }
});

/* ── VAD Gate Threshold Slider ───────────────────────────────────────────── */

vadThreshSlider.addEventListener("input", () => {
  const pct = parseInt(vadThreshSlider.value, 10);
  vadThreshValue.textContent = pct + "%";
});

vadThreshSlider.addEventListener("change", async () => {
  const threshold = parseInt(vadThreshSlider.value, 10) / 100.0;
  saveSettings({ vadThreshold: threshold });
  try {
    if (bridge) await bridge.setVadThreshold(threshold);
  } catch (err) {
    /* Non-critical */
  }
});

/* ── Device selection change while running -> restart ────────────────────── */

inputSelect.addEventListener("change", () => {
  saveSettings({ inputId: inputSelect.value });
  restartIfRunning();
});
outputSelect.addEventListener("change", () => {
  saveSettings({ outputId: outputSelect.value });
  restartIfRunning();
});

/*
 * Device changes while running restart the engine. Restarts are serialised:
 * overlapping change events (e.g. arrow-keying through a select) used to run
 * stop/start pairs concurrently and fail with "Engine already running".
 * Each queued restart uses the selection current at the time it runs.
 */
let restartQueue = Promise.resolve();

function restartIfRunning() {
  restartQueue = restartQueue.then(doRestart, doRestart);
  return restartQueue;
}

async function doRestart() {
  if (!isRunning || !bridge) return;

  try {
    stopMetricsPolling();
    statusText.textContent = "Restarting...";

    await bridge.stop();

    if (feedbackLoopSelected()) {
      updateUI(false);
      return;
    }
    const result = await bridge.start(inputSelect.value, outputSelect.value);

    if (result.success) {
      updateUI(true);
      hideError();
    } else {
      showError(result.error || "Restart failed");
      updateUI(false);
    }
  } catch (err) {
    showError("Restart error: " + err.message);
    updateUI(false);
  }
}

/* ── Metrics Polling ─────────────────────────────────────────────────────── */

function startMetricsPolling() {
  if (metricsInterval || !bridge) return;

  noInputPollCount = 0;
  metricsInterval = setInterval(async () => {
    try {
      const m = await bridge.getMetrics();

      if (!isRunning) return;

      const inputRms = Number(m.inputRms) || 0;
      const outputRms = Number(m.outputRms) || 0;
      const vadProb = Number(m.vadProbability) || 0;
      const gateGain = Number(m.gateGain) || 0;
      const framesProcessed = Number(m.framesProcessed) || 0;

      const inPct = rmsToPercent(inputRms);
      const outPct = rmsToPercent(outputRms);

      inputMeter.style.width = inPct + "%";
      outputMeter.style.width = outPct + "%";
      inputDb.textContent = rmsToDb(inputRms);
      outputDb.textContent = rmsToDb(outputRms);

      const vadPct = Math.min(100, Math.max(0, Math.round(vadProb * 100)));
      vadBar.style.width = vadPct + "%";
      vadValue.textContent = vadPct + "%";

      framesText.textContent = formatFrameCount(framesProcessed);
      gateText.textContent = Number.isFinite(gateGain)
        ? (gateGain * 100).toFixed(0) + "%"
        : "--";

      /* Live end-to-end latency estimate (device + rings + RNNoise frame). */
      const lat = m.engine && m.engine.latency ? Number(m.engine.latency.estimatedTotalMs) : NaN;
      if (outputSelect.value === MUTE) {
        latencyText.textContent = "Muted";
      } else if (Number.isFinite(lat) && lat > 0) {
        latencyText.textContent = "~" + Math.round(lat) + " ms";
      }

      if (meterHint) {
        const outputIsMute = outputSelect.value === MUTE;
        if (inputRms <= 0.001) {
          noInputPollCount++;
          if (noInputPollCount >= 20) {
            meterHint.textContent =
              "No input signal \u2014 check microphone selection.";
            meterHint.classList.remove("hidden");
          } else if (outputIsMute) {
            meterHint.textContent =
              "Output is muted. Select Speakers or CABLE to hear audio.";
            meterHint.classList.remove("hidden");
          }
        } else {
          noInputPollCount = 0;
          if (outputIsMute) {
            meterHint.textContent =
              "Output is muted. Select " + (cable ? cable.outputName : "CABLE Input") +
              " as Output, then choose " + (cable ? cable.micName : "CABLE Output") + " in Discord/Zoom.";
            meterHint.classList.remove("hidden");
          } else {
            meterHint.textContent = "";
            meterHint.classList.add("hidden");
          }
        }
      }
    } catch (err) {
      /* Ignore polling errors */
    }
  }, 100);
}

function stopMetricsPolling() {
  if (metricsInterval) {
    clearInterval(metricsInterval);
    metricsInterval = null;
  }
  noInputPollCount = 0;
  if (meterHint) {
    meterHint.textContent = "";
    meterHint.classList.add("hidden");
  }
  inputMeter.style.width = "0%";
  outputMeter.style.width = "0%";
  vadBar.style.width = "0%";
  inputDb.textContent = "-\u221E";
  outputDb.textContent = "-\u221E";
  vadValue.textContent = "0%";
  framesText.textContent = "0";
  gateText.textContent = "--";
}

/* ── UI Update Helpers ───────────────────────────────────────────────────── */

function updateUI(running, level) {
  isRunning = running;

  toggleBtn.classList.toggle("on", running);
  toggleHint.textContent = running ? "Click to disable" : "Click to enable";
  statusDot.classList.toggle("active", running);
  statusText.textContent = running ? "Active" : "Idle";

  if (!running) {
    latencyText.textContent = "-- ms";
  } else if (outputSelect.value === MUTE) {
    latencyText.textContent = "Muted";
  }
  /* While running, the live estimate is filled in by the metrics poll. */

  if (level !== undefined) {
    const pct = Math.round(level * 100);
    levelSlider.value = pct;
    levelValue.textContent = pct + "%";
  }

  if (running) {
    startMetricsPolling();
  } else {
    stopMetricsPolling();
  }
}

function showError(msg) {
  if (!errorBar) return;
  errorBar.textContent = msg;
  errorBar.classList.remove("hidden");
}

function hideError() {
  if (!errorBar) return;
  errorBar.classList.add("hidden");
  errorBar.textContent = "";
}

/* ── VB-Cable Detection & Auto-Select ────────────────────────────────────── */

function detectVBCable(inputDevices, outputDevices) {
  if (vbCableFound) vbCableFound.classList.add("hidden");
  if (vbCableMissing) vbCableMissing.classList.add("hidden");

  /* Both ends must exist: the app writes to the playback end ("CABLE Input"),
     other apps record from the recording end ("CABLE Output"). */
  cable = choice.findVirtualCable(inputDevices, outputDevices);
  const cableDevice = cable ? { id: cable.outputId } : null;
  if (cable && vbCableMicName) vbCableMicName.textContent = cable.micName;

  if (cableDevice) {
    if (vbCableFound) vbCableFound.classList.remove("hidden");
    if (vbCableMissing) vbCableMissing.classList.add("hidden");
  } else {
    if (vbCableMissing) vbCableMissing.classList.remove("hidden");
  }
  /* Only a suggestion: loadDevices() applies it if no output was ever saved. */
  return cableDevice ? cableDevice.id : null;
}

/* Open VB-Audio download page. */
if (vbCableLink) {
  vbCableLink.addEventListener("click", (e) => {
    e.preventDefault();
    if (bridge && bridge.openExternal) {
      bridge.openExternal("https://vb-audio.com/Cable/");
    }
  });
}

/* Re-check for VB-Cable after user reports having installed it.
   This also handles the post-install reboot scenario. */
if (vbCableRefresh) {
  vbCableRefresh.addEventListener("click", async (e) => {
    e.preventDefault();
    /* Force a device re-enumeration by calling loadDevices().
       PortAudio re-scans hardware on each call, so newly installed
       devices will appear without a full app restart. */
    await loadDevices();
  });
}

/* ── Boot ────────────────────────────────────────────────────────────────── */

init();
