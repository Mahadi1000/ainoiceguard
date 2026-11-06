const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { DEFAULTS, sanitize, loadSettings, saveSettings } = require("../electron/settings");

function tmpFile() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ng-settings-"));
  return { dir, file: path.join(dir, "settings.json") };
}

test("missing file gives defaults", () => {
  const { dir, file } = tmpFile();
  try {
    assert.deepEqual(loadSettings(file), { ...DEFAULTS });
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test("corrupt file gives defaults", () => {
  const { dir, file } = tmpFile();
  try {
    fs.writeFileSync(file, "{ not json");
    assert.deepEqual(loadSettings(file), { ...DEFAULTS });
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test("save merges, persists device ids, and leaves no temp file", () => {
  const { dir, file } = tmpFile();
  try {
    saveSettings(file, { inputId: "Windows WASAPI|External Microphone (Synaptics HD Audio)" });
    const s = saveSettings(file, { outputId: "mute", level: 0.8 });
    assert.equal(s.inputId, "Windows WASAPI|External Microphone (Synaptics HD Audio)");
    assert.equal(s.outputId, "mute");
    assert.equal(s.level, 0.8);
    assert.deepEqual(loadSettings(file), s);
    assert.deepEqual(fs.readdirSync(dir), ["settings.json"]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test("sanitize clamps ranges and rejects wrong types", () => {
  const s = sanitize({ level: 7, vadThreshold: -1, inputId: 42, outputId: { x: 1 }, exclusive: "yes" });
  assert.equal(s.level, 1);
  assert.equal(s.vadThreshold, 0);
  assert.equal(s.inputId, DEFAULTS.inputId);
  assert.equal(s.outputId, DEFAULTS.outputId);
  assert.equal(s.exclusive, false);
  assert.equal(sanitize({ outputId: null }).outputId, null);
  assert.equal(sanitize({ level: NaN }).level, DEFAULTS.level);
});
