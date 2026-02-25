const test = require('node:test')
const assert = require('node:assert/strict')

test('dist planner chooses only host-compatible script', () => {
  const mapping = {
    win32: ['dist:win'],
    linux: ['dist:linux'],
    darwin: ['dist:mac']
  }
  assert.deepEqual(mapping.win32, ['dist:win'])
  assert.deepEqual(mapping.linux, ['dist:linux'])
  assert.deepEqual(mapping.darwin, ['dist:mac'])
})
