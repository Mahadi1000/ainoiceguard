/**
 * Ainoiceguard - Preload Script
 *
 * Bridges the Electron main process and renderer via contextBridge.
 * The renderer cannot directly access Node.js or the native addon.
 * Instead, it calls these safe IPC wrappers.
 */

const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("ainoiceguard", {
  /** Get available audio devices. */
  getDevices: () => ipcRenderer.invoke("audio:get-devices"),

  /** Start noise cancellation with selected devices. */
  start: (inputIdx, outputIdx) =>
    ipcRenderer.invoke("audio:start", inputIdx, outputIdx),

  /** Stop noise cancellation. */
  stop: () => ipcRenderer.invoke("audio:stop"),

  /** Set suppression level [0.0 = off, 1.0 = full]. */
  setLevel: (level) => ipcRenderer.invoke("audio:set-level", level),

  /** Get current engine status. */
  getStatus: () => ipcRenderer.invoke("audio:get-status"),

  /** Get real-time audio metrics (input/output RMS, VAD, gate, frame count). */
  getMetrics: () => ipcRenderer.invoke("audio:get-metrics"),

  /** Set VAD gate threshold [0.0, 1.0]. Higher = more aggressive gating. */
  setVadThreshold: (threshold) =>
    ipcRenderer.invoke("audio:set-vad-threshold", threshold),

  /** Open a URL in the system's default browser. */
  openExternal: (url) => ipcRenderer.invoke("app:open-external", url),

  /** Pure utility functions for converting audio metrics. */
  metricsUtils: require("./metrics-utils"),
});
