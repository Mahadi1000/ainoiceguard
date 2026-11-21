<!-- Thanks for contributing! Please read CONTRIBUTING.md first. -->

## What does this change?

<!-- A short summary. Link the issue: "Closes #123". -->

## Why?

<!-- The problem it solves, or the motivation. -->

## How was it tested?

<!-- OS, microphone, output device / VB-Cable, and what you checked.
     For audio changes, add before/after numbers (bench/, ng_loopback,
     engine-sessions.jsonl) — see "Audio changes need numbers" in CONTRIBUTING.md. -->

## Checklist

- [ ] `npm test` passes
- [ ] The app builds and starts (`npm run build:native` / `build:native:unix`, `npm run rebuild:electron`, `npm start`)
- [ ] No blocking calls (allocation, locks, I/O) added to the audio callbacks or processing thread
- [ ] Tests added or updated for new logic
- [ ] Docs and `CHANGELOG.md` (**Unreleased**) updated if behaviour changed
- [ ] Commits follow Conventional Commits (`feat:`, `fix:`, ...)
