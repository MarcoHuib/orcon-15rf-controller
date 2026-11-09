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
  |        |                                            ^
  +--------+-- expired / RX unhealthy / deadline -------+   (radio untouched)
```

Radio work starts at `prepare()` (`touched`). Only touched transactions run
cleanup and one fast restore; an expired request or unhealthy RX before that
publishes directly, with nothing to clean up. The **first** error is kept; a
later restore failure never overwrites it. The owner has no recovery loop of its
own: a hardware backend's failed fast restore must mark the radio not-ready
(`g_rxReady=false`) so the existing bounded `g_rec` recovery restarts it. The
phase 1 backend only encodes bytes and observes RX health: it never changes
radio modes, UART, SPI, DIO mapping, decoder buffers, or the existing RX
recovery budget. `rx_restored` in this build means the dry-run observation
succeeded, not that a real TX/RX transition was measured.

RX readiness crosses tasks only as one atomic word published by the radio owner
on every pass (`packHealth`: heartbeat ms, bit 0 = healthy; release store,
acquire load before the clock read). The web layer treats a heartbeat older than
250 ms, or never published, as not ready. Health is `g_rxReady` plus the RX
opmode from the existing 1 s register snapshot, so it can lag by up to 1 s.

Queue lifetime is 500 ms, owner work deadline 100 ms, controller deadline 1000 ms. These are software/model limits, not validated RF
timing. Owner timeout retains the reservation until completion/cleanup returns;
it cannot admit a second overlapping request. Cleanup failure, failed restore
after radio work, or owner timeout locks further admission until a future boot.
An untouched failure (expired, RX unhealthy) does not lock; admission stays
refused anyway while RX is not ready. No automatic retry
or background command is generated.

There is one open transaction, including any future confirmation window. One
accepted action builds exactly one `22F1` with `00 NN 04`, NN = 1, 2 or 3. IDs
must be valid, nonzero, nonbroadcast and different. The copied request pins both
IDs. The phase 1 image never writes or generates identity keys. Only fully valid,
existing NVS identities are accepted; absent/incomplete identities fail closed.

Admission also requires, in this order after the request checks: fresh healthy
RX, a source ID that no heard sender uses, a physical remote for the selected
unit heard this boot, the 10 s rate limit, a unit mode reported by the selected
unit within the last 10 minutes and different from the request, and no remote
action for the unit in the last 3 s. The prior mode is pinned in the request.

Identity (software-checkable part): the source ID is refused if it equals any
sender in `rlog` (all devices heard, the last remote and the last unit) or the
sender of a remote command to the selected unit. Without such a remote heard
this boot the identity is unverified and refused, so a clone of an unheard
remote cannot pass silently. Not checkable in software: devices that never
transmitted while we listened, and whether the unit accepts this source ID at
all. Both need real RF. IDs are never generated, changed, cloned or paired.

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

Header validation is a pure, host-tested function: `Host` must be exactly the
station IP (optionally `:80`; no DNS names, no prefix matches), `Origin` exactly
`http://` + that host, `Content-Type` exactly `application/json`. Missing
headers count as empty and fail.

Large bodies (pre-existing, all builds): the Arduino-ESP32 2.0.16 `WebServer`
reads a non-form POST body up to `Content-Length` into heap before any handler
or authentication runs, on every route including unknown ones, and waits up to
5 s for each stalled chunk. A LAN client can therefore cause transient heap
pressure or stall the main loop (web, OLED, controller ticks). It cannot cause
TX: the owner runs in the RX task, never waits for the loop, and the controller
fails closed on timeouts. Fixing this means replacing the web server, so it is
not changed here; the isolated-network requirement below covers it.

HTTP is not encrypted. This dry-run portal is suitable only for an isolated,
trusted local test network. A session token and CSRF do not protect an untrusted
LAN against sniffing or active traffic manipulation. RF enablement remains a
separate NO-GO until transport security/network isolation and a physical or local
one-shot RF authorization gate are reviewed and implemented.

`202` means accepted. `dry-run-complete` means software simulation completed;
`rf_sent`, `rf_capable`, `rf_armed` and `unit_confirmed` remain false. Physical
remote activity cannot turn a dry-run into unit confirmation.

