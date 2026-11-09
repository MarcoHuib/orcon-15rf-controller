#!/usr/bin/env bash
# heltec_v2.sh — fetch, verify, flash and monitor the Heltec V2 builds without
# PlatformIO uploads: historical GitHub Actions artifacts, or local builds.
#
#   tools/heltec_v2.sh fetch   <commit>        download the CI artifact of <commit> (historical)
#   tools/heltec_v2.sh package <evidence-dir> <env>   local build -> flash package (no board access)
#   LOCAL_ENV=<env> tools/heltec_v2.sh verify|precheck|plan|flash <commit>   use that local package
#   tools/heltec_v2.sh ports                    list USB serial ports (no port is opened)
#   tools/heltec_v2.sh plan    <commit|rollback>  verify offline, print the exact write command
#   tools/heltec_v2.sh flash   rollback         back to the RX-only image the board ran before the PoC
#   tools/heltec_v2.sh verify  <commit>        checksums, commit stamp, RX-only, ESP32 target
#   tools/heltec_v2.sh precheck <commit>       verify + read-only board checks (chip, 8MB, partitions)
#   tools/heltec_v2.sh flash   <commit>        precheck, then write via USB (asks for "yes")
#   tools/heltec_v2.sh monitor <secs> [log] [reset]  serial log + RX summary
#
# esptool 4.8.1 runs from its PyPI source tarball (SHA-256 pinned below) in a
# local venv with hash-pinned pure-Python wheels; nothing is built or installed
# system-wide. The four images are written at their own offsets so the NVS
# partition (Wi-Fi/MQTT/IDs at 0x9000) is preserved.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$REPO/.flashtools"
PORT="${PORT:-}"               # empty: the single USB serial port found in $DEV_DIR
DEV_DIR="${DEV_DIR:-/dev}"
BRANCH="feature/heltec-v2-rx-only"
ESPTOOL_SHA256="dc4ef26b659e1a8dcb019147c0ea6d94980b34de99fbe09121c7941c8b254531"
LOCAL_ENV="${LOCAL_ENV:-}"     # set: a local package from tools/test_local.ps1 -Firmware, not Actions
LOCAL_ENVS="heltec_v2_sx1276 heltec_v2_sx1276_tx_poc_dryrun"   # the only images that may be written
BOOT_APP0_SHA256="f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0"  # == CI d5c39e1
# A local package needs these local checks PASS on the same clean commit (RESULTS.json).
ROLLBACK_COMMIT="${ROLLBACK_COMMIT:-d5c39e1b4d1f0ed37defe69b51e3562c36058208}"  # RX-only, ran before the PoC
# offset:image:slot size. NVS (0x9000-0xDFFF) is never written; the merged image is never used.
FLASH_MAP="0x1000:bootloader.bin:0x7000 0x8000:partitions.bin:0xc00 0xe000:boot_app0.bin:0x2000 0x10000:firmware.bin:0x330000"
FLASH_HDR="0230"   # image header bytes 2-3: DIO, 8 MB / 40 MHz (as the CI d5c39e1 image on the board)
LOCAL_REQUIRED="codec frames rx log orcon_rx orcon_v3 rxpipe netcfg oled tx_poc safety-and-negative-compiles privacy poc-web"

die() { echo "FAIL: $*" >&2; exit 1; }

bootstrap() {
    [ -x "$TOOLS/venv/bin/python" ] && [ -d "$TOOLS/esptool-4.8.1" ] && return
    local py; py="$(command -v python3.12 || command -v python3)"
    mkdir -p "$TOOLS/dl"
    curl -sSfL -o "$TOOLS/dl/esptool-4.8.1.tar.gz" \
        https://files.pythonhosted.org/packages/source/e/esptool/esptool-4.8.1.tar.gz
    echo "$ESPTOOL_SHA256  $TOOLS/dl/esptool-4.8.1.tar.gz" | shasum -a 256 -c - >/dev/null \
        || die "esptool tarball checksum mismatch"
    tar -xzf "$TOOLS/dl/esptool-4.8.1.tar.gz" -C "$TOOLS"
    "$py" -m venv "$TOOLS/venv"
    printf '%s\n' \
        'pyserial==3.5 --hash=sha256:c4451db6ba391ca6ca299fb3ec7bae67a5c55dde170964c7a14ceefec02f2cf0' \
        'intelhex==2.3.0 --hash=sha256:87cc5225657524ec6361354be928adfd56bcf2a3dcc646c40f8f094c39c07db4' \
        > "$TOOLS/requirements.txt"
    "$TOOLS/venv/bin/pip" install -q --disable-pip-version-check --require-hashes \
        --only-binary=:all: --no-deps -r "$TOOLS/requirements.txt"
}
esptool() { PYTHONPATH="$TOOLS/esptool-4.8.1" "$TOOLS/venv/bin/python" -m esptool "$@"; }

