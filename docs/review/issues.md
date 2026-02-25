# Review Issues Tracker

Derived from the prior audit findings, converted into actionable issues with priority/status.

| ID | Type | Priority | Status | Summary |
|---|---|---:|---|---|
| ISSUE-001 | Bug / Emergency | P0 | Done | Windows native build script pinned to valid VS 2022 toolchain (removed invalid 2026/VS18 references). |
| ISSUE-002 | Bug | P1 | Done | Startup behavior aligned with tray-first UX (window stays hidden until user opens via tray). |
| ISSUE-003 | Bug | P1 | Done | Tray ON/OFF status now updates after start/stop IPC operations. |
| ISSUE-004 | Improvement | P1 | Done | Added automated tests and `npm test` entrypoint. |
| ISSUE-005 | Improvement | P2 | Done | `dist:all` now host-aware with explicit cross-OS guidance. |
| ISSUE-006 | Improvement | P2 | Done | Native-addon load failure hint now points to OS-appropriate build command. |
| ISSUE-007 | Improvement | P2 | Done | RNNoise dependency pinned to a fixed commit for reproducible builds. |
| ISSUE-008 | Improvement | P3 | Done | Processing-loop comment corrected to match actual sleep interval. |
| ISSUE-009 | Improvement | P3 | Done | Removed unused renderer state and centralized meter utility functions. |
| ISSUE-010 | Docs | P3 | Done | README wording updated for cross-platform support and packaging behavior. |

