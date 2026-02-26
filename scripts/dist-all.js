#!/usr/bin/env node
const { platform } = require('node:process')
const { spawnSync } = require('node:child_process')

const byPlatform = {
  win32: ['dist:win'],
  linux: ['dist:linux'],
  darwin: ['dist:mac']
}

module.exports = { byPlatform }

if (require.main === module) {
  const planned = byPlatform[platform]
  if (!planned) {
    console.error(`Unsupported host platform: ${platform}`)
    process.exit(1)
  }

  console.log(`Host platform: ${platform}`)
  console.log(`Running compatible packaging script(s): ${planned.join(', ')}`)
  console.log('For full multi-OS artifacts, run a CI matrix (Windows + Linux + macOS).')

  for (const scriptName of planned) {
    const result = spawnSync('npm', ['run', scriptName], { stdio: 'inherit', shell: true })
    if (result.status !== 0) {
      process.exit(result.status || 1)
    }
  }
}
