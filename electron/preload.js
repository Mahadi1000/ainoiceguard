/**
 * Ainoiceguard - Preload Script
 *
 * Bridges the Electron main process and renderer via contextBridge.
 * The renderer cannot directly access Node.js or the native addon.
 * Instead, it calls these safe IPC wrappers.
 */

const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("ainoiceguard", {
  getDevices: () => ipcRenderer.invoke("audio:get-devices"),
  /* inputId/outputId: stable device ids from getDevices(); "" = default, "mute" = no output */
  start: (inputId, outputId) =>
    ipcRenderer.invoke("audio:start", inputId, outputId),
  getSettings: () => ipcRenderer.invoke("settings:get"),
  setSettings: (patch) => ipcRenderer.invoke("settings:set", patch),
  /* Engine status events: cb({ type, message }); returns an unsubscribe fn. */
  onStatus: (cb) => {
    const listener = (_e, ev) => cb(ev);
    ipcRenderer.on("audio:status", listener);
    return () => ipcRenderer.removeListener("audio:status", listener);
  },
  stop: () => ipcRenderer.invoke("audio:stop"),
  setLevel: (level) => ipcRenderer.invoke("audio:set-level", level),
  getStatus: () => ipcRenderer.invoke("audio:get-status"),
  getMetrics: () => ipcRenderer.invoke("audio:get-metrics"),
  setVadThreshold: (threshold) =>
    ipcRenderer.invoke("audio:set-vad-threshold", threshold),
  openExternal: (url) => ipcRenderer.invoke("app:open-external", url),
});