Future correlation is model-tested independently (only reachable once a backend
sets `rf_sent`). The window is 3 s after the send: the unit answered 16/17
remote presses within 300 ms and repeats about 1 s later (HELTEC_V2_RX.md,
measurement 2). Only an `I`/`RP` `31D9` with domain byte `00` from the pinned
unit, received after send and restore and before the deadline, counts:

| Observation in the window | Outcome |
| --- | --- |
| Reports the requested mode | `unit-reported-mode` |
| Reports the pinned prior mode | `unchanged` |
| Reports a third mode | `ambiguous` |
| `22F1`/`22F3` to the unit (address slot 1 or 2, `orcon::toUnit` rules) from 3 s before the send onward | `ambiguous` |
| Nothing by deadline + 250 ms loop grace | `not-confirmed` |

Frames without a receive time, or with one later than the loop clock, are never
evidence. An untimed remote command is counted as happening now (interference).
Old queued frames and neighbours are ignored. `unit_confirmed` stays false: a
matching report is an observation, not proof that this transmission caused it.
The model never uses legacy `txAckMs`.

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

The runner covers the original nine C++ suites (also `test/run_host_tests.sh`,
which now runs `tx_poc` too), new golden vectors/transaction/
security/concurrency/fault tests, original source guards executed locally,
negative capability compiles, privacy including new files, actual emitted page
scripts, OLED regressions and, with `-Firmware`, the flash tooling on simulated
boards (`tools/test_precheck.sh`). `-Firmware` builds V2 RX, V2 dry-run, V3/CC1101,
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

## Local USB flashing (no Actions)

`tools/heltec_v2.sh` keeps its board checks and adds a local artifact source.
Nothing below is run by the PoC tooling itself; every write needs the owner's
consent for that specific commit.

1. Build and test locally on a clean, committed tree:
   `pwsh -NoProfile -File tools/test_local.ps1 -OutputDirectory build/<name> -Firmware`
2. Package one image: `tools/heltec_v2.sh package build/<name> heltec_v2_sx1276_tx_poc_dryrun`
   (or `heltec_v2_sx1276`). Only these two environments are accepted. The package
   lands in `.flashtools/local/<commit>-<env>/` with `SHA256SUMS.txt` and
   `BUILD_INFO.txt` and is verified at once.
3. Offline dry run: `LOCAL_ENV=<env> tools/heltec_v2.sh plan <commit>` verifies
   everything below and prints the exact `esptool write_flash` command. It opens
   no port.
4. With the board connected: `tools/heltec_v2.sh ports` lists USB serial ports
   (`/dev/cu.usbserial-*`, `cu.SLAB_USBtoUART*`, `cu.wchusbserial*`) without
   opening them. Exactly one is used automatically; with several, set `PORT=`.
5. `LOCAL_ENV=<env> tools/heltec_v2.sh precheck <commit>`, then `... flash <commit>`.
   `flash` repeats the precheck, prints the command and writes only after `yes`.
   Note: `precheck` opens the port, which resets the ESP32 into its ROM loader
   (DTR/RTS); that already interrupts RX and needs the same consent as a flash.

