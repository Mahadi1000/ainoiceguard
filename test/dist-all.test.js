const test = require('node:test')
const assert = require('node:assert/strict')
const { planDistScripts } = require('../scripts/dist-all')

test('dist planner chooses only host-compatible script', () => {
  assert.deepEqual(planDistScripts('win32'), ['dist:win'])
  assert.deepEqual(planDistScripts('linux'), ['dist:linux'])
  assert.deepEqual(planDistScripts('darwin'), ['dist:mac'])
})

test('dist planner rejects unsupported platform', () => {
  assert.throws(
    () => planDistScripts('sunos'),
    /unsupported platform/i
  )
})
