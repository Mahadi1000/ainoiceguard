/**
 * Golden regression test for the noise-suppression output.
 *
 * Runs ng_bench on the fixed inputs in test/golden/ and compares against the
 * stored expected outputs (RMS tolerance from golden.json).
 *
 * ng_bench is an optional dev build (-DNG_BUILD_BENCH=ON). If it is not found
 * the test is skipped, unless NG_BENCH_REQUIRED=1 (set in CI), in which case a
 * missing binary is a failure. Override the binary path with NG_BENCH_BIN.
 */
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const ROOT = path.join(__dirname, "..");
const GOLDEN = path.join(__dirname, "golden");

function findBench() {
  /* An explicit path is authoritative: never fall back to another build. */
  if (process.env.NG_BENCH_BIN) {
    const p = path.resolve(ROOT, process.env.NG_BENCH_BIN);
    return fs.existsSync(p) ? p : undefined;
  }
  const candidates = [
    path.join(ROOT, "deps", "bench-build", "Release", "ng_bench.exe"),
    path.join(ROOT, "deps", "bench-build", "ng_bench"),
    path.join(ROOT, "deps", "bench-build", "ng_bench.exe"),
  ].filter(Boolean);
  return candidates.find((p) => fs.existsSync(p));
}

const bench = findBench();
const required = process.env.NG_BENCH_REQUIRED === "1";
const spec = JSON.parse(fs.readFileSync(path.join(GOLDEN, "golden.json"), "utf8"));

test("ng_bench binary is available when required", () => {
  if (required) assert.ok(bench, "ng_bench not found (set NG_BENCH_BIN)");
});

for (const c of spec.cases) {
  test(`golden: ${c.backend} on ${c.input}`, { skip: bench ? false : "ng_bench not built" }, () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "ng-golden-"));
    try {
      const out = path.join(tmp, "out.wav");
      const run = spawnSync(bench, [
        "process", "--backend", c.backend, "--format", spec.format,
        "--in", path.join(GOLDEN, c.input), "--out", out, "--json", path.join(tmp, "stats.json"),
      ], { encoding: "utf8" });
      assert.equal(run.status, 0, run.stderr);

      const cmp = spawnSync(bench, [
        "compare", "--a", out, "--b", path.join(GOLDEN, c.expected),
        "--tol", String(spec.tolerance_rms),
      ], { encoding: "utf8" });
      const result = JSON.parse(cmp.stdout);
      assert.equal(result.match, true,
        `output drifted from golden: rms_diff=${result.rms_diff} (tol ${result.tol})`);
    } finally {
      fs.rmSync(tmp, { recursive: true, force: true });
    }
  });
}