fulsha() { [ -n "${1:-}" ] || die "commit argument missing"
           git -C "$REPO" rev-parse --verify --quiet "$1^{commit}" || die "unknown commit '$1'"; }
target() {   # <commit>, or "rollback" = the historical RX-only Actions image (only without LOCAL_ENV)
    if [ "${1:-}" = rollback ]; then
        [ -z "$LOCAL_ENV" ] || die "rollback is the historical RX-only image: unset LOCAL_ENV (a local RX-only package is flashed by commit)"
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
flash_args() {   # FLASH_ARGS for <dir> <port>: write_flash only, no erase option, image by image
    local d=$1 e off img max
    FLASH_ARGS=(--chip esp32 --port "$2" --baud 460800 write_flash)
    for e in $FLASH_MAP; do IFS=: read -r off img max <<<"$e"; FLASH_ARGS+=("$off" "$d/$img"); done
}

ci_ok() {   # every check run on the commit completed with success (and there is at least one)
    local runs
    runs="$(gh api "repos/{owner}/{repo}/commits/$1/check-runs" \
            --jq '.check_runs[] | "\(.status)/\(.conclusion) \(.name)"')" || die "cannot read CI status"
    [ -n "$runs" ] || die "no CI check runs for ${1:0:7}"
    echo "$runs" >&2
    ! grep -qv '^completed/success ' <<<"$runs" || die "CI is not green for ${1:0:7}"
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
    python3 -I - "$ev" "$d" "$sha" "$env" $LOCAL_REQUIRED <<'PY' || die "local evidence does not cover this image"
import hashlib, json, sys
ev, d, sha, env, *required = sys.argv[1:]
r = json.load(open(f"{ev}/RESULTS.json"))
rows = {c["name"]: c["result"] for c in r["checks"]}
problems = []
if r.get("commit") != sha: problems.append(f"evidence commit {r.get('commit')} != {sha}")
if r.get("dirty") != "": problems.append("evidence from a dirty tree")
problems += [f"{n}: {v}" for n, v in rows.items() if v != "PASS"]
for n in required + [f"firmware-{env}", f"image-{env}", f"partitions-{env}"]:
    if rows.get(n) != "PASS": problems.append(f"{n} not PASS")
recorded = {f["name"]: f["sha256"] for f in json.load(open(f"{ev}/artifacts/{env}/BUILD_INFO.json"))["files"]}
for n in ("bootloader.bin", "partitions.bin", "firmware.bin"):
    if hashlib.sha256(open(f"{d}/{n}", "rb").read()).hexdigest() != recorded.get(n): problems.append(f"{n} hash differs from evidence")
for p in problems: print("  " + p, file=sys.stderr)
sys.exit(1 if problems else 0)
PY
    echo "local evidence OK: $ev"
}

markers() {   # image contents per environment; network firmware update must be compiled out
    local f=$1 env=$2 s
    grep -q -a "SX1276 direct RX (TX disabled)" "$f" || die "not the SX1276 RX-only backend"
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
        *) die "env $env may not be flashed" ;;
    esac
}

dist()   { if [ -n "$LOCAL_ENV" ]; then echo "$TOOLS/local/$(fulsha "$1")-$LOCAL_ENV"; else echo "$TOOLS/fw/$(fulsha "$1")"; fi; }

fetch() {
    local sha; sha="$(fulsha "$1")"; local d; d="$(dist "$sha")"
    [ -f "$d/BUILD_INFO.txt" ] && { echo "$d"; return; }
    [ -z "$LOCAL_ENV" ] || die "no local package $d (run: tools/heltec_v2.sh package <evidence-dir> $LOCAL_ENV)"
    local run
    run="$(gh run list --branch "$BRANCH" --commit "$sha" --status success --limit 1 \
           --json databaseId --jq '.[0].databaseId')"
    [ -n "$run" ] || die "no successful CI run for $sha on $BRANCH"
    rm -rf "$d"; mkdir -p "$d"
    gh run download "$run" -n heltec-v2-rx-only -D "$d" >&2
    echo "$d"
}

