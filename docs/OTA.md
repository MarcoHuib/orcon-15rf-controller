# Signed OTA and remote diagnostics (Heltec V2)

## Why this is safe on the existing layout
- **Partition table is unchanged.** It already has `otadata` plus two app slots, `app0`/`app1` of 0x330000 B each (`tools/check_parts.py`). A firmware image is about 1.1 MB.
- **The board's bootloader already does rollback.** It is the Arduino-ESP32 2.0.16 bootloader, built with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. Its `bootloader_utility_get_selected_boot_partition` writes otadata state 1 (`PENDING_VERIFY`) and state 4 (`ABORTED`). `heltec_v2.sh precheck` already requires that exact bootloader. No bootloader or partition-table write is needed.
- **Interrupted upload.** The boot partition only changes after the complete image has passed every check, so an interrupted upload leaves the running image selected.
- **Trial boot.** A new image boots once as `PENDING_VERIFY`. `ota::tick` confirms it only when all three hold:
  - it has run at least 60 s;
  - WiFi is connected;
  - the receiver is up.
- **Automatic rollback.** If the new image is not healthy within 300 s, or resets before it confirms (crash, watchdog, power loss), the bootloader returns to the previous image.
  - At 300 s the image marks itself invalid (`esp_ota_mark_app_invalid_rollback_and_reboot`). That call refuses when otadata holds no usable previous entry. The image then restarts anyway, and the bootloader marks the unconfirmed image `ABORTED` and starts the other slot. A trial boot therefore never stays running unhealthy.
  - After the USB bootstrap, otadata is the pinned `boot_app0.bin`. Its entry 0 (seq 1, app0) is valid, so the first OTA can roll back to app0 through either path.
- **The trial boot depends on one linker detail.** `verifyRollbackLater()` must be defined `extern "C"`. Otherwise the Arduino core's weak default stays linked, and `initArduino()` confirms every new image at boot, so there is no trial boot and no rollback. The evidence row `rollback-hook-<env>` checks the linked ELF. `orcon.py` treats an image that is already valid when first seen as a failure (`NO TRIAL BOOT`).
- **Rollback test.** `POST /api/ota/hold` works only during a trial boot. It stops that boot from confirming itself, so the 300 s path runs on real hardware. It can only make an update fail; it never installs anything. It needs the owner's logged-in session (Codex review of PR #10, SEC-01).
- **Owner consent (ownership-model firmware, `docs/PROVISIONING.md`).** The release key proves an image is official; it does not authorise installing it. Nonce, begin, upload and hold need the owner's session with its CSRF token. Two manifest forms exist:
  - v1, targeted: this board plus a nonce. The CLI path below.
  - v2, release: one signature for every board, never older than the running release, never the RF test image. The web page installs `.orconfw` bundles made by `orcon.py release`.
  Every image must also carry `ORCON-OWNER-MODEL v1`, so firmware from before per-device owner keys cannot come back over the air.
- **Data that survives.** NVS (WiFi, profiles, identities, OLED, MQTT) is never written by an update.

## What the device accepts (`src/ota_signed.cpp`, rules in `src/ota_policy.cpp`)
1. `POST /api/ota/nonce` returns a one-time nonce, valid for 5 min. Each `begin` consumes it, whether it succeeds or not, so an old manifest cannot be replayed or used to downgrade.
2. `POST /api/ota/begin` takes `{"manifest": "<json>", "sig": "<DER hex>"}`.
   - **Signature:** ECDSA P-256 over the manifest text, verified against `src/ota_pubkey.h`. This is real authentication; an Origin header or token is not enough. The private key exists only in `~/.orcon/ota-signing-key.pem` (0600).
   - **Manifest:**
     - this board's MAC;
     - the issued nonce;
     - a size that fits the inactive slot;
     - an approved variant:
       - `rx-only` = `heltec_v2_sx1276` with `allow_tx:false`;
       - `rf-test` = `heltec_v2_sx1276_rftest` with `allow_tx:true`;
       - `control` = `heltec_v2_sx1276_control` (daily use, see `docs/CONTROL.md`) with `allow_tx:true`.

   Anything else is refused.
3. `POST /api/ota/upload` (multipart) streams the image into the inactive slot with `esp_ota_*`. The legacy Update/ArduinoOTA code is not used. While streaming, the device hashes the image and scans it. It writes nothing to the boot partition unless all of these hold:
   - size and SHA-256 equal the manifest;
   - the commit is stamped in the image;
   - the image carries `ORCON-SIGNED-OTA v1` (an update can never remove the WiFi update path);
   - there are no unauthenticated-OTA strings;
   - the variant markers match:
     - RX-only images contain no `txpoc packet TX`;
     - RF test images carry the capped `RF TEST: max N packets per boot` marker;
     - control images carry `CONTROL: max 10/min, 60/h` and neither of the other two markers;
   - ESP-IDF's own image check passes.

   Only then does the device set the boot partition and restart.
