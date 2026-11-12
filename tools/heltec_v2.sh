#!/usr/bin/env bash
# heltec_v2.sh — OFFLINE local packages and pinned historical RX-only rollback.
# package <evidence-dir> <env> : create and verify a local package
# LOCAL_ENV=<env> verify|plan <commit> : offline, no serial access
# verify|plan rollback : independently pinned, already-local historical RX-only image
# ports : list device names only (no serial access)
# LOCAL_ENV=<env> precheck|flash <commit> : HARDWARE ACCESS, separate consent required
# flash rollback : same hardware gates, local RX-only firmware
# LOCAL_ENV=heltec_v2_sx1276 recover check|write <commit> : HARDWARE ACCESS, OTA-aware USB recovery
#   (after WiFi OTA): exact bootloader/table, otadata and BOTH slots identified; write = app0 +
#   the pinned app0 selection in otadata, each with readback. Separate consent required.
# monitor <secs> [log] [reset] : HARDWARE ACCESS, separate consent required
# flash writes only app0 (recover: app0 + otadata). Bootloader, partition table, NVS and app1 stay intact.
# There is NO automatic/power-fail rollback. Interrupted app0 writes may require ROM USB repair.
# esptool/dependencies must already be installed locally; this script never downloads tools.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$REPO/.flashtools"
PORT="${PORT:-}"               # empty: the single USB serial port found in $DEV_DIR
DEV_DIR="${DEV_DIR:-/dev}"
ESPTOOL_SHA256="dc4ef26b659e1a8dcb019147c0ea6d94980b34de99fbe09121c7941c8b254531"
LOCAL_ENV="${LOCAL_ENV:-}"     # empty permits ONLY the pinned historical rollback
ORCON_INSTALLED="${ORCON_INSTALLED:-$HOME/.orcon/installed.txt}"   # images tools/orcon.py installed over WiFi
LOCAL_ENVS="heltec_v2_sx1276 heltec_v2_sx1276_tx_poc_dryrun"   # the only images that may be written
# The RF test image (capped packets per boot) is deliberately NOT in LOCAL_ENVS. package/verify/
# precheck/flash accept it only with ORCON_RF_TEST_ACK set to this exact phrase; flash asks again.
RF_TEST_ENV="heltec_v2_sx1276_rftest"
RF_TEST_ACK="RF test image"
env_ok() { [[ " $LOCAL_ENVS " == *" $1 "* ]] || { [ "$1" = "$RF_TEST_ENV" ] && [ "${ORCON_RF_TEST_ACK:-}" = "$RF_TEST_ACK" ]; }; }
# A local package needs these local checks PASS on the same clean commit (RESULTS.json).
ROLLBACK_COMMIT="d5c39e1b4d1f0ed37defe69b51e3562c36058208"  # immutable local trust anchor
# offset:image:slot size. NVS (0x9000-0xDFFF) is never written; the merged image is never used.
FLASH_MAP="0x1000:bootloader.bin:0x7000 0x8000:partitions.bin:0xc00 0xe000:boot_app0.bin:0x2000 0x10000:firmware.bin:0x330000"
FLASH_HDR="0230"   # image header bytes 2-3: DIO, 8 MB / 40 MHz (as the CI d5c39e1 image on the board)
LOCAL_REQUIRED="codec frames rx log orcon_rx orcon_v3 rxpipe netcfg oled tx_poc safety-and-negative-compiles privacy poc-web ubsan-host app1 elf-selftest rxmon rxmon-soak flash-tooling flash-local source-stability firmware-heltec_v2_sx1276 firmware-heltec_v2_sx1276_tx_poc_dryrun firmware-heltec_v3_cc1101_ota firmware-heltec_v3_1262_ota firmware-esp32-480480s040"

die() { echo "FAIL: $*" >&2; exit 1; }
keep() {   # keep <dir> <label>: readbacks of a FAILED verification stay local (.flashtools/ is git-ignored)
    [ -n "$(ls -A "$1" 2>/dev/null)" ] || { rm -rf "$1"; return 0; }
    local to; to="$TOOLS/evidence/$(date -u +%Y%m%dT%H%M%SZ)-$2-$$"
    mkdir -p "$TOOLS/evidence" && mv "$1" "$to" && ( cd "$to" && shasum -a 256 -- * > EVIDENCE_SHA256.txt ) \
        && echo "evidence kept: $to" >&2
}

