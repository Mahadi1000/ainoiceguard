/**
 * Ainoiceguard - Renderer Process (Vanilla JS)
 *
 * Handles UI interaction and communicates with main process via the
 * preload-exposed `window.ainoiceguard` bridge.
 *
 * Features:
 *   - Device selection and power toggle
 *   - Live level meters (input RMS, output RMS, VAD)
 *   - VAD gate threshold control
 *   - VB-Cable auto-detect
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

/* Meters */
const inputMeter = document.getElementById("inputMeter");
const outputMeter = document.getElementById("outputMeter");
const inputDb = document.getElementById("inputDb");
const outputDb = document.getElementById("outputDb");
const vadBar = document.getElementById("vadBar");
const vadValue = document.getElementById("vadValue");
const meterSection = document.getElementById("meterSection");
const meterHint = document.getElementById("meterHint");

/* VB-Cable setup guide */
const setupGuide = document.getElementById("setupGuide");
const vbCableFound = document.getElementById("vbCableFound");
const vbCableMissing = document.getElementById("vbCableMissing");
const vbCableLink = document.getElementById("vbCableLink");

/* ── State ───────────────────────────────────────────────────────────────── */

let isRunning = false;
let metricsInterval = null;
let noInputPollCount = 0;

/* ── Initialize ──────────────────────────────────────────────────────────── */

async function init() {
  await loadDevices();
  await syncStatus();

  /* Poll status every 2 seconds for external state changes. */
  setInterval(syncStatus, 2000);
}

/** Load available audio devices into the dropdown selects. */
async function loadDevices() {
  try {
    const devices = await window.ainoiceguard.getDevices();

    if (devices.error) {
      showError(devices.error);
      return;
    }

    populateSelect(inputSelect, devices.inputs, "input");
    populateSelect(outputSelect, devices.outputs, "output");

    /* Auto-detect VB-Cable and show setup guide. */
    detectVBCable(devices.outputs);

    hideError();
  } catch (err) {
    showError("Failed to load audio devices: " + err.message);
  }
}

/** Populate a <select> with device options. */
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

/** Sync UI with engine status. */
async function syncStatus() {
  try {
    const status = await window.ainoiceguard.getStatus();
    updateUI(status.running, status.level);
  } catch (err) {
    /* Silently ignore polling errors. */
  }
}

/* ── Toggle Noise Cancellation ───────────────────────────────────────────── */

toggleBtn.addEventListener("click", async () => {
  toggleBtn.disabled = true;

  try {
    if (isRunning) {
      const result = await window.ainoiceguard.stop();
      if (result.success) {
        updateUI(false);
      } else {
        showError(result.error || "Failed to stop");
      }
    } else {
      const inputIdx = parseInt(inputSelect.value, 10);
      const outputIdx = parseInt(outputSelect.value, 10);

      statusText.textContent = "Starting...";
      const result = await window.ainoiceguard.start(inputIdx, outputIdx);

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
    await window.ainoiceguard.setLevel(level);
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
    await window.ainoiceguard.setVadThreshold(threshold);
  } catch (err) {
    /* Non-critical */
  }
});

/* ── Device selection change while running -> restart ────────────────────── */

inputSelect.addEventListener("change", restartIfRunning);
outputSelect.addEventListener("change", restartIfRunning);

async function restartIfRunning() {
  if (!isRunning) return;

  try {
    stopMetricsPolling();
    statusText.textContent = "Restarting...";

    await window.ainoiceguard.stop();

    const inputIdx = parseInt(inputSelect.value, 10);
    const outputIdx = parseInt(outputSelect.value, 10);
    const result = await window.ainoiceguard.start(inputIdx, outputIdx);

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
  if (metricsInterval) return;

  noInputPollCount = 0;
  metricsInterval = setInterval(async () => {
    try {
      const m = await window.ainoiceguard.getMetrics();

      if (!isRunning) return;

      /* Coerce to numbers so we never show NaN (IPC or addon can return undefined) */
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
      gateText.textContent = Number.isFinite(gateGain) ? (gateGain * 100).toFixed(0) + "%" : "--";

      /* Hints: no input for ~2s, or output is Mute */
      if (meterHint) {
        const outputIsMute = parseInt(outputSelect.value, 10) === -2;
        if (inputRms <= 0.001) {
          noInputPollCount++;
          if (noInputPollCount >= 20) {
            meterHint.textContent = "No input signal — check microphone and device selection.";
            meterHint.classList.remove("hidden");
          } else if (outputIsMute) {
            meterHint.textContent = "Select an output (e.g. Speakers or CABLE) to hear processed audio.";
            meterHint.classList.remove("hidden");
          }
        } else {
          noInputPollCount = 0;
          if (outputIsMute) {
            meterHint.textContent = "Select an output (e.g. Speakers or CABLE) to hear processed audio.";
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

const { rmsToPercent, rmsToDb, formatFrameCount } = window.metricsUtils;

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

  inputSelect.disabled = false;
  outputSelect.disabled = false;

  if (level !== undefined) {
    const pct = Math.round(level * 100);
    levelSlider.value = pct;
    levelValue.textContent = pct + "%";
  }

  /* Start/stop metrics polling */
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

/**
 * Detect VB-Cable in the device lists.
 * If found in output: auto-select it and show green guide.
 * If not found: show yellow guide with download link.
 */
function detectVBCable(outputDevices) {
  /* Hide both banners initially */
  vbCableFound.classList.add("hidden");
  vbCableMissing.classList.add("hidden");

  const cableDevice = outputDevices.find((d) =>
    d.name.toLowerCase().includes("cable"),
  );

  if (cableDevice) {
    outputSelect.value = String(cableDevice.index);
    vbCableFound.classList.remove("hidden");
  } else {
    vbCableMissing.classList.remove("hidden");
  }
}

/* Open VB-Cable download link in the system browser. */
vbCableLink.addEventListener("click", (e) => {
  e.preventDefault();
  if (window.ainoiceguard.openExternal) {
    window.ainoiceguard.openExternal("https://vb-audio.com/Cable/");
  }
});

/* ── Boot ────────────────────────────────────────────────────────────────── */

init();
