const test = require('node:test')
const assert = require('node:assert/strict')
const { byPlatform } = require('../scripts/dist-all.js')

test('dist planner chooses only host-compatible script', () => {
  assert.deepEqual(byPlatform.win32, ['dist:win'])
  assert.deepEqual(byPlatform.linux, ['dist:linux'])
  assert.deepEqual(byPlatform.darwin, ['dist:mac'])
})