bootstrap() {
    [ -x "$TOOLS/venv/bin/python" ] && [ -d "$TOOLS/esptool-4.8.1" ] \
        || die "offline esptool 4.8.1 installation missing; no automatic download"
    [ -f "$TOOLS/dl/esptool-4.8.1.tar.gz" ] || die "pinned local esptool archive missing"
    echo "$ESPTOOL_SHA256  $TOOLS/dl/esptool-4.8.1.tar.gz" | shasum -a 256 -c - >/dev/null \
        || die "esptool archive checksum mismatch"
    # Verify the actual Python sources we execute, independently of the install directory name.
    "$TOOLS/venv/bin/python" -I - "$TOOLS" <<'CHECK' || die "local esptool sources/dependencies invalid"
import importlib.metadata, pathlib, sys, tarfile
root = pathlib.Path(sys.argv[1])
with tarfile.open(root / 'dl/esptool-4.8.1.tar.gz') as archive:
    expected = set()
    for member in archive.getmembers():
        if member.isfile() and member.name.endswith('.py'):
            target = root / member.name
            assert target.read_bytes() == archive.extractfile(member).read(), member.name
            if '/esptool/' in member.name: expected.add(target.resolve())
    actual = {p.resolve() for p in (root / 'esptool-4.8.1/esptool').rglob('*.py')}
    assert actual == expected, 'unexpected esptool Python sources'
assert importlib.metadata.version('pyserial') == '3.5'
assert importlib.metadata.version('intelhex') == '2.3.0'
CHECK
}
esptool() { PYTHONPATH="$TOOLS/esptool-4.8.1" "$TOOLS/venv/bin/python" -m esptool "$@"; }

fulsha() { [ -n "${1:-}" ] || die "commit argument missing"
           git -C "$REPO" rev-parse --verify --quiet "$1^{commit}" || die "unknown commit '$1'"; }
target() {   # <commit>, or the pinned, already-local historical rollback (no network)
    if [ "${1:-}" = rollback ]; then
        [ -z "$LOCAL_ENV" ] || die "rollback is the pinned local historical RX-only image: unset LOCAL_ENV (a local RX-only package is flashed by commit)"
        fulsha "$ROLLBACK_COMMIT"
    else fulsha "${1:-}"; fi
}

ports() { ls "$DEV_DIR"/cu.usbserial-* "$DEV_DIR"/cu.SLAB_USBtoUART* "$DEV_DIR"/cu.wchusbserial* 2>/dev/null || true; }
pick_port() {   # PORT if set (must exist), else exactly one detected USB serial port
    if [ -n "$PORT" ]; then [ -e "$PORT" ] || die "serial port $PORT not found"; echo "$PORT"; return; fi
    local list n; list="$(ports)"; n="$(grep -c . <<<"$list" || true)"
    [ "$n" -ge 1 ] || die "no USB serial port found (connect the Heltec, or set PORT)"
    [ "$n" -eq 1 ] || die "several USB serial ports, choose one with PORT=: $(tr '\n' ' ' <<<"$list")"
    echo "$list"
}

layout_ok() {   # every image fits its slot (so nothing reaches NVS) and carries the board's flash header
    local d=$1 e off img max size
    for e in $FLASH_MAP; do
        IFS=: read -r off img max <<<"$e"
        [ -f "$d/$img" ] || die "$img missing"
        size="$(wc -c < "$d/$img" | tr -d ' ')"
        [ "$size" -gt 0 ] && [ "$size" -le $((max)) ] || die "$img ($size B) does not fit its $max slot at $off"
    done
    [ "$(wc -c < "$d/boot_app0.bin" | tr -d ' ')" -eq $((0x2000)) ] || die "boot_app0.bin is not 8 KB"
    for img in bootloader.bin firmware.bin; do
        [ "$(xxd -p -l 1 "$d/$img")" = e9 ] || die "$img is not an ESP image"
        [ "$(xxd -p -s 2 -l 2 "$d/$img")" = "$FLASH_HDR" ] || die "$img flash header is not DIO/8MB/40MHz"
    done
}
flash_args() {   # The only write operation: app0, with no reset until verified readback
    local d=$1
    FLASH_ARGS=(--chip esp32 --port "$2" --baud 460800 --after no_reset write_flash 0x10000 "$d/firmware.bin")
}

