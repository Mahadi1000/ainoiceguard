/**
 * Manual end-to-end check of device handling in the REAL app (main.js,
 * window, renderer), with a throwaway userData folder. Needs a microphone;
 * output is set to "mute", so nothing is played.
 *
 *   NG_ROOT=. NG_USERDATA=<empty dir> NG_PHASE=first   ./node_modules/.bin/electron tools/e2e-device-handling.js
 *   NG_ROOT=. NG_USERDATA=<same dir>  NG_PHASE=restore ./node_modules/.bin/electron tools/e2e-device-handling.js
 *
 * "first": select a mic (first whose name matches NG_MIC, default /Synaptics/),
 * mute output, set level 80%, start, force a device loss, change output twice
 * while running, stop. "restore": relaunch and read back what was restored.
 * Prints one line "E2E_RESULT {json}" with UI state, settings.json and the
 * engine-sessions.jsonl summaries. Not part of `npm test` (needs hardware).
 */
const { app, BrowserWindow } = require("electron");
const path = require("path");
const fs = require("fs");
const ROOT = process.env.NG_ROOT;
const USERDATA = process.env.NG_USERDATA;
const PHASE = process.env.NG_PHASE; // "first" or "restore"
app.setPath("userData", USERDATA);
app.setPath("logs", path.join(USERDATA, "logs"));
require(path.join(ROOT, "electron", "main.js"));
const addon = require(path.join(ROOT, "build", "Release", "ainoiceguard.node")); // same module instance
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
app.whenReady().then(async () => {
  let win;
  for (let i = 0; i < 50 && !(win = BrowserWindow.getAllWindows()[0]); i++) await sleep(100);
  await new Promise((r) => (win.webContents.isLoading() ? win.webContents.once("did-finish-load", r) : r()));
  await sleep(800); // let init() load settings + devices
  const js = (code) => win.webContents.executeJavaScript(code);
  const ui = () => js(`({ input: inputSelect.value, output: outputSelect.value,
    inputOptions: [...inputSelect.options].map(o => o.value || '(default)'),
    status: statusText.textContent, latency: latencyText.textContent,
    error: errorBar.classList.contains('hidden') ? '' : errorBar.textContent,
    level: levelSlider.value, vad: vadThreshSlider.value })`);
  const out = {};
  if (PHASE === "first") {
    out.initial = await ui();
    await js(`(() => { const mic = [...inputSelect.options].find(o => new RegExp(${JSON.stringify(process.env.NG_MIC || "Synaptics")}).test(o.textContent));
      inputSelect.value = mic.value; inputSelect.dispatchEvent(new Event('change'));
      outputSelect.value = 'mute'; outputSelect.dispatchEvent(new Event('change'));
      levelSlider.value = 80; levelSlider.dispatchEvent(new Event('change')); })()`);
    await js(`toggleBtn.click()`);
    await sleep(2000);
    out.running = await ui();
    addon._debugForceDeviceLoss();
    await sleep(150);
    out.duringLoss = await ui();
    await sleep(2500);
    out.afterRestart = await ui();
    await js(`outputSelect.value = ''; outputSelect.dispatchEvent(new Event('change'));
              outputSelect.value = 'mute'; outputSelect.dispatchEvent(new Event('change'));`);
    await sleep(500);
    await js(`toggleBtn.click()`);
    await sleep(800);
    out.stopped = await ui();
  } else {
    out.restored = await ui();
  }
  out.settingsFile = JSON.parse(fs.readFileSync(path.join(USERDATA, "settings.json"), "utf8"));
  const log = path.join(USERDATA, "logs", "engine-sessions.jsonl");
  out.sessions = fs.existsSync(log) ? fs.readFileSync(log, "utf8").trim().split("\n").map((l) => {
    const s = JSON.parse(l); return { reason: s.reason, durationS: s.durationS, input: s.input, restarts: s.recovery.restarts, latency: s.latencyMs.estimatedTotal, dropped: s.dropouts.captureDroppedSamples }; }) : [];
  console.log("E2E_RESULT " + JSON.stringify(out, null, 1));
  app.quit();
});
