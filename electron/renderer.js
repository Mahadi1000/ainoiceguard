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

/* ── State ───────────────────────────────────────────────────────────────── */

let isRunning = false;
let metricsInterval = null;
let noInputPollCount = 0;

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
  await loadDevices();
  await syncStatus();
  setInterval(syncStatus, 2000);
}

async function loadDevices() {
  try {
    const devices = await bridge.getDevices();

    if (devices.error) {
      showError(devices.error);
      return;
    }

    populateSelect(inputSelect, devices.inputs, "input");
    populateSelect(outputSelect, devices.outputs, "output");
    detectVBCable(devices.outputs);
    hideError();
  } catch (err) {
    showError("Failed to load audio devices: " + err.message);
  }
}

function populateSelect(select, devices, type) {
  select.innerHTML = '<option value="-1">System Default</option>';

  if (type === "output") {
    select.innerHTML += '<option value="-2">No Output (Mute)</option>';
  }

  for (const d of devices) {
    const opt = document.createElement("option");
    opt.value = d.index;
    opt.textContent = d.name;

    if (d.name.toLowerCase().includes("cable")) {
      opt.textContent += " [VB-Cable]";
    }

    select.appendChild(opt);
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

  try {
    if (isRunning) {
      const result = await bridge.stop();
      if (result.success) {
        updateUI(false);
      } else {
        showError(result.error || "Failed to stop");
      }
    } else {
      const inputIdx = parseInt(inputSelect.value, 10);
      const outputIdx = parseInt(outputSelect.value, 10);

      statusText.textContent = "Starting...";
      const result = await bridge.start(inputIdx, outputIdx);

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
  try {
    if (bridge) await bridge.setVadThreshold(threshold);
  } catch (err) {
    /* Non-critical */
  }
});

/* ── Device selection change while running -> restart ────────────────────── */

inputSelect.addEventListener("change", restartIfRunning);
outputSelect.addEventListener("change", restartIfRunning);

async function restartIfRunning() {
  if (!isRunning || !bridge) return;

  try {
    stopMetricsPolling();
    statusText.textContent = "Restarting...";

    await bridge.stop();

    const inputIdx = parseInt(inputSelect.value, 10);
    const outputIdx = parseInt(outputSelect.value, 10);
    const result = await bridge.start(inputIdx, outputIdx);

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

      if (meterHint) {
        const outputIsMute = parseInt(outputSelect.value, 10) === -2;
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
              "Output is muted. For other apps (Discord/Zoom), set their mic to CABLE Input.";
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
  } else if (parseInt(outputSelect.value, 10) === -2) {
    latencyText.textContent = "Muted";
  } else {
    latencyText.textContent = "~12 ms";
  }

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

function detectVBCable(outputDevices) {
  if (vbCableFound) vbCableFound.classList.add("hidden");
  if (vbCableMissing) vbCableMissing.classList.add("hidden");

  const cableDevice = outputDevices.find((d) =>
    d.name.toLowerCase().includes("cable"),
  );

  if (cableDevice) {
    outputSelect.value = String(cableDevice.index);
    if (vbCableFound) vbCableFound.classList.remove("hidden");
    if (vbCableMissing) vbCableMissing.classList.add("hidden");
  } else {
    if (vbCableMissing) vbCableMissing.classList.remove("hidden");
  }
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