expected_mac() {   # the one board we may write to: HELTEC_MAC or .flashtools/board.mac
    local mac="${HELTEC_MAC:-$(cat "$TOOLS/board.mac" 2>/dev/null || true)}"
    mac="$(tr 'A-F' 'a-f' <<<"$mac" | tr -d '[:space:]')"
    [[ "$mac" =~ ^([0-9a-f]{2}:){5}[0-9a-f]{2}$ ]] || die "set the board MAC in HELTEC_MAC or $TOOLS/board.mac"
    echo "$mac"
}
local_ok() {   # local evidence: clean commit, every required check PASS, image hashes as recorded
    local d=$1 sha=$2 env ev
    env="$(sed -n 's/^env=//p' "$d/BUILD_INFO.txt")"; ev="$(sed -n 's/^evidence=//p' "$d/BUILD_INFO.txt")"
    [ -f "$ev/RESULTS.json" ] || die "local evidence $ev/RESULTS.json missing"
    grep -qx "evidence_sha256=$(shasum -a 256 "$ev/RESULTS.json" | cut -d' ' -f1)" "$d/BUILD_INFO.txt" \
        || die "local evidence changed since packaging"
    local name key
    for name in SOURCE_HASHES.json "artifacts/$env/BUILD_INFO.json"; do
        key=source_sha256; [ "$name" = SOURCE_HASHES.json ] || key=build_sha256
        [ -f "$ev/$name" ] || die "evidence $name missing"
        grep -qx "$key=$(shasum -a 256 "$ev/$name" | cut -d' ' -f1)" "$d/BUILD_INFO.txt" \
            || die "evidence $name changed since packaging"
    done
    python3 -I - "$ev" "$d" "$sha" "$env" $LOCAL_REQUIRED <<'PY' || die "local evidence does not cover this image"
import hashlib, json, sys
ev, d, sha, env, *required = sys.argv[1:]
r = json.load(open(f"{ev}/RESULTS.json"))
rows = {c["name"]: c["result"] for c in r["checks"]}
if len(rows) != len(r['checks']): raise ValueError('duplicate evidence checks')
if any(c.get('exit') != 0 for c in r['checks']): raise ValueError('evidence exit code missing/nonzero')
problems = []
if r.get("commit") != sha: problems.append(f"evidence commit {r.get('commit')} != {sha}")
if r.get("dirty") != "": problems.append("evidence from a dirty tree")
problems += [f"{n}: {v}" for n, v in rows.items() if v != "PASS"]
for n in required + [f"firmware-{env}", f"image-{env}", f"partitions-{env}", f"elf-{env}"]:
    if rows.get(n) != "PASS": problems.append(f"{n} not PASS")
recorded = {f["name"]: f["sha256"] for f in json.load(open(f"{ev}/artifacts/{env}/BUILD_INFO.json"))["files"]}
for n in ("bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin"):
    if hashlib.sha256(open(f"{d}/{n}", "rb").read()).hexdigest() != recorded.get(n): problems.append(f"{n} hash differs from evidence")
for p in problems: print("  " + p, file=sys.stderr)
sys.exit(1 if problems else 0)
PY
    echo "local evidence OK: $ev"
}

markers() {   # image contents per environment; network firmware update must be compiled out
    local f=$1 env=$2 s
    if [ "$env" = "$RF_TEST_ENV" ]; then
        env_ok "$env" || die "RF test image refused: ORCON_RF_TEST_ACK not given"
        grep -q -a -E "SX1276 direct RX \+ txpoc packet TX \(RF TEST: max [0-9]+ packets per boot\)" "$f" \
            || die "RF test marker missing (uncapped RF image?)"
    else
        grep -q -a "SX1276 direct RX (TX disabled)" "$f" || die "not the SX1276 RX-only backend"
    fi
    for s in "Firmware update (OTA)" "ArduinoOTA" "auth_upload" "Updater.cpp"; do
        if grep -q -a "$s" "$f"; then die "image still contains '$s' (OTA/HTTP update)"; fi
    done
    case "$env" in
        heltec_v2_sx1276)
            grep -q -a "RX-ONLY TEST FIRMWARE" "$f" || die "RX-only build marker missing"
            if grep -q -a "tx-poc-dryrun" "$f"; then die "RX-only image contains the TX-PoC"; fi ;;
        heltec_v2_sx1276_tx_poc_dryrun)
            grep -q -a "tx-poc-dryrun" "$f" && grep -q -a "DRY-RUN ONLY / NO RF" "$f" \
                || die "dry-run markers missing" ;;
        "$RF_TEST_ENV")
            if grep -q -a "tx-poc-dryrun" "$f"; then die "RF test image contains the dry-run backend"; fi ;;
        *) die "env $env may not be flashed" ;;
    esac
}

