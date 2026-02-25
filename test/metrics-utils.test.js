const test = require('node:test')
const assert = require('node:assert/strict')
const { rmsToPercent, rmsToDb, formatFrameCount } = require('../electron/metrics-utils')

test('rmsToPercent maps silence and clamps range', () => {
  assert.equal(rmsToPercent(0), 0)
  assert.equal(rmsToPercent(0.001), 0)
  assert.equal(rmsToPercent(1), 100)
  assert.equal(Math.round(rmsToPercent(0.01)), 33)
})

test('rmsToDb formats values for UI', () => {
  assert.equal(rmsToDb(0), '-∞')
  assert.equal(rmsToDb(1), '0dB')
  assert.equal(rmsToDb(0.1), '-20dB')
})

test('formatFrameCount abbreviates large values', () => {
  assert.equal(formatFrameCount(950), '950')
  assert.equal(formatFrameCount(1500), '1.5K')
  assert.equal(formatFrameCount(1500000), '1.5M')
})