Verification replaces the Actions check with local evidence: `RESULTS.json` of
the same commit, `dirty` empty, every row PASS (a `NOT TESTED` row also blocks),
the required suites present, the firmware/image/partition rows for the env, and
image hashes equal to those recorded by the runner. `RESULTS.json` is hashed at
packaging; any later edit blocks. Image checks: commit stamp, SX1276 RX-only
backend string, no OTA/HTTP-update code, the env's marker (`RX-ONLY TEST
FIRMWARE` without PoC, or the dry-run markers), ESP32 chip ID, valid image hash,
8 MB headers, and the exact partition layout (`tools/check_parts.py`).
`boot_app0.bin` is pinned by SHA-256 (identical to the historical CI image).
Every image must fit its slot, so nothing reaches NVS: bootloader ≤ 0x7000,
partition table ≤ 0xC00, `boot_app0` exactly 8 KB, app ≤ 0x330000. Bootloader and
app must be ESP images (`0xE9`) with flash header bytes `02 30` (DIO, 8 MB,
40 MHz), as the image now on the board. Measured on this machine: the local
bootloader and both local partition tables are byte-identical to the `d5c39e1`
Actions image the board runs.

Board precheck (read-only on flash): ESP32-D0WDQ6, the expected MAC
(`HELTEC_MAC` or `.flashtools/board.mac`), exactly 8 MB flash, the board's
partition table at 0x8000 (0xC00 bytes) byte-identical to the new one, and the
board's bootloader header at 0x1000 with the same flash metadata (`02 30`).
Nothing ever erases the whole flash: there is no `erase_flash`, no `--erase-all`,
and esptool only erases the sectors of the four images it writes.

| Image | Offset |
| --- | --- |
| `bootloader.bin` | 0x1000 |
| `partitions.bin` | 0x8000 |
| `boot_app0.bin` (otadata init) | 0xE000 |
| `firmware.bin` | 0x10000 |

Partition table (default 8 MB, espressif32@6.7.0): nvs 0x9000/0x5000,
otadata 0xE000/0x2000, app0 0x10000/0x330000, app1 0x340000/0x330000,
spiffs 0x670000/0x180000, coredump 0x7F0000/0x10000. No image covers
0x9000–0xDFFF, so NVS (Wi-Fi, IDs, settings) is kept; the merged image is never
used because it would erase NVS. esptool verifies each written region's hash.

Rollback to RX-only, either:
- `tools/heltec_v2.sh plan rollback`, then `tools/heltec_v2.sh flash rollback`:
  the image the board ran before the PoC (`d5c39e1`, historical Actions
  artifact, already in `.flashtools/fw/`). It only reads that commit's
  historical check-run status; it starts nothing. `rollback` refuses to run with
  `LOCAL_ENV` set, and an image carrying the TX-PoC markers fails the RX-only
  check; or
- a local RX-only build: `LOCAL_ENV=heltec_v2_sx1276 tools/heltec_v2.sh flash <commit>`.
Both keep NVS (Wi-Fi, MQTT, Orcon IDs, OLED setting) and the partition table:
the tables of the PoC and of `d5c39e1` are identical, so the precheck passes in
both directions. After any flash: `tools/heltec_v2.sh
monitor 40 boot.log reset` must show the expected `fw=` and `op=x5`.

Hardware-free tests (local, run by `test_local.ps1 -Firmware`):
`tools/test_precheck.sh` (board checks, board metadata, port missing, partition
tables, `local_ok`, image markers) and `tools/test_flash_local.sh` (argument
validation on the real script, port selection, invalid and mismatching
packages, layout, `plan` output, rollback selection, and flashing against a
simulated board: no write without `yes`, never to a wrong board, exactly one
`write_flash` with four regions and no erase). Both use the real script
functions with esptool and gh replaced by local stubs.

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
must restore DIO mapping and altered modem settings. Full re-initialisation
belongs to the existing `g_rec` recovery, not to the owner. Neither path is
implemented or timed on hardware here. A real TX spans several RX-task passes:
the existing 1 s "not in RX" opmode check must be suspended while the owner is
not idle, or it would count `rxLost` and restart the radio mid-transmission.

Confirm acceptance of the unchanged NVS source identity without automatic pairing
or cloning. Verify the target against the owner's own physical remote. Require
one separately approved command, independent RF observation (SDR/second receiver),
measured TX duration and RX return, and a local RX-only rollback image preserving
NVS and partition layout. No RF approval is implied by this implementation.

## Review status

The independent review of `b8418f7` (no BLOCKER) found two MAJOR and several
MINOR items. Addressed on `fix/heltec-v2-tx-poc-review`: false `31D9`
confirmations (A–D) and the correlation outcomes above; owner first-error,
untouched-path and single-recovery rules; atomic RX health with heartbeat;
source-ID conflict and unverified-identity refusal; header validation and
tests; the separate host runner; the local flash path. Accepted and documented:
global authentication rate limit (LAN lockout is denial of service only),
plain HTTP, and the web server's pre-authentication body buffering.
