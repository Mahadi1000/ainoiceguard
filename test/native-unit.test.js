/**
 * Runs the native engine unit tests (ng_engine_tests: EngineStats + RingBuffer).
 *
 * The binary is an optional dev build (-DNG_BUILD_BENCH=ON). If it is missing
 * the test is skipped, unless NG_BENCH_REQUIRED=1 (CI). Override the path
 * with NG_ENGINE_TESTS_BIN.
 */
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const ROOT = path.join(__dirname, "..");

function findBin() {
  if (process.env.NG_ENGINE_TESTS_BIN) {
    const p = path.resolve(ROOT, process.env.NG_ENGINE_TESTS_BIN);
    return fs.existsSync(p) ? p : undefined;
  }
  return [
    path.join(ROOT, "deps", "bench-build", "Release", "ng_engine_tests.exe"),
    path.join(ROOT, "deps", "bench-build", "ng_engine_tests"),
  ].find((p) => fs.existsSync(p));
}

const bin = findBin();
const required = process.env.NG_BENCH_REQUIRED === "1";

test("native engine unit tests", { skip: bin || required ? false : "ng_engine_tests not built" }, () => {
  assert.ok(bin, "ng_engine_tests not found (set NG_ENGINE_TESTS_BIN)");
  const r = spawnSync(bin, [], { encoding: "utf8" });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /all engine tests passed/);
});
