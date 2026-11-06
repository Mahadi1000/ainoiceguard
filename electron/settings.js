/**
 * Persistent user settings (<userData>/settings.json).
 *
 * Devices are stored by stable id ("<hostApi>|<name>", from getDevices()),
 * never by PortAudio index, which changes whenever devices are added or
 * removed. Writes are atomic (temp file + rename) so a crash cannot leave a
 * truncated file; a missing or corrupt file falls back to defaults.
 *
 * No Electron imports: unit-tested with node:test.
 */
const fs = require("fs");
const path = require("path");

const DEFAULTS = Object.freeze({
  version: 1,
  inputId: "",       // "" = system default (preferred host API)
  outputId: null,    // null = never chosen (renderer may auto-pick VB-Cable); "" = default; "mute"
  level: 1.0,        // suppression level [0, 1]
  vadThreshold: 0.5, // gate threshold [0, 1]
  exclusive: false,  // WASAPI exclusive mode (opt-in)
});

const clamp01 = (v, d) => (typeof v === "number" && Number.isFinite(v) ? Math.min(1, Math.max(0, v)) : d);
const str = (v, d) => (typeof v === "string" && v.length <= 512 ? v : d);

/** Validate/normalise any object into a complete settings object. */
function sanitize(raw) {
  const s = raw && typeof raw === "object" ? raw : {};
  return {
    version: DEFAULTS.version,
    inputId: str(s.inputId, DEFAULTS.inputId),
    outputId: s.outputId === null ? null : str(s.outputId, DEFAULTS.outputId),
    level: clamp01(s.level, DEFAULTS.level),
    vadThreshold: clamp01(s.vadThreshold, DEFAULTS.vadThreshold),
    exclusive: typeof s.exclusive === "boolean" ? s.exclusive : DEFAULTS.exclusive,
  };
}

function loadSettings(file) {
  try {
    return sanitize(JSON.parse(fs.readFileSync(file, "utf8")));
  } catch {
    return sanitize({});
  }
}

/** Merge `patch` into the stored settings and write atomically. Returns the result. */
function saveSettings(file, patch) {
  const next = sanitize({ ...loadSettings(file), ...(patch || {}) });
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const tmp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify(next, null, 2));
  fs.renameSync(tmp, file);
  return next;
}

module.exports = { DEFAULTS, sanitize, loadSettings, saveSettings };
