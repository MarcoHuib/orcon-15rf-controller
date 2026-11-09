# Heltec V2 TX-PoC — phase 1, dry-run only

Baseline: `8e1821b316ddb5168ffa228817c459ec4f4d4405` on
`feature/heltec-v2-rx-only`. Phase 1 is local development only: no Actions,
Actions artifacts, push/PR, flash/reset, serial connection or RF test.

## Capabilities and boundaries

| Environment | Legacy RX_ONLY | PoC | RF |
| --- | --- | --- | --- |
| `heltec_v2_sx1276` | 1 by default | 0 | no application TX path |
| `heltec_v2_sx1276_tx_poc_dryrun` | 1 explicitly | 1 | no hardware TX implementation |
| `heltec_v2_sx1276_tx_poc` | 1 explicitly | 1 | intentionally fails compilation |

`tx_poc_caps.h` rejects RF enablement, invalid capability values, wrong/multiple
radio backends, an unlocked legacy command layer and diagnostic pin-test builds.
Removing the RF compile error alone cannot implement transmission: there is no
SX1276 TX backend in this PoC. No web endpoint can arm RF.

The existing project-wide default environment is still the V3/CC1101 build.
It has not been silently changed. The V2 default remains RX-only. Always select
an explicit environment or use the local runner below.

The old `sendFrame()` refusal, `rx_only.h` allowlist, Orcon command refusals,
`orcon::tick()` guards, OTA/update guards and original workflow guards remain.
`orcon_frames.cpp`, `orcon.cpp`, `ramses_codec.cpp` and the RX stream implementation
are unchanged. The existing `07` frame variant is unchanged.

## Ownership and transaction model

Main loop: authentication, request validation, admission and status/correlation.
The existing `ramses_rx` owner task alone services the PoC owner state machine.
Two statically allocated SPSC mailboxes carry one copied request and one copied
completion. No lock is held across scheduling; there is no spinlock, dynamic
queue allocation, second radio task or second producer. The task does one bounded
state-machine step per existing pass, after its normal RX work.

Owner model:

```
Idle -> Prepare -> Simulate -> Cleanup -> Restore -> Publish -> Idle
                                  |          |
                                  |          +-> Recover (at most two attempts)
                                  +-----------------------------^
```

All preparation/start/completion failures converge on cleanup. Fast restore is
tried first, then a bounded full-reinitialization *model*. The phase 1 backend
only encodes bytes and observes RX health: it never changes radio modes, UART,
SPI, DIO mapping, decoder buffers, or the existing RX recovery budget.
Therefore it needs no decoder reset/boundary change. `rx_restored` in this build
means the dry-run observation succeeded, not that a real TX/RX transition was
measured. Real cleanup/reinitialization code is deferred to the hardware phase.

Queue lifetime is 500 ms, owner work deadline 100 ms, recovery spacing 20 ms,
controller deadline 1000 ms. These are software/model limits, not validated RF
timing. Owner timeout retains the reservation until completion/cleanup returns;
it cannot admit a second overlapping request. Cleanup failure, exhausted restore
or owner timeout locks further admission until a future boot. No automatic retry
or background command is generated.

There is one open transaction, including any future confirmation window. One
accepted action builds exactly one `22F1` with `00 NN 04`, NN = 1, 2 or 3. IDs
must be valid, nonzero, nonbroadcast and different. The copied request pins both
IDs. The phase 1 image never writes or generates identity keys. Only fully valid,
existing NVS identities are accepted; absent/incomplete identities fail closed.

Sequences are monotonic per boot. Only the current expected request number is
accepted. Repeating the latest identical request returns its status without
queuing again; a changed body/identity conflicts. Older requests and skipped
sequences are refused, never replayed. Sequence exhaustion fails closed. A global
10-second interval applies between newly accepted requests, with no accumulated
burst allowance.

## Web security and truthful status

The PoC serves a dedicated page at `/`, authenticated state at
`GET /api/tx-poc/state`, commands at `POST /api/tx-poc/mode` and the authenticated
local OLED setting. Other routes are refused, including legacy commands, IDs,
WiFi/setup configuration, logs and firmware updates. MQTT is disabled at compile
time regardless of saved broker settings. Existing saved settings are not erased.

Authentication uses a random 128-bit per-boot bearer secret, shown only on an
awake OLED during the first 60 seconds and hidden after authentication. Existing
OLED wake/power policies and the PRG button role remain. The secret is not sent to
serial, stored in NVS, placed in HTML/URLs, or persisted in browser storage.
The boot session expires after ten minutes; there is no unauthenticated credential
issuance or network rearm endpoint. Authentication failures have a bounded global
rate. Credentials and CSRF values are separate.

POST requires the bearer, CSRF header, exact JSON content type, expected Host,
matching Origin and a connected STA-only network mode. Setup-AP access is refused.
JSON is bounded to 96 bytes and exactly two unique literal keys, integer `mode`
and nonzero integer `request`. Duplicates, coercions, escapes, overflow and trailing
input are rejected. Responses are not cached; the dedicated page cannot be framed
and loads no external assets. There is no automatic POST retry. An explicit retry
retains the exact idempotency body after a network error.

