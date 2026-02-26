/**
 * Ainoiceguard - Electron Main Process
 *
 * Responsibilities:
 * - Load the native C++ addon (ainoiceguard.node)
 * - Create system tray icon (no visible window by default)
 * - Handle IPC from renderer for start/stop/device selection
 * - Ensure clean shutdown of audio engine on app exit
 */

const { app, BrowserWindow, ipcMain, shell } = require("electron");
const path = require("path");
const fs = require("fs");
const { createTray, destroyTray, updateTrayMenu } = require("./tray");

/* ── Load native addon ─────────────────────────────────────────────────────── */
let addon;
try {
  const addonName = "ainoiceguard.node";
  const rootDir = path.join(__dirname, "..");
  const candidates = [
    path.join(rootDir, "build", "Release", addonName),
    path.join(rootDir, "native", "build", "Release", addonName),
    path.join(
      __dirname.replace("app.asar", "app.asar.unpacked"),
      "..",
      "build",
      "Release",
      addonName,
    ),
  ];

  const addonPath = candidates.find((p) => fs.existsSync(p));
  if (!addonPath) {
    console.error("Native addon not found. Tried:", candidates);
    process.exit(1);
  }
  addon = require(addonPath);
  console.log("Loaded native addon from:", addonPath);
} catch (err) {
  console.error("Failed to load native addon:", err.message);
  const nativeBuildHint = process.platform === "win32" ? "npm run build:native" : "npm run build:native:unix";
  console.error(`Did you run "${nativeBuildHint}" first?`);
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
app.on("window-all-closed", () => {
  /* Intentionally empty: keep running in system tray. */
});

/* Clean shutdown: stop audio engine before quitting. */
app.on("before-quit", () => {
  app.isQuitting = true;
  console.log("Shutting down audio engine...");
  try {
    if (addon.isRunning()) {
      addon.stop();
    }
  } catch (err) {
    console.error("Error stopping audio engine:", err.message);
  }
  destroyTray();
});

/* ── Main Window (Hidden) ──────────────────────────────────────────────────── */

function createMainWindow() {
  const { screen } = require("electron");
  const primaryDisplay = screen.getPrimaryDisplay();
  const { width: screenW, height: screenH } = primaryDisplay.workAreaSize;

  const winW = 380;
  const winH = 640;

  mainWindow = new BrowserWindow({
    width: winW,
    height: winH,
    x: screenW - winW - 12,
    y: screenH - winH - 12,
    show: false,
    frame: false,
    resizable: false,
    skipTaskbar: false,
    transparent: false,
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  mainWindow.loadFile(path.join(__dirname, "index.html"));

  /* Hide instead of close so the tray can re-show it. */
  mainWindow.on("close", (e) => {
    if (!app.isQuitting) {
      e.preventDefault();
      mainWindow.hide();
    }
  });
}

/* ── IPC Handlers ──────────────────────────────────────────────────────────── */

/**
 * audio:get-devices -> { inputs: [...], outputs: [...] }
 */
ipcMain.handle("audio:get-devices", () => {
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
ipcMain.handle("audio:start", (_event, inputIdx, outputIdx) => {
  try {
    const errMsg = addon.start(
      inputIdx !== undefined ? inputIdx : -1,
      outputIdx !== undefined ? outputIdx : -1,
    );
    if (errMsg && errMsg.length > 0) {
      updateTrayMenu(false);
      return { success: false, error: errMsg };
    }
    updateTrayMenu(true);
    return { success: true };
  } catch (err) {
    return { success: false, error: err.message };
  }
});

/**
 * audio:stop -> { success: boolean }
 */
ipcMain.handle("audio:stop", () => {
  try {
    addon.stop();
    updateTrayMenu(false);
    return { success: true };
  } catch (err) {
    return { success: false, error: err.message };
  }
});

/**
 * audio:set-level -> void
 * @param {number} level - Suppression level [0.0, 1.0]
 */
ipcMain.handle("audio:set-level", (_event, level) => {
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
ipcMain.handle("audio:get-status", () => {
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
ipcMain.handle("audio:get-metrics", () => {
  try {
    return addon.getMetrics();
  } catch (err) {
    return {
      inputRms: 0,
      outputRms: 0,
      vadProbability: 0,
      gateGain: 0,
      framesProcessed: 0,
    };
  }
});

/**
 * audio:set-vad-threshold -> { success: boolean }
 * @param {number} threshold - VAD gate threshold [0.0, 1.0]
 */
ipcMain.handle("audio:set-vad-threshold", (_event, threshold) => {
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
ipcMain.handle("app:open-external", (_event, url) => {
  if (typeof url === "string" && url.startsWith("https://")) {
    shell.openExternal(url);
  }
});
