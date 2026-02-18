/**
 * NoiseGuard - Electron Main Process
 *
 * Responsibilities:
 * - Load the native C++ addon (noiseguard.node)
 * - Create system tray icon (no visible window by default)
 * - Handle IPC from renderer for start/stop/device selection
 * - Ensure clean shutdown of audio engine on app exit
 */

const { app, BrowserWindow, ipcMain, shell } = require('electron');
const path = require('path');
const { createTray, destroyTray } = require('./tray');

/* ── Load native addon ─────────────────────────────────────────────────────── */
let addon;
try {
  const addonPath = path.join(__dirname, '..', 'build', 'Release', 'noiseguard.node');
  addon = require(addonPath);
} catch (err) {
  console.error('Failed to load native addon:', err.message);
  console.error('Did you run "npm run build:native" first?');
  process.exit(1);
}

/* ── State ─────────────────────────────────────────────────────────────────── */
let mainWindow = null;

/* ── App Lifecycle ─────────────────────────────────────────────────────────── */

app.whenReady().then(() => {
  createMainWindow();
  createTray(mainWindow);
});

/* Prevent app from quitting when all windows are closed (tray app behavior). */
app.on('window-all-closed', (e) => {
  e.preventDefault();
});

/* Clean shutdown: stop audio engine before quitting. */
app.on('before-quit', () => {
  console.log('Shutting down audio engine...');
  try {
    if (addon.isRunning()) {
      addon.stop();
    }
  } catch (err) {
    console.error('Error stopping audio engine:', err.message);
  }
  destroyTray();
});

/* ── Main Window (Hidden) ──────────────────────────────────────────────────── */

function createMainWindow() {
  mainWindow = new BrowserWindow({
    width: 380,
    height: 720,
    show: false,          /* Start hidden -- tray icon shows the window. */
    frame: false,         /* Frameless for a clean tray-popup look. */
    resizable: false,
    skipTaskbar: true,     /* Don't show in taskbar. */
    transparent: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  mainWindow.loadFile(path.join(__dirname, 'index.html'));

  /* Hide instead of close so the tray can re-show it. */
  mainWindow.on('close', (e) => {
    if (!app.isQuitting) {
      e.preventDefault();
      mainWindow.hide();
    }
  });

  /* Lose focus -> hide (tray popup behavior). */
  mainWindow.on('blur', () => {
    if (mainWindow.isVisible()) {
      mainWindow.hide();
    }
  });
}

/* ── IPC Handlers ──────────────────────────────────────────────────────────── */

/**
 * audio:get-devices -> { inputs: [...], outputs: [...] }
 */
ipcMain.handle('audio:get-devices', () => {
  try {
    return addon.getDevices();
  } catch (err) {
    return { inputs: [], outputs: [], error: err.message };
  }
});

/**
 * audio:start -> { success: boolean, error?: string }
 * @param {number} inputIdx  - Input device index (-1 for default)
 * @param {number} outputIdx - Output device index (-1 for default)
 */
ipcMain.handle('audio:start', (_event, inputIdx, outputIdx) => {
  try {
    const errMsg = addon.start(
      inputIdx !== undefined ? inputIdx : -1,
      outputIdx !== undefined ? outputIdx : -1
    );
    if (errMsg && errMsg.length > 0) {
      return { success: false, error: errMsg };
    }
    return { success: true };
  } catch (err) {
    return { success: false, error: err.message };
  }
});

/**
 * audio:stop -> { success: boolean }
 */
ipcMain.handle('audio:stop', () => {
  try {
    addon.stop();
    return { success: true };
  } catch (err) {
    return { success: false, error: err.message };
  }
});

/**
 * audio:set-level -> void
 * @param {number} level - Suppression level [0.0, 1.0]
 */
ipcMain.handle('audio:set-level', (_event, level) => {
  try {
    addon.setNoiseLevel(level);
    return { success: true };
  } catch (err) {
    return { success: false, error: err.message };
  }
});

/**
 * audio:get-status -> { running: boolean, level: number }
 */
ipcMain.handle('audio:get-status', () => {
  try {
    return {
      running: addon.isRunning(),
      level: addon.getNoiseLevel(),
    };
  } catch (err) {
    return { running: false, level: 1.0, error: err.message };
  }
});

/**
 * audio:get-metrics -> { inputRms, outputRms, vadProbability, gateGain, framesProcessed }
 * Polled from the renderer at ~100ms intervals for the level meter and logs.
 */
ipcMain.handle('audio:get-metrics', () => {
  try {
    return addon.getMetrics();
  } catch (err) {
    return { inputRms: 0, outputRms: 0, vadProbability: 0, gateGain: 0, framesProcessed: 0 };
  }
});

/**
 * audio:set-vad-threshold -> { success: boolean }
 * @param {number} threshold - VAD gate threshold [0.0, 1.0]
 */
ipcMain.handle('audio:set-vad-threshold', (_event, threshold) => {
  try {
    addon.setVadThreshold(threshold);
    return { success: true };
  } catch (err) {
    return { success: false, error: err.message };
  }
});

/**
 * app:open-external -> void
 * Open a URL in the system's default browser (used for VB-Cable download link).
 */
ipcMain.handle('app:open-external', (_event, url) => {
  if (typeof url === 'string' && url.startsWith('https://')) {
    shell.openExternal(url);
  }
});
