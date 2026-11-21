#!/usr/bin/env node
/**
 * Prints the release notes for one version: its section of CHANGELOG.md plus
 * install notes. Used by the CI release job (body of the GitHub Release).
 *
 *   node scripts/release-notes.js v0.2.0 > notes.md
 *
 * Exits 1 if CHANGELOG.md has no section for that version, so a release is
 * never published without notes.
 */
const fs = require("fs");
const path = require("path");

/** The body of "## [x.y.z] ..." up to the next "## " heading, or null. */
function changelogSection(changelog, version) {
  const v = String(version).replace(/^v/, "");
  const lines = changelog.split(/\r?\n/);
  const start = lines.findIndex((l) => l.startsWith(`## [${v}]`));
  if (start < 0) return null;
  let end = lines.findIndex((l, i) => i > start && /^## /.test(l));
  if (end < 0) end = lines.length;
  /* Drop link-reference lines ("[0.2.0]: https://...") at the end of the file. */
  const body = lines
    .slice(start + 1, end)
    .filter((l) => !/^\[[^\]]+\]: /.test(l))
    .join("\n")
    .trim();
  return body || null;
}

const INSTALL_NOTES = `
---

### Install

- **Windows:** download \`Ainoiceguard Setup <version>.exe\` and run it. The installer
  is not code-signed yet, so SmartScreen may say "Windows protected your PC":
  choose **More info → Run anyway**.
- **Linux:** \`.AppImage\` (any distro), \`.deb\` (Debian/Ubuntu) or \`.tar.gz\`.
- **macOS:** \`.dmg\` (unsigned: right-click the app → **Open** the first time).

Then install [VB-Cable](https://vb-audio.com/Cable/) (free), set Ainoiceguard's
output to **CABLE Input**, and pick **CABLE Output** as the microphone in
Zoom / Discord / Meet. Full guide: [README](https://github.com/Mahadi1000/ainoiceguard#readme).
`;

if (require.main === module) {
  const version = process.argv[2];
  if (!version) {
    console.error("usage: node scripts/release-notes.js <version>");
    process.exit(2);
  }
  const changelog = fs.readFileSync(path.join(__dirname, "..", "CHANGELOG.md"), "utf8");
  const section = changelogSection(changelog, version);
  if (!section) {
    console.error(`CHANGELOG.md has no section for ${version}`);
    process.exit(1);
  }
  process.stdout.write(section + "\n" + INSTALL_NOTES);
}

module.exports = { changelogSection };