verify() {
    local sha; sha="$(fulsha "${1:-}")"
    bootstrap
    local d; d="$(fetch "$sha")"
    ( cd "$d" && shasum -a 256 -c SHA256SUMS.txt ) || die "SHA256SUMS mismatch"
    layout_ok "$d"
    local env="${LOCAL_ENV:-heltec_v2_sx1276}"
    grep -qx "commit=$sha" "$d/BUILD_INFO.txt"        || die "artifact is not built from $sha"
    grep -qx "env=$env" "$d/BUILD_INFO.txt"           || die "wrong PlatformIO env"
    grep -q -a "$sha" "$d/firmware.bin"                || die "firmware.bin lacks commit stamp"
    markers "$d/firmware.bin" "$env"
    local info; info="$(esptool --chip esp32 image_info --version 2 "$d/firmware.bin")"
    grep -q "Chip ID: 0 (ESP32)" <<<"$info"            || die "firmware.bin is not for ESP32 (classic)"
    grep -q "Validation hash: .* (valid)" <<<"$info"   || die "firmware.bin hash invalid"
    "$TOOLS/venv/bin/python" -I "$REPO/tools/check_parts.py" "$d/partitions.bin" || die "partition layout"
    # source-level guard (same as CI) on the exact commit
    git -C "$REPO" show "$sha:src/ramses_radio.cpp" | grep -q 'Serial.println("\[radio\] TX blocked' \
        || die "sendFrame not blocked in source"
    git -C "$REPO" show "$sha:src/main.cpp" | python3 -I "$REPO/tools/check_tick.py" \
        || die "periodic TX: orcon::tick() not excluded from RX-only"
    if [ -n "$LOCAL_ENV" ]; then local_ok "$d" "$sha"; else ci_ok "$sha"; fi
    echo "VERIFIED $sha $env ($d)"
}

# Read-only device checks. Any deviation, missing value or esptool error stops here.
precheck() {
    local sha; sha="$(fulsha "${1:-}")"
    verify "$sha"; local d; d="$(dist "$sha")"
    PORT="$(pick_port)"; echo "port: $PORT"
    local img
    for img in bootloader.bin firmware.bin; do
        esptool --chip esp32 image_info --version 2 "$d/$img" | grep -qx "Flash size: 8MB" \
            || die "$img header is not set for 8MB flash"
    done
    local id
    id="$(esptool --chip esp32 --port "$PORT" flash_id 2>&1)" \
        || { echo "$id" >&2; die "flash_id failed (port busy, no board, or not an ESP32)"; }
    grep -E '^(Chip is|MAC:|Detected flash size:) ' <<<"$id"
    grep -qE '^Chip is ESP32-D0WDQ6 \(revision ' <<<"$id" || die "board is not an ESP32-D0WDQ6 (Heltec V2)"
    local mac; mac="$(expected_mac)"
    grep -qix "MAC: $mac" <<<"$id"                         || die "board MAC is not $mac (another board?)"
    [ "$(grep -c '^Detected flash size: ' <<<"$id")" = 1 ] || die "flash size not reported"
    grep -qx 'Detected flash size: 8MB' <<<"$id"           || die "flash is not exactly 8MB"
    # The board's current partition table must equal the new one, so NVS keeps its place.
    local cur="$d/device-partitions.bin"; rm -f "$cur"
    esptool --chip esp32 --port "$PORT" read_flash 0x8000 0xc00 "$cur" >/dev/null \
        || die "reading the board's partition table failed"
    echo "board partition table:"
    "$TOOLS/venv/bin/python" -I "$REPO/tools/check_parts.py" "$cur" || die "board partition layout unexpected"
    cmp -s "$cur" "$d/partitions.bin" || die "board partition table differs from the new one (NVS could move)"
    local hdr="$d/device-bootloader-header.bin"; rm -f "$hdr"
    esptool --chip esp32 --port "$PORT" read_flash 0x1000 0x10 "$hdr" >/dev/null \
        || die "reading the board's bootloader header failed"
    [ "$(xxd -p -s 2 -l 2 "$hdr")" = "$FLASH_HDR" ] || die "board bootloader header is not DIO/8MB/40MHz"
    echo "PRECHECK OK ${sha:0:7}: ESP32-D0WDQ6 $mac, 8MB, partition table identical (NVS @0x9000 kept)"
}