dist()   { if [ -n "$LOCAL_ENV" ]; then echo "$TOOLS/local/$(fulsha "$1")-$LOCAL_ENV"; else echo "$TOOLS/fw/$(fulsha "$1")"; fi; }

local_dist() {
    local sha=$1 d; d="$(dist "$sha")"
    [ -f "$d/BUILD_INFO.txt" ] || die "local package missing: $d (no download is attempted)"
    [ -n "$LOCAL_ENV" ] || [ "$sha" = "$ROLLBACK_COMMIT" ] || die "historical image is not the pinned rollback"
    echo "$d"
}

# Exact manifest coverage, regular files, and independently pinned infrastructure/rollback.
files_ok() {
    python3 -I "$REPO/tools/check_flash_package.py" "$1" "$2" || die "package integrity"
}

verify() {
    local sha; sha="$(target "${1:-}")"
    bootstrap
    local d; d="$(local_dist "$sha")"
    files_ok "$d" "$sha"
    layout_ok "$d"
    local env="${LOCAL_ENV:-heltec_v2_sx1276}"
    grep -qx "commit=$sha" "$d/BUILD_INFO.txt"        || die "artifact is not built from $sha"
    grep -qx "env=$env" "$d/BUILD_INFO.txt"           || die "wrong PlatformIO env"
    grep -q -a "$sha" "$d/firmware.bin"                || die "firmware.bin lacks commit stamp"
    markers "$d/firmware.bin" "$env"
    local info img
    for img in bootloader.bin firmware.bin; do
        info="$(esptool --chip esp32 image_info --version 2 "$d/$img")" || die "$img image_info failed"
        grep -q "Chip ID: 0 (ESP32)" <<<"$info" || die "$img is not for classic ESP32"
        grep -q "Validation hash: .* (valid)" <<<"$info" || die "$img image hash invalid"
        grep -qE '^Checksum: .* \(valid\)$' <<<"$info" || die "$img checksum invalid"
    done
    "$TOOLS/venv/bin/python" -I "$REPO/tools/check_parts.py" "$d/partitions.bin" || die "partition layout"
    # source-level guard (same as CI) on the exact commit
    git -C "$REPO" show "$sha:src/ramses_radio.cpp" | grep 'Serial.println("\[radio\] TX blocked' >/dev/null \
        || die "sendFrame not blocked in source"
    git -C "$REPO" show "$sha:src/main.cpp" | python3 -I "$REPO/tools/check_tick.py" \
        || die "periodic TX: orcon::tick() not excluded from RX-only"
    if [ -n "$LOCAL_ENV" ]; then local_ok "$d" "$sha"; else echo "pinned historical RX-only image verified locally (no remote attestation)"; fi
    echo "VERIFIED $sha $env ($d)"
}

