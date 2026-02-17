/**
 * NoiseGuard - Renderer Process (Vanilla JS)
 *
 * Handles UI interaction and communicates with main process via the
 * preload-exposed `window.noiseGuard` bridge.
 *
 * No framework dependencies -- pure DOM manipulation.
 */

/* ── DOM References ──────────────────────────────────────────────────────── */

const toggleBtn = document.getElementById('toggleBtn');
const toggleHint = document.getElementById('toggleHint');
const statusDot = document.getElementById('statusDot');
const inputSelect = document.getElementById('inputSelect');
const outputSelect = document.getElementById('outputSelect');
const levelSlider = document.getElementById('levelSlider');
const levelValue = document.getElementById('levelValue');
const statusText = document.getElementById('statusText');
const latencyText = document.getElementById('latencyText');
const errorBar = document.getElementById('errorBar');

/* ── State ───────────────────────────────────────────────────────────────── */

let isRunning = false;

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
    const devices = await window.noiseGuard.getDevices();

    if (devices.error) {
      showError(devices.error);
      return;
    }

    populateSelect(inputSelect, devices.inputs, 'input');
    populateSelect(outputSelect, devices.outputs, 'output');

    hideError();
  } catch (err) {
    showError('Failed to load audio devices: ' + err.message);
  }
}

/** Populate a <select> with device options. */
function populateSelect(select, devices, type) {
  /* Keep the default option. */
  select.innerHTML = '<option value="-1">System Default</option>';

  for (const d of devices) {
    const opt = document.createElement('option');
    opt.value = d.index;
    opt.textContent = d.name;

    /* Highlight VB-Cable devices for easy identification. */
    if (d.name.toLowerCase().includes('cable')) {
      opt.textContent += ' [VB-Cable]';
    }

    select.appendChild(opt);
  }
}

/** Sync UI with engine status. */
async function syncStatus() {
  try {
    const status = await window.noiseGuard.getStatus();
    updateUI(status.running, status.level);
  } catch (err) {
    /* Silently ignore polling errors. */
  }
}

/* ── Toggle Noise Cancellation ───────────────────────────────────────────── */

toggleBtn.addEventListener('click', async () => {
  toggleBtn.disabled = true;

  try {
    if (isRunning) {
      const result = await window.noiseGuard.stop();
      if (result.success) {
        updateUI(false);
      } else {
        showError(result.error || 'Failed to stop');
      }
    } else {
      const inputIdx = parseInt(inputSelect.value, 10);
      const outputIdx = parseInt(outputSelect.value, 10);

      statusText.textContent = 'Starting...';
      const result = await window.noiseGuard.start(inputIdx, outputIdx);

      if (result.success) {
        updateUI(true);
        hideError();
      } else {
        showError(result.error || 'Failed to start');
        statusText.textContent = 'Error';
      }
    }
  } catch (err) {
    showError(err.message);
  } finally {
    toggleBtn.disabled = false;
  }
});

/* ── Suppression Level Slider ────────────────────────────────────────────── */

levelSlider.addEventListener('input', () => {
  const pct = parseInt(levelSlider.value, 10);
  levelValue.textContent = pct + '%';
});

/* Debounced: only send to native on change (mouseup / touchend). */
levelSlider.addEventListener('change', async () => {
  const level = parseInt(levelSlider.value, 10) / 100.0;
  try {
    await window.noiseGuard.setLevel(level);
  } catch (err) {
    /* Non-critical -- slider value will apply on next frame. */
  }
});

/* ── Device selection change while running -> restart ────────────────────── */

inputSelect.addEventListener('change', restartIfRunning);
outputSelect.addEventListener('change', restartIfRunning);

async function restartIfRunning() {
  if (!isRunning) return;

  /* Stop and restart with new devices. */
  await window.noiseGuard.stop();
  const inputIdx = parseInt(inputSelect.value, 10);
  const outputIdx = parseInt(outputSelect.value, 10);

  statusText.textContent = 'Restarting...';
  const result = await window.noiseGuard.start(inputIdx, outputIdx);

  if (result.success) {
    updateUI(true);
    hideError();
  } else {
    showError(result.error || 'Restart failed');
    updateUI(false);
  }
}

/* ── UI Update Helpers ───────────────────────────────────────────────────── */

function updateUI(running, level) {
  isRunning = running;

  /* Toggle button. */
  toggleBtn.classList.toggle('on', running);
  toggleBtn.classList.toggle('off', !running);
  toggleBtn.querySelector('.toggle-label').textContent = running ? 'ON' : 'OFF';

  /* Hint text. */
  toggleHint.textContent = running
    ? 'Noise cancellation active'
    : 'Click to enable noise cancellation';

  /* Status dot. */
  statusDot.classList.toggle('active', running);

  /* Status text. */
  statusText.textContent = running ? 'Active' : 'Idle';

  /* Latency estimate: 10ms frame + ~2ms processing + buffer. */
  latencyText.textContent = running ? '~12 ms' : '-- ms';

  /* Disable device selects while running (changing requires restart). */
  inputSelect.disabled = false;
  outputSelect.disabled = false;

  /* Sync slider if level provided. */
  if (level !== undefined) {
    const pct = Math.round(level * 100);
    levelSlider.value = pct;
    levelValue.textContent = pct + '%';
  }
}

function showError(msg) {
  errorBar.textContent = msg;
  errorBar.classList.remove('hidden');
}

function hideError() {
  errorBar.classList.add('hidden');
  errorBar.textContent = '';
}

/* ── Boot ────────────────────────────────────────────────────────────────── */

init();
