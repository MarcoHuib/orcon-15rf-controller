# Independent review before remediation — 2026-11-09

Reviewed clean f04ae0fde1d83031103622581e36360e40017eb9, PR #3 open,
base b8418f73b1fd1d4ed833214e87b8ce9dcfa9ea2e, verified using GitHub metadata.
No hardware, serial, reset, RF or Actions access performed.

BLOCKER B1: verify/plan/flash rollback call ci_ok; missing historical packages invoke
fetch/gh run. Thus the active release path is not offline/Actions-independent.
MAJOR M1a: matching 31D9 settles immediately; a later-dequeued interfering remote
frame (even with an earlier receive timestamp) cannot downgrade the result.
MAJOR M1b: a changed unit status after admission but before send is ignored as a
precondition change; later repeats of that already-established mode report success.
MAJOR M2a: send/prepare failure followed by cleanup failure and successful restore
retains first error but drops cleanup failure, so the controller does not latch.
MAJOR I1: identityConflict uses a 12-entry evicting discovery table and last-seen
slots, not a sticky history. A heard source collision can be forgotten this boot.
All four confirmed by independent C++ probes on unmodified original sources:
build/review-regressions-original.log (4 failing assertions, binary exit 1).
MAJOR F1: verify checks image_info only for app, not bootloader. A checksum list
can omit members or contain duplicates; boot_app0's pinned digest is checked only
on packaging, not verification. A corrupt but 8KB boot_app0 can be accepted.
MAJOR F2: writes four regions including bootloader/partitions/otadata every update.
A power failure can damage otherwise working boot infrastructure. No check of
current boot selection for application-only operation.
MINOR T1: 250ms grace is not an upper bound on evidence processing; original test
explicitly accepts a timely report processed 20s late. Owner success consumed
before timeout detection also bypasses a main-loop stall deadline.
MINOR R1: ASan/TSan historical reports cannot establish race-freedom on ESP32.
The dry-run has no hardware restore implementation, so g_rec handoff is a future
contract, not independently hardware-proven integration.

Original A-D tests, atomic release/acquire SPSC structure, compile-time no-RF,
SX1276 RX/RSSI/watchdog/recovery source paths and unchanged baseline conditions
were inspected independently. Initial local firmware test attempt failed due to
sandbox writes in the global PlatformIO cache. A cloned workspace cache is used
for subsequent builds; no system permissions or security settings changed.

## Remediation on a separate local branch

`fix/codex-local-release-review` preserves all original commits. Changes:

- B1/F1/F2: offline-only verified packages and pinned historical rollback;
  complete checksum coverage; both ESP images validated; exact known bootloader,
  table and app0 selection; plaintext-update refusal on enabled/unknown security
  configuration; app-only write, frozen inputs and identical readback before boot.
- M1a/M1b/T1: keep the complete observation window open, enforce its exact
  dequeue grace boundary, reject changed pre-send state as ambiguous, and retain
  ambiguity on a third mode. Late result consumption enforces the controller
  deadline; owner cleanup/restore delays preserve first error and fail closed.
- M2a: independent cleanupFailed bit so a second failure still latches admission.
- I1: source collision latched for the boot, independent of discovery eviction.
- Test evidence now includes UBSan, passive-log tools, all five firmware builds,
  source stability and package/build/source manifest hashes.

The final, executed results, exit codes, clean source commit, tool versions,
firmware hashes and offline package/plan evidence are retained locally in
`build/codex-final/` and the sibling sanitizer evidence directories. The final
review report is `build/codex-final/REVIEW.md`. This source document deliberately
does not assert results for a commit that has not yet been built.

## Evidence limits and decisions

Original PR #3 at f04ae0f: **NO-GO for merge as reviewed**; incorporate the fixes
and review their final evidence first. No existing PR is merged or changed.
The reviewed local dry-run release can receive a **conditional GO for a controlled
USB attempt** once every local gate passes, with separate hardware consent and
mandatory on-board precheck. Actual hardware readiness is unverified here.
Actual RF transmission: **NO-GO** (compile-time blocked and no hardware TX backend).

RX-only firmware logic, SX1276 RSSI workaround, decoder, UART, watchdog and
existing g_rec recovery are unchanged. The only main-loop change passes the
clock to a PoC-only result-consumption call. New work runs on the existing owner;
RX-only excludes it. Baseline RX regression risk is low based on source isolation
and local builds/tests; PoC stack/timing, live WiFi/OLED and RX continuity remain
hardware gates. No source-level claim substitutes for measurements on ESP32.

NVS has no write/erase overlap with app0, including flash-sector rounding. Normal
WiFi/OLED operation can still update NVS. The preserved boot infrastructure makes
manual ROM USB recovery the intended path after an interrupted application write;
there is **no proven automatic rollback or guaranteed recovery**. The exact local
procedure and primary Espressif references are in [LOCAL_USB_RELEASE.md](LOCAL_USB_RELEASE.md).

Two further rollback probes failed before the final follow-up fix: direct
`precheck rollback` did not resolve its alias, and the frozen snapshot omitted
the unused merged image still listed in the historical checksum manifest.
Both are covered through simulated full precheck/write/readback/reset on the
real historical manifest; only the four input checksum rows enter the snapshot.
The synthetic log-tool tests use the installed pyserial interpreter and require
loopback-socket capability; their pseudo-terminals do not access physical serial.