# Read-only device checks. Any deviation, missing value or esptool error stops here.
board_checks() {   # <package dir>: chip, pinned MAC, 8 MB, no flash encryption/secure boot. Sets BOARD_ID.
    local d=$1
    PORT="$(pick_port)"; echo "port: $PORT"
    local img info
    for img in bootloader.bin firmware.bin; do
        info="$(esptool --chip esp32 image_info --version 2 "$d/$img")" || die "$img image_info failed"
        grep -qx "Flash size: 8MB" <<<"$info" || die "$img header is not set for 8MB flash"
    done
    local id
    id="$(esptool --chip esp32 --port "$PORT" --after no_reset flash_id 2>&1)" \
        || { echo "$id" >&2; die "flash_id failed (port busy, no board, or not an ESP32)"; }
    grep -E '^(Chip is|MAC:|Detected flash size:) ' <<<"$id"
    grep -qE '^Chip is ESP32-D0WDQ6 \(revision ' <<<"$id" || die "board is not an ESP32-D0WDQ6 (Heltec V2)"
    local mac; mac="$(expected_mac)"
    grep -qix "MAC: $mac" <<<"$id"                         || die "board MAC is not $mac (another board?)"
    [ "$(grep -c '^Detected flash size: ' <<<"$id")" = 1 ] || die "flash size not reported"
    grep -qx 'Detected flash size: 8MB' <<<"$id"           || die "flash is not exactly 8MB"
    # Read only the public security configuration words, never keys or eFuse writes.
    # Classic ESP32 does not implement get_security_info; these masks/addresses
    # match esptool 4.8.1 targets/esp32.py.
    local crypt boot
    crypt="$(esptool --chip esp32 --port "$PORT" --after no_reset read_mem 0x3ff5a000)" || die "security word read failed"
    boot="$(esptool --chip esp32 --port "$PORT" --after no_reset read_mem 0x3ff5a018)" || die "secure boot word read failed"
    python3 -I - "$crypt" "$boot" <<'SECURITY' || die "security state unknown/enabled; plaintext app update refused"
import re, sys
def word(text, address):
    found = re.findall(r'^' + address + r' = (0x[0-9a-f]{8})$', text, re.I | re.M)
    assert len(found) == 1, 'missing/duplicate security word'
    return int(found[0], 16)
assert bin((word(sys.argv[1], '0x3ff5a000') >> 20) & 0x7f).count('1') % 2 == 0, 'flash encryption enabled'
assert word(sys.argv[2], '0x3ff5a018') & 0x30 == 0, 'secure boot enabled'
SECURITY
    BOARD_ID="$id"
}

precheck() {
    local sha; sha="$(target "${1:-}")"
    verify "$sha"; local d; d="$(dist "$sha")"
    board_checks "$d"
    local id="$BOARD_ID" mac; mac="$(expected_mac)"
    # A temporary readback directory, never mutable files inside the release package.
    # On any failure the readbacks are kept as evidence, never deleted.
    local curdir; curdir="$(mktemp -d)"
    # shellcheck disable=SC2064 # expand now: curdir is a local of this function
    trap "keep '$curdir' precheck-failed" EXIT
    local cur="$curdir/partitions.bin"
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0x8000 0xc00 "$cur" >/dev/null \
        || die "reading the board partition table failed"
    "$TOOLS/venv/bin/python" -I "$REPO/tools/check_parts.py" "$cur" || die "board partition layout unexpected"
    cmp -s "$cur" "$d/partitions.bin" || die "board partition table differs (NVS could move)"
    # app1 bounds come from the REAL, just-validated board table. A failed app0 write,
    # power loss or reset can make the bootloader try app1, so the whole slot must be
    # erased or the pinned RX-only image. Never erase/repair here: unknown data is NO-GO.
    local app1; app1="$("$TOOLS/venv/bin/python" -I "$REPO/tools/check_app1.py" bounds "$cur")" \
        || die "app1 bounds not derivable from the verified board table"
    # shellcheck disable=SC2086 # "<offset> <size>", two arguments
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash $app1 "$curdir/app1.bin" >/dev/null \
        || die "reading the entire app1 fallback slot failed"
    local verdict
    verdict="$("$TOOLS/venv/bin/python" -I "$REPO/tools/check_app1.py" check "$cur" "$curdir/app1.bin" \
        "$(sed -n 's/^MAC: //p' <<<"$id")" "$TOOLS/board.mac")" \
        || { echo "$verdict"; die "app1 fallback is unknown/unsafe; app-only update refused (nothing erased or repaired)"; }
    echo "$verdict"
    # The accepted known board state is re-read independently: same bytes in a second read.
    local sector; sector="$(sed -n 's/.*RECHECK-SECTOR \(0x[0-9a-f]*\)$/\1/p' <<<"$verdict")"
    if [ -n "$sector" ]; then
        local abs; abs="0x$(printf %x $(( ${app1%% *} + sector )))"
        esptool --chip esp32 --port "$PORT" --after no_reset read_flash "$abs" 0x1000 "$curdir/app1-sector.bin" >/dev/null \
            || die "re-reading the known app1 sector failed"
        tail -c +$((sector + 1)) "$curdir/app1.bin" | head -c 4096 | cmp -s - "$curdir/app1-sector.bin" \
            || die "app1 sector differs between two reads; NO-GO"
        echo "app1 sector $abs re-read: identical"
    fi
    # Application-only is safe only with this EXACT known bootloader and app0 selection.
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0x1000 \
        "$(wc -c < "$d/bootloader.bin" | tr -d ' ')" "$curdir/bootloader.bin" >/dev/null \
        || die "reading the board bootloader failed"
    cmp -s "$curdir/bootloader.bin" "$d/bootloader.bin" || die "board bootloader differs; app-only update refused"
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0xe000 0x2000 "$curdir/otadata.bin" >/dev/null \
        || die "reading the board boot selection failed"
    cmp -s "$curdir/otadata.bin" "$d/boot_app0.bin" || die "boot selection differs from pinned app0 selection; refused"
    rm -rf "$curdir"; trap - EXIT
    echo "PRECHECK OK ${sha:0:7}: ESP32-D0WDQ6 $mac, 8MB, exact bootloader/table/app0 selection, app1 empty, pinned RX-only or the exact known board state; NVS untouched"

}