HTTP is not encrypted. This dry-run portal is suitable only for an isolated,
trusted local test network. A session token and CSRF do not protect an untrusted
LAN against sniffing or active traffic manipulation. RF enablement remains a
separate NO-GO until transport security/network isolation and a physical or local
one-shot RF authorization gate are reviewed and implemented.

`202` means accepted. `dry-run-complete` means software simulation completed;
`rf_sent`, `rf_capable`, `rf_armed` and `unit_confirmed` remain false. Physical
remote activity cannot turn a dry-run into unit confirmation.

Future correlation is model-tested independently: only a matching `I`/`RP` `31D9`
from the pinned unit after send/restore and before the ten-second deadline may
produce `unit-reported-mode`. An old queued frame, neighbour, unrelated response
or different mode is not confirmation. Observed overlapping remote control makes
the result ambiguous. The model never uses legacy `txAckMs`.

`lastReceivedMs()` carries the existing decoder sync-observation timestamp from
the owner through the RX queue. This avoids using main-loop dequeue time. It is
not a UART ISR or RF-air timestamp: UART backlog/timing must be evaluated before
real RF correlation is released. Matching unit status is evidence of observed
mode, not cryptographic proof of causality or measured fan speed.

## Local verification

From this checkout, with PowerShell 7, C++ compiler, Python 3 and Node installed:

```powershell
pwsh -NoProfile -File tools/test_local.ps1 -OutputDirectory build/tx-poc-local -Firmware
pwsh -NoProfile -File tools/test_local.ps1 -OutputDirectory build/tx-poc-ubsan -Sanitizer Undefined
pwsh -NoProfile -File tools/test_local.ps1 -OutputDirectory build/tx-poc-asan -Sanitizer AddressUndefined
pwsh -NoProfile -File tools/test_local.ps1 -OutputDirectory build/tx-poc-tsan -Sanitizer Thread
```

`-Compiler` accepts an alternate compiler executable without changing the system
compiler or security settings. A broken sanitizer records a failing execution
exit code and subsequent suites as `NOT TESTED`; it never counts as PASS. See the
existing sanitizer note in `HELTEC_V2_OLED.md`. No security/quarantine bypass is
part of these commands. PlatformIO package/tool versions are recorded rather than
assuming the old historical Actions toolchain is still installed.

The runner covers the original nine C++ suites, new golden vectors/transaction/
security/concurrency/fault tests, original source guards executed locally,
negative capability compiles, privacy including new files, actual emitted page
scripts and OLED regressions. `-Firmware` builds V2 RX, V2 dry-run, V3/CC1101,
V3/SX1262 and 4848 locally. It only invokes `pio run -e <allowlisted environment>`;
there is no upload target or hardware tooling.

`RESULTS.json` records commit, dirty state, commands and exit codes;
`SOURCE_HASHES.json` fingerprints source. Artifacts have separate environment
directories, full commit stamps, image checks and SHA-256 manifests. No merged
flash image is produced. Keep results local; paths/tool logs are not public
hardware evidence. Logs in ignored `build/` survive a normal local review, but are
not a backup. Existing Actions are still enabled in repository configuration;
the existing RX-branch push trigger and workflow files were not changed. No push
is allowed until the owner separately guarantees that it cannot start Actions.

## Gates before any hardware/RF work

Hardware boot, web/OLED behaviour, stack/heap, FreeRTOS timing, RX continuity,
actual radio recovery, power/spectrum and 24-hour stability are **NOT TESTED** by
host tests or compilation. No new 24-hour RX test is required before reviewing
phase 1. A later hardware step still needs separate permission to flash/reset.

The future packet-mode implementation must fit the 64-byte FIFO. This three-byte,
two-address command yields 14 logical bytes, 34 wire bytes and a 50-byte/400-bit
buffer with six UART-framed software preamble bytes. Test the entire on-air result:
disable hardware sync, CRC, whitening/encoding and variable-length/address fields;
account explicitly for hardware versus software preamble. Fast direct-RX restore
must restore DIO mapping and altered modem settings. Use full `beginFSK()` only
as a bounded fallback. Neither path is implemented or timed on hardware here.

Confirm acceptance of the unchanged NVS source identity without automatic pairing
or cloning. Verify the target against the owner's own physical remote. Require
one separately approved command, independent RF observation (SDR/second receiver),
measured TX duration and RX return, and a local RX-only rollback image preserving
NVS and partition layout. No RF approval is implied by this implementation.

## Independent Claude review proposal

Review the local diff against the baseline, without editing, pushing, building
through Actions or communicating with hardware. Prioritize: absence of a hardware
TX path; RX_ONLY/default/build isolation; SPSC memory ordering and reservation
lifetime; cleanup/fault latches; auth, expiry, CSRF/Host/Origin and disabled legacy
routes; NVS identity immutability; exact `04` golden vectors; receive-time versus
queue-time correlation; privacy and truthful UI states. Re-run local tests with
separate evidence directories. Report findings by severity and separate software
proof from deferred RF/FreeRTOS timing and identity-acceptance assumptions.
