const test = require("node:test");
const assert = require("node:assert/strict");
const { summarizeSession, formatSessionLine } = require("../electron/session-stats");

function sampleEngine(overrides = {}) {
  return {
    running: true,
    streams: {
      inputDevice: 5, inputName: "Mic", inputHostApi: "Windows WASAPI",
      outputDevice: -1, outputName: null, outputHostApi: null,
      inputExclusive: false, outputExclusive: false, sampleRate: 48000, ringCapacity: 4096,
    },
    xruns: { inputUnderflow: 0, inputOverflow: 2, outputUnderflow: 0, outputOverflow: 0 },
    callbacks: { capture: 500, output: 0 },
    ring: {
      captureFill: 0, outputFill: 0, captureFillMean: 34.49, captureFillMax: 2400,
      outputFillMean: 0, outputFillMin: null, outputFillMax: 0,
      captureDroppedSamples: 705, outputDroppedSamples: 0, outputUnderrunEvents: 1, outputUnderrunSamples: 480,
    },
    processing: {
      frames: 500, meanUs: 1153.4, maxUs: 17375, budgetUs: 10000, overruns: 2,
      histogramUpperUs: [100, 250, 500, 1000, 2000, 5000, 10000, null],
      histogramCounts: [0, 1, 132, 155, 159, 47, 4, 2],
    },
    latency: {
      inputDeviceMs: 22, outputDeviceMs: 0, captureRingMs: 0.72, outputRingMs: 0,
      backendMs: 10, estimatedTotalMs: 32.72,
    },
    recovery: { restarts: 1, restartFailures: 0, deviceLossEvents: 1 },
    ...overrides,
  };
}

test("summarizeSession condenses engine counters", () => {
  const s = summarizeSession(sampleEngine(), {
    startedAt: new Date("2026-10-24T10:00:00Z"),
    endedAt: new Date("2026-10-24T10:00:05Z"),
    reason: "stop",
    cpuPercent: 3.456,
  });
  assert.equal(s.durationS, 5);
  assert.deepEqual(s.input, { name: "Mic", hostApi: "Windows WASAPI", exclusive: false });
  assert.equal(s.output, null, "muted output is reported as null");
  assert.equal(s.xruns.inputOverflow, 2);
  assert.equal(s.dropouts.captureDroppedSamples, 705);
  assert.equal(s.processing.overrunPct, 0.4);
  assert.equal(s.processing.meanUs, 1153);
  assert.equal(s.latencyMs.estimatedTotal, 32.7);
  assert.equal(s.recovery.restarts, 1);
  assert.equal(s.cpuPercent, 3.5);
});

test("summarizeSession tolerates missing data", () => {
  const s = summarizeSession(undefined, { reason: "quit" });
  assert.equal(s.durationS, null);
  assert.equal(s.reason, "quit");
  assert.equal(s.processing.frames, 0);
  assert.equal(s.processing.overrunPct, 0);
  assert.equal(s.dropouts.outputUnderrunSamples, 0);
});

test("formatSessionLine totals xruns and dropouts", () => {
  const s = summarizeSession(sampleEngine(), {
    startedAt: new Date(0), endedAt: new Date(5000), reason: "stop",
  });
  const line = formatSessionLine(s);
  assert.match(line, /Windows WASAPI/);
  assert.match(line, /xruns 2/);
  assert.match(line, /dropped\/underrun samples 1185/);
  assert.match(line, /restarts 1/);
});
