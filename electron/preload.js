/**
 * NoiseGuard - Preload Script
 *
 * Bridges the Electron main process and renderer via contextBridge.
 * The renderer cannot directly access Node.js or the native addon.
 * Instead, it calls these safe IPC wrappers.
 */

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('noiseGuard', {
  /** Get available audio devices. */
  getDevices: () => ipcRenderer.invoke('audio:get-devices'),

  /** Start noise cancellation with selected devices. */
  start: (inputIdx, outputIdx) =>
    ipcRenderer.invoke('audio:start', inputIdx, outputIdx),

  /** Stop noise cancellation. */
  stop: () => ipcRenderer.invoke('audio:stop'),

  /** Set suppression level [0.0 = off, 1.0 = full]. */
  setLevel: (level) => ipcRenderer.invoke('audio:set-level', level),

  /** Get current engine status. */
  getStatus: () => ipcRenderer.invoke('audio:get-status'),
});