4. **What this prevents.** RX-only can never silently become RF-TX:
   - the variant is signed and matched against the image contents;
   - the CLI demands the same phrase as the USB path: `"RF test image"` for the test image, `"control firmware"` for the control image (`--tx-ack`).

**Side effect of the flash erase:** the receiver can miss frames while the slot is written.

## Remote diagnostics
- `GET /api/setup/console?after=N`: the RF console. It has per-boot sequence numbers, `lost` and `more`, and now includes a line per finished TX transaction (sent, error, RX restore result).
- `GET /api/setup/capture`: the last 8 complete `1FC9`/`10E0` frames.
- `GET /api/diag`:
  - reset reason, boot count and previous uptime (RTC memory: no flash wear, lost only on power loss);
  - heap and RX counters;
  - OTA slot state;
  - a crash summary from the existing coredump partition (task, PC, backtrace), written only on a crash.
- `GET /api/ota`: firmware, running/boot/next slot and their OTA states, last refusal.

## From the Mac: `tools/orcon.py`
- `status`, `capture`.
- `logs --minutes N` writes to `captures/sessions/<UTC>/` (git-ignored, 0700/0600):
  - `console.jsonl`;
  - `gaps.jsonl`, which records missing sequence numbers, board-side loss, restarts and lost links;
  - `capture-*.json` and `diag.jsonl`.
- `ota-install --worktree <release worktree> --env <env> --yes [--rf-ack "RF test image"]`:
  1. runs the same `tools/heltec_v2.sh verify` as the USB path (evidence, hashes, markers);
  2. signs a manifest for this board and uploads it;
  3. watches the trial boot and prints `CONFIRMED` or `ROLLED BACK`;
  4. appends the image hash to `~/.orcon/installed.txt` (0600), which `heltec_v2.sh recover` uses to identify slots.
- `ota-install ... --test-rollback`: the same upload, then `POST /api/ota/hold` in the trial boot. It passes only if the previous image returns. The CLI asks for the web password and logs in inside the trial boot.
- On ownership-model firmware (`owner_model` in `/api/ota`) `ota-install` asks for the web password and sends the session with every step. Older firmware installs exactly as before.
- `release --worktree <wt> --env <env> --version N --yes`: a signed v2 bundle for every board (`docs/RELEASES.md`).
- **Host.** Signed-OTA builds answer on `orcon15rf.local` (mDNS). An IP address in `~/.orcon/config.json` also works.

## USB is still needed once
The firmware on the board (`ae36e42`) has no signed OTA receiver. A single USB flash of an image that contains it, using the existing app0-only procedure, is required. After that, these steps run over WiFi:
- updates;
- RF test images (with the phrase);
- logs, captures, diagnostics and crash summaries;
- restarts.

## Limits
- **Two cases still need USB:**
  - a new image that is healthy by the check above but whose OTA receiver is broken;
  - a broken bootloader.
- **The USB precheck refuses a board after its first OTA, by design.** It expects otadata to select app0 and app1 to be empty or the known state. Use the OTA-aware recovery instead (next section).

## USB recovery after OTA: `heltec_v2.sh recover`
`LOCAL_ENV=heltec_v2_sx1276 tools/heltec_v2.sh recover check|write <commit>`, from the release worktree. It needs separate consent (it opens the port and resets the board).

**Both modes check, read-only:**
- chip, MAC pin, 8 MB, no flash encryption or secure boot;
- the exact bootloader and partition table.

**`tools/check_slots.py` then decodes the state the way the bootloader does:**
- both otadata entries (sequence, state, CRC) and the slot they select;
- each slot identified by the SHA-256 of exactly its image bytes. The length comes from the ESP image header.

**A slot must be one of:**
- erased;
- the pinned known app1 board state;
- a local package image;
- an image listed in `~/.orcon/installed.txt`.

Anything else is NO-GO and nothing is written. So is a selected slot without a known image.

**`write`, after typing `recover`:**
1. app0 gets the verified RX-only package, with readback;
2. only then is otadata set to the pinned `boot_app0.bin` (app0 selected), with readback;
3. reset.

Bootloader, partition table, NVS and app1 are not written.

**The write order survives interruption:**
- during the app0 write, otadata still selects the old, known slot (or the bootloader falls back to the known image in app1);
- during the otadata write, the bootloader falls back to app0, which has already been verified.

**When recovery refuses:** a slot left half-written by an aborted upload is unknown content, and `recover` refuses it. The board itself still runs (the boot slot did not change), so repeat the OTA over WiFi first.
- **What rollback falls back to.** Rollback returns to whatever image ran before, including an approved RF test image if that was the previous one.
