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
const { summarizeSession, formatSessionLine } = require("./session-stats");
const { loadSettings, saveSettings } = require("./settings");

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
let sessionStartedAt = null;

/* ── Settings ──────────────────────────────────────────────────────────────── */

function settingsFile() {
  return path.join(app.getPath("userData"), "settings.json");
}

/* ── Engine status events (device lost / restarted / fallback) ─────────────── */

/* Registered once, before any start(); events arrive from the audio thread. */
addon.onStatus((type, message) => {
  console.log(`[engine] ${type}: ${message}`);
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send("audio:status", { type, message });
  }
});

/* ── Engine session stats ──────────────────────────────────────────────────── */

/** CPU usage (%) of this (main) process, where the native addon runs. */
function mainProcessCpuPercent() {
  try {
    const m = app.getAppMetrics().find((p) => p.pid === process.pid);
    return m ? m.cpu.percentCPUUsage : null;
  } catch {
    return null;
  }
}

/**
 * Stop the engine and append one summary line per session to
 * <logs>/engine-sessions.jsonl. Counters are read before stop() resets them.
 */
function stopEngineAndLog(reason) {
  if (!addon.isRunning()) return;
  let engine = null;
  try {
    engine = addon.getMetrics().engine;
  } catch {
    /* metrics are best-effort; never block stopping */
  }
  addon.stop();
  if (!engine) return;
  try {
    const summary = summarizeSession(engine, {
      startedAt: sessionStartedAt,
      endedAt: new Date(),
      reason,
      cpuPercent: mainProcessCpuPercent(),
    });
    console.log(formatSessionLine(summary));
    const dir = app.getPath("logs");
    fs.mkdirSync(dir, { recursive: true });
    fs.appendFileSync(path.join(dir, "engine-sessions.jsonl"), JSON.stringify(summary) + "\n");
  } catch (err) {
    console.error("Could not write engine session stats:", err.message);
  } finally {
    sessionStartedAt = null;
  }
}

/* ── App Lifecycle ─────────────────────────────────────────────────────────── */

app.whenReady().then(() => {
  /* Restore saved processing settings before anything starts. */
  const s = loadSettings(settingsFile());
  try {
    addon.setNoiseLevel(s.level);
    addon.setVadThreshold(s.vadThreshold);
  } catch (err) {
    console.error("Could not apply saved settings:", err.message);
  }
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
    stopEngineAndLog("quit");
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

  mainWindow.once("ready-to-show", () => {
    mainWindow.show();
    mainWindow.focus();
  });

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
 * @param {string} inputId  - device id from getDevices(), "" = system default
 * @param {string} outputId - device id, "" = system default, "mute" = no output
 * (Legacy numeric indices are still accepted by the addon.)
 */
ipcMain.handle("audio:start", (_event, inputId, outputId) => {
  try {
    const { exclusive } = loadSettings(settingsFile());
    const errMsg = addon.start(
      inputId !== undefined ? inputId : "",
      outputId !== undefined ? outputId : "",
      { exclusive },
    );
    if (errMsg && errMsg.length > 0) {
      updateTrayMenu(false);
      return { success: false, error: errMsg };
    }
    sessionStartedAt = new Date();
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
    stopEngineAndLog("stop");
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
 * audio:get-metrics -> { inputRms, outputRms, vadProbability, gateGain, framesProcessed,
 *                        noiseFloor, engine: {...}, process: { cpuPercent } }
 * Polled from the renderer at ~100ms intervals for the level meter and logs.
 * `engine` = native health counters (xruns, drops, timing, latency estimate).
 */
ipcMain.handle("audio:get-metrics", () => {
  try {
    const m = addon.getMetrics();
    m.process = { cpuPercent: mainProcessCpuPercent() };
    return m;
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
 * settings:get -> settings object (see electron/settings.js)
 * settings:set(patch) -> merged settings, persisted atomically
 */
ipcMain.handle("settings:get", () => loadSettings(settingsFile()));

ipcMain.handle("settings:set", (_event, patch) => {
  try {
    return saveSettings(settingsFile(), patch);
  } catch (err) {
    console.error("Could not save settings:", err.message);
    return loadSettings(settingsFile());
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
