# Security Policy

## Supported versions

Security fixes go into the latest release. Please update to it before
reporting.

| Version | Supported |
|---|---|
| 0.2.x (latest) | yes |
| < 0.2 | no |

## Reporting a vulnerability

**Please do not open a public issue for security problems.**

Report them privately through GitHub:
**[Report a vulnerability](https://github.com/Mahadi1000/ainoiceguard/security/advisories/new)**
(repository → *Security* tab → *Report a vulnerability*).

Please include:

- what the problem is and what an attacker could do with it;
- the affected version and OS;
- steps or a proof of concept to reproduce it.

You can expect an acknowledgement within **7 days**. We will keep you updated
while we work on a fix, agree on a disclosure date with you, and credit you in
the release notes unless you prefer not to be named.

## Things that are in scope

- the Electron app (IPC between renderer and main, `preload.js`, loading of
  remote content, settings and log files);
- the native addon (memory safety, handling of device names and other input);
- the installers and the build/release scripts and workflows;
- anything that could expose a user's microphone audio.

## Download safely

Only download Ainoiceguard from the
[GitHub Releases page](https://github.com/Mahadi1000/ainoiceguard/releases).
The installers are not code-signed yet, so Windows SmartScreen may show a
warning. If you build from source, review changes to `scripts/` and
`.github/workflows/` before running them.
