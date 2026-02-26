(function setupMetricsUtils(globalScope) {
  function rmsToPercent(rms) {
    if (rms <= 0.001) return 0;
    const db = 20 * Math.log10(rms);
    return Math.max(0, Math.min(100, ((db + 60) / 60) * 100));
  }

  function rmsToDb(rms) {
    if (rms <= 0.0001) return "-\u221E";
    const db = 20 * Math.log10(rms);
    return db.toFixed(0) + "dB";
  }

  function formatFrameCount(n) {
    if (n >= 1000000) return (n / 1000000).toFixed(1) + "M";
    if (n >= 1000) return (n / 1000).toFixed(1) + "K";
    return String(n);
  }

  const api = { rmsToPercent, rmsToDb, formatFrameCount };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = api;
  }

  if (globalScope) {
    globalScope.metricsUtils = api;
  }
})(typeof window !== "undefined" ? window : globalThis);