# OTA-aware USB recovery. After a WiFi OTA, otadata selects app1 and app1 holds an image, so the
# pristine-state precheck refuses the board by design. recover reads the state the way the
# bootloader does and identifies BOTH slots by image hash (check_slots.py); unknown content is
# NO-GO and nothing is written. write: app0 <- the verified RX-only package, then otadata <- the
# pinned app0 selection, each verified by readback before the next step. Write order is safe
# against interruption: during the app0 write otadata still selects the old, known slot (or,
# if that was app0, the bootloader falls back to the known app1 image); during the otadata
# write both entries may be blank, and the bootloader then starts app0 = the image just verified.
recover() {
    local mode=${1:-} sha; sha="$(target "${2:-}")"
    [ "$mode" = check ] || [ "$mode" = write ] || die "usage: LOCAL_ENV=heltec_v2_sx1276 recover check|write <commit>"
    [ "$LOCAL_ENV" = heltec_v2_sx1276 ] || die "recovery writes only a local RX-only package (LOCAL_ENV=heltec_v2_sx1276)"
    verify "$sha"; local d; d="$(dist "$sha")"
    board_checks "$d"
    local w; w="$(mktemp -d)"
    # shellcheck disable=SC2064 # expand now: w is a local of this function
    trap "keep '$w' recover-failed" EXIT
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0x8000 0xc00 "$w/partitions.bin" >/dev/null \
        || die "reading the board partition table failed"
    "$TOOLS/venv/bin/python" -I "$REPO/tools/check_parts.py" "$w/partitions.bin" || die "board partition layout unexpected"
    cmp -s "$w/partitions.bin" "$d/partitions.bin" || die "board partition table differs (NVS could move)"
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0x1000 \
        "$(wc -c < "$d/bootloader.bin" | tr -d ' ')" "$w/bootloader.bin" >/dev/null || die "reading the board bootloader failed"
    cmp -s "$w/bootloader.bin" "$d/bootloader.bin" || die "board bootloader differs; recovery refused"
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0xe000 0x2000 "$w/otadata.bin" >/dev/null \
        || die "reading otadata failed"
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0x10000 0x330000 "$w/app0.bin" >/dev/null \
        || die "reading the entire app0 slot failed"
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0x340000 0x330000 "$w/app1.bin" >/dev/null \
        || die "reading the entire app1 slot failed"
    local f
    { for f in "$TOOLS"/local/*/firmware.bin "$TOOLS/fw/$ROLLBACK_COMMIT/firmware.bin"; do
          [ -f "$f" ] && echo "$(shasum -a 256 < "$f" | cut -d' ' -f1) package $(basename "$(dirname "$f")")"
      done
      [ ! -f "$ORCON_INSTALLED" ] || cat "$ORCON_INSTALLED"; } > "$w/known.txt"
    "$TOOLS/venv/bin/python" -I "$REPO/tools/check_slots.py" check "$w/partitions.bin" "$w/otadata.bin" \
        "$w/app0.bin" "$w/app1.bin" "$w/known.txt" "$(sed -n 's/^MAC: //p' <<<"$BOARD_ID")" "$TOOLS/board.mac" \
        || die "board state not positively identified; nothing written"
    if [ "$mode" = check ]; then
        rm -rf "$w"; trap - EXIT
        echo "RECOVERY CHECK OK ${sha:0:7}: exact bootloader/table, otadata and both slots identified; nothing written"
        return
    fi
    cp "$d/"{bootloader.bin,partitions.bin,boot_app0.bin,firmware.bin,BUILD_INFO.txt} "$w/"
    sed '/  heltec-v2-rx-only-merged.bin$/d' "$d/SHA256SUMS.txt" > "$w/SHA256SUMS.txt"
    flash_args "$w" "$PORT"
    echo "will write: app0 <- ${sha:0:7} RX-only, then otadata 0xe000 <- pinned boot_app0.bin (app0 selection)"
    read -r -p "Recover ${sha:0:7} on $PORT? Type recover: " ok
    [ "$ok" = recover ] || die "aborted by user"
    files_ok "$w" "$sha"
    local_ok "$w" "$sha" || die "snapshot differs from the validated evidence"
    esptool "${FLASH_ARGS[@]}" || die "app0 write failed; otadata untouched (old selection still boots)"
    local size; size="$(wc -c < "$w/firmware.bin" | tr -d ' ')"
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0x10000 "$size" "$w/readback.bin" >/dev/null \
        || die "app0 readback failed; otadata untouched"
    cmp -s "$w/readback.bin" "$w/firmware.bin" || die "app0 readback mismatch; otadata untouched"
    echo "APP0 READBACK SHA256 $(shasum -a 256 "$w/readback.bin" | cut -d' ' -f1)"
    esptool --chip esp32 --port "$PORT" --after no_reset write_flash 0xe000 "$w/boot_app0.bin" \
        || die "otadata write failed; no reset (app0 is verified, bootloader falls back to it)"
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0xe000 0x2000 "$w/otadata-readback.bin" >/dev/null \
        || die "otadata readback failed; no reset"
    cmp -s "$w/otadata-readback.bin" "$w/boot_app0.bin" || die "otadata readback mismatch; no reset"
    echo "OTADATA READBACK OK (app0 selected)"
    esptool --chip esp32 --port "$PORT" --after hard_reset read_mac
    rm -rf "$w"; trap - EXIT
    echo "RECOVERED ${sha:0:7}: app0 RX-only and app0 selection verified; NVS/bootloader/table/app1 untouched"
}

# Offline: full verification, then the exact command flash would run. Never opens a port.
plan() {
    local sha; sha="$(target "${1:-}")"
    verify "$sha"; local d; d="$(dist "$sha")"
    flash_args "$d" "${PORT:-<detected at flash time>}"
    echo "PLAN ${sha:0:7} ${LOCAL_ENV:-heltec_v2_sx1276} (no board access; app0 only, NVS/bootloader/table/otadata/app1 untouched):"
    echo "  esptool ${FLASH_ARGS[*]}"
}

flash() {
    local sha; sha="$(target "${1:-}")"
    precheck "$sha"; local d; d="$(dist "$sha")"
    # Freeze verified bytes before confirmation; verify the snapshot again after it.
    local snapshot; snapshot="$(mktemp -d)"; trap 'rm -rf "$snapshot"' EXIT
    cp "$d/"{bootloader.bin,partitions.bin,boot_app0.bin,firmware.bin,BUILD_INFO.txt} "$snapshot/"
    # The historical manifest also covers an unused merged image. Keep the
    # original checksums of all four verified flash inputs, without staging it.
    sed '/  heltec-v2-rx-only-merged.bin$/d' "$d/SHA256SUMS.txt" > "$snapshot/SHA256SUMS.txt"
    flash_args "$snapshot" "$PORT"
    echo "will write app0 only: esptool ${FLASH_ARGS[*]}"
    read -r -p "Write ${sha:0:7} (${LOCAL_ENV:-heltec_v2_sx1276}) on $PORT? Type yes: " ok
    [ "$ok" = "yes" ] || die "aborted by user"
    if [ "$LOCAL_ENV" = "$RF_TEST_ENV" ]; then
        echo "WARNING: RF TEST image: the web page can put real 868 MHz packets on air (capped per boot, 2 s apart)."
        read -r -p "Type the RF test acknowledgement phrase: " ok
        [ "$ok" = "$RF_TEST_ACK" ] || die "RF test image not confirmed"
    fi
    files_ok "$snapshot" "$sha"
    # The frozen bytes must still be the ones the ORIGINAL evidence recorded, not just
    # self-consistent with a package manifest that could have changed alongside them.
    [ -z "$LOCAL_ENV" ] || local_ok "$snapshot" "$sha" || die "snapshot differs from the validated evidence"
    esptool "${FLASH_ARGS[@]}" || die "app write failed; no reset; use approved USB rollback procedure"
    local size; size="$(wc -c < "$snapshot/firmware.bin" | tr -d ' ')"
    esptool --chip esp32 --port "$PORT" --after no_reset read_flash 0x10000 "$size" "$snapshot/readback.bin" \
        || { keep "$snapshot" readback-failed; die "app readback failed; no reset; USB recovery may be needed"; }
    cmp -s "$snapshot/readback.bin" "$snapshot/firmware.bin" \
        || { keep "$snapshot" readback-mismatch; die "app readback SHA-256/bytes mismatch; no reset"; }
    echo "READBACK SHA256 $(shasum -a 256 "$snapshot/readback.bin" | cut -d' ' -f1)"
    # Only a successful, identical readback permits the final normal boot.
    esptool --chip esp32 --port "$PORT" --after hard_reset read_mac
    rm -rf "$snapshot"; trap - EXIT

}

# Package a local build (tools/test_local.ps1 -Firmware evidence dir) for verify/precheck/flash.
# Reads files only; no board access. The package is verified completely before it is accepted.
package() {
    local ev="${1:?usage: package <evidence-dir> <env>}" env="${2:?usage: package <evidence-dir> <env>}"
    env_ok "$env" || die "env $env may not be flashed"
    ev="$(cd "$ev" && pwd)"; local src="$ev/artifacts/$env"
    [ -f "$ev/RESULTS.json" ] && [ -f "$src/firmware.bin" ] || die "no evidence/artifacts for $env in $ev"
    local sha; sha="$(fulsha "$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["commit"])' "$ev/RESULTS.json")")"
    LOCAL_ENV="$env"; local d; d="$(dist "$sha")"
    rm -rf "$d"; mkdir -p "$d"
    cp "$src/"{bootloader.bin,partitions.bin,boot_app0.bin,firmware.bin} "$d/"
    ( cd "$d" && shasum -a 256 bootloader.bin partitions.bin boot_app0.bin firmware.bin > SHA256SUMS.txt )
    { echo "commit=$sha"; echo "env=$env"; echo "chip=esp32"; echo "source=local"; echo "evidence=$ev"
      echo "evidence_sha256=$(shasum -a 256 "$ev/RESULTS.json" | cut -d' ' -f1)"
      echo "source_sha256=$(shasum -a 256 "$ev/SOURCE_HASHES.json" | cut -d' ' -f1)"
      echo "build_sha256=$(shasum -a 256 "$src/BUILD_INFO.json" | cut -d' ' -f1)"; } > "$d/BUILD_INFO.txt"
    verify "$sha"
}

monitor() {
    bootstrap
    PORT="$(pick_port)"
    "$TOOLS/venv/bin/python" -I "$REPO/tools/rxmon.py" "$PORT" "${1:-60}" "${2:-}" "${3:-}"
}

# Tests source functions only; all hardware operations are replaced by failing/local stubs.
[ "${HELTEC_V2_SOURCE_ONLY:-}" != 1 ] || return 0
[ -z "$LOCAL_ENV" ] || env_ok "$LOCAL_ENV" || die "LOCAL_ENV=$LOCAL_ENV may not be flashed"
cmd="${1:-}"; shift || true
case "$cmd" in
    ports)   ports ;;
    plan)    plan "$@" ;;
    package) package "$@" ;;
    verify)  verify "$@" ;;
    precheck) precheck "$@" ;;
    flash)   flash "$@" ;;
    monitor) monitor "$@" ;;
    recover) recover "$@" ;;
    *) sed -n '2,22p' "$0"; exit 2 ;;
esac