# Offline: full verification, then the exact command flash would run. Never opens a port.
plan() {
    local sha; sha="$(target "${1:-}")"
    verify "$sha"; local d; d="$(dist "$sha")"
    flash_args "$d" "${PORT:-<detected at flash time>}"
    echo "PLAN ${sha:0:7} ${LOCAL_ENV:-heltec_v2_sx1276} (no board access; NVS 0x9000-0xDFFF untouched, no erase):"
    echo "  esptool ${FLASH_ARGS[*]}"
}

flash() {
    local sha; sha="$(target "${1:-}")"
    precheck "$sha"; local d; d="$(dist "$sha")"
    flash_args "$d" "$PORT"
    echo "will run: esptool ${FLASH_ARGS[*]}"
    read -r -p "Write commit ${sha:0:7} (${LOCAL_ENV:-heltec_v2_sx1276}) to the Heltec on $PORT? Type yes: " ok
    [ "$ok" = "yes" ] || die "aborted by user"
    esptool "${FLASH_ARGS[@]}"
}

# Package a local build (tools/test_local.ps1 -Firmware evidence dir) for verify/precheck/flash.
# Reads files only; no board access. The package is verified completely before it is accepted.
package() {
    local ev="${1:?usage: package <evidence-dir> <env>}" env="${2:?usage: package <evidence-dir> <env>}"
    [[ " $LOCAL_ENVS " == *" $env "* ]] || die "env $env may not be flashed"
    ev="$(cd "$ev" && pwd)"; local src="$ev/artifacts/$env"
    [ -f "$ev/RESULTS.json" ] && [ -f "$src/firmware.bin" ] || die "no evidence/artifacts for $env in $ev"
    local sha; sha="$(fulsha "$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["commit"])' "$ev/RESULTS.json")")"
    LOCAL_ENV="$env"; local d; d="$(dist "$sha")"
    rm -rf "$d"; mkdir -p "$d"
    cp "$src/bootloader.bin" "$src/partitions.bin" "$src/firmware.bin" "$d/"
    local b
    for b in "$HOME"/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin; do
        [ "$(shasum -a 256 "$b" | cut -d' ' -f1)" = "$BOOT_APP0_SHA256" ] && { cp "$b" "$d/boot_app0.bin"; break; }
    done
    [ -f "$d/boot_app0.bin" ] || die "no boot_app0.bin with the pinned SHA-256"
    ( cd "$d" && shasum -a 256 bootloader.bin partitions.bin boot_app0.bin firmware.bin > SHA256SUMS.txt )
    { echo "commit=$sha"; echo "env=$env"; echo "chip=esp32"; echo "source=local"; echo "evidence=$ev"
      echo "evidence_sha256=$(shasum -a 256 "$ev/RESULTS.json" | cut -d' ' -f1)"; } > "$d/BUILD_INFO.txt"
    verify "$sha"
}

monitor() {
    bootstrap
    PORT="$(pick_port)"
    "$TOOLS/venv/bin/python" -I "$REPO/tools/rxmon.py" "$PORT" "${1:-60}" "${2:-}" "${3:-}"
}

# Tests source the functions only (HELTEC_V2_SOURCE_ONLY=1) and stub esptool/gh/ports.
[ "${HELTEC_V2_SOURCE_ONLY:-}" != 1 ] || return 0
[ -z "$LOCAL_ENV" ] || [[ " $LOCAL_ENVS " == *" $LOCAL_ENV "* ]] || die "LOCAL_ENV=$LOCAL_ENV may not be flashed"
cmd="${1:-}"; shift || true
case "$cmd" in
    ports)   ports ;;
    plan)    plan "$@" ;;
    fetch)   fetch "$@" ;;
    package) package "$@" ;;
    verify)  verify "$@" ;;
    precheck) precheck "$@" ;;
    flash)   flash "$@" ;;
    monitor) monitor "$@" ;;
    *) sed -n '2,19p' "$0"; exit 2 ;;
esac
