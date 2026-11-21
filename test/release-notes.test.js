const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("fs");
const path = require("path");
const { changelogSection } = require("../scripts/release-notes");

const SAMPLE = `# Changelog

## [Unreleased]

## [1.1.0] - 2026-01-02
### Added
- thing

## [1.0.0] - 2026-01-01
### Fixed
- bug

[1.1.0]: https://example.invalid/compare/v1.0.0...v1.1.0
[1.0.0]: https://example.invalid/releases/tag/v1.0.0
`;

test("changelogSection returns one version's body", () => {
  assert.equal(changelogSection(SAMPLE, "v1.1.0"), "### Added\n- thing");
  assert.equal(changelogSection(SAMPLE, "1.0.0"), "### Fixed\n- bug"); /* no link refs */
  assert.equal(changelogSection(SAMPLE, "2.0.0"), null);
  assert.equal(changelogSection(SAMPLE, "Unreleased"), null); /* empty section */
});

test("CHANGELOG.md has notes for the package.json version", () => {
  const root = path.join(__dirname, "..");
  const { version } = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
  const notes = changelogSection(fs.readFileSync(path.join(root, "CHANGELOG.md"), "utf8"), version);
  assert.ok(notes && notes.length > 50, `CHANGELOG.md needs a section for ${version}`);
});
