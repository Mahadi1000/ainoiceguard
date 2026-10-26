/**
 * Session statistics: condenses the addon's getMetrics().engine counters into
 * one JSON record per audio session (start -> stop), appended to
 * <logs>/engine-sessions.jsonl. Pure functions here; file I/O is in main.js.
 *
 * Kept free of Electron imports so it can be unit-tested with node:test.
 */

/** Round to `d` decimals (keeps log lines short and diffable). */
function round(v, d = 1) {
  if (typeof v !== "number" || !Number.isFinite(v)) return v;
  const f = 10 ** d;
  return Math.round(v * f) / f;
}

/**
 * Build a session record.
 * @param {object} engine   getMetrics().engine, read just before stop()
 * @param {object} info     { startedAt: Date, endedAt: Date, reason: string,
 *                            cpuPercent?: number }
 */
function summarizeSession(engine, info) {
  const e = engine || {};
  const s = e.streams || {};
  const x = e.xruns || {};
  const r = e.ring || {};
  const p = e.processing || {};
  const l = e.latency || {};
  const rec = e.recovery || {};
  const durationS = info.startedAt && info.endedAt ? (info.endedAt - info.startedAt) / 1000 : null;
  const frames = p.frames || 0;

  return {
    startedAt: info.startedAt ? info.startedAt.toISOString() : null,
    endedAt: info.endedAt ? info.endedAt.toISOString() : null,
    durationS: round(durationS, 1),
    reason: info.reason || "stop",
    input: { name: s.inputName ?? null, hostApi: s.inputHostApi ?? null, exclusive: !!s.inputExclusive },
    output: s.outputDevice >= 0
      ? { name: s.outputName ?? null, hostApi: s.outputHostApi ?? null, exclusive: !!s.outputExclusive }
      : null,
    xruns: {
      inputUnderflow: x.inputUnderflow || 0,
      inputOverflow: x.inputOverflow || 0,
      outputUnderflow: x.outputUnderflow || 0,
      outputOverflow: x.outputOverflow || 0,
    },
    dropouts: {
      captureDroppedSamples: r.captureDroppedSamples || 0,
      outputDroppedSamples: r.outputDroppedSamples || 0,
      outputUnderrunEvents: r.outputUnderrunEvents || 0,
      outputUnderrunSamples: r.outputUnderrunSamples || 0,
    },
    processing: {
      frames,
      meanUs: round(p.meanUs, 0),
      maxUs: p.maxUs || 0,
      overruns: p.overruns || 0,
      overrunPct: frames ? round((100 * (p.overruns || 0)) / frames, 3) : 0,
      histogramUpperUs: p.histogramUpperUs || [],
      histogramCounts: p.histogramCounts || [],
    },
    ringFill: {
      captureMean: round(r.captureFillMean, 0),
      captureMax: r.captureFillMax || 0,
      outputMean: round(r.outputFillMean, 0),
      outputMin: r.outputFillMin ?? null,
      outputMax: r.outputFillMax || 0,
    },
    latencyMs: {
      inputDevice: round(l.inputDeviceMs),
      captureRing: round(l.captureRingMs),
      backend: round(l.backendMs),
      outputRing: round(l.outputRingMs),
      outputDevice: round(l.outputDeviceMs),
      estimatedTotal: round(l.estimatedTotalMs),
    },
    recovery: {
      restarts: rec.restarts || 0,
      restartFailures: rec.restartFailures || 0,
      deviceLossEvents: rec.deviceLossEvents || 0,
    },
    cpuPercent: round(info.cpuPercent),
  };
}

/** Short one-line human summary for the console. */
function formatSessionLine(s) {
  const d = s.dropouts;
  const drops = d.captureDroppedSamples + d.outputDroppedSamples + d.outputUnderrunSamples;
  const xr = s.xruns.inputUnderflow + s.xruns.inputOverflow + s.xruns.outputUnderflow + s.xruns.outputOverflow;
  return (
    `[engine] session ${s.durationS}s ${s.input.hostApi || "?"}: ` +
    `latency~${s.latencyMs.estimatedTotal}ms, proc mean ${s.processing.meanUs}us max ${s.processing.maxUs}us ` +
    `(${s.processing.overruns} over budget), xruns ${xr}, dropped/underrun samples ${drops}, ` +
    `restarts ${s.recovery.restarts}`
  );
}

module.exports = { summarizeSession, formatSessionLine };
