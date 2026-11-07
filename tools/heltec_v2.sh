#!/usr/bin/env bash
# heltec_v2.sh — fetch, verify, flash and monitor the Heltec V2 RX-only build
# from GitHub Actions, without PlatformIO.
#
#   tools/heltec_v2.sh fetch   <commit>        download the CI artifact of <commit>
#   tools/heltec_v2.sh verify  <commit>        checksums, commit stamp, RX-only, ESP32 target
#   tools/heltec_v2.sh flash   <commit>        verify, then write via USB (asks for "yes")
#   tools/heltec_v2.sh monitor <secs> [log] [reset]  serial log + RX summary
#
# esptool 4.8.1 runs from its PyPI source tarball (SHA-256 pinned below) in a
# local venv with hash-pinned pure-Python wheels; nothing is built or installed
# system-wide. The four images are written at their own offsets so the NVS
# partition (Wi-Fi/MQTT/IDs at 0x9000) is preserved.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$REPO/.flashtools"
PORT="${PORT:-/dev/cu.usbserial-0001}"
BRANCH="feature/heltec-v2-rx-only"
ESPTOOL_SHA256="dc4ef26b659e1a8dcb019147c0ea6d94980b34de99fbe09121c7941c8b254531"

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

fulsha() { git -C "$REPO" rev-parse --verify "$1^{commit}"; }
dist()   { echo "$TOOLS/fw/$(fulsha "$1")"; }

fetch() {
    local sha; sha="$(fulsha "$1")"; local d; d="$(dist "$sha")"
    [ -f "$d/BUILD_INFO.txt" ] && { echo "$d"; return; }
    local run
    run="$(gh run list --branch "$BRANCH" --commit "$sha" --status success --limit 1 \
           --json databaseId --jq '.[0].databaseId')"
    [ -n "$run" ] || die "no successful CI run for $sha on $BRANCH"
    rm -rf "$d"; mkdir -p "$d"
    gh run download "$run" -n heltec-v2-rx-only -D "$d" >&2
    echo "$d"
}

verify() {
    bootstrap
    local sha; sha="$(fulsha "$1")"; local d; d="$(fetch "$sha")"
    ( cd "$d" && shasum -a 256 -c SHA256SUMS.txt ) || die "SHA256SUMS mismatch"
    grep -qx "commit=$sha" "$d/BUILD_INFO.txt"        || die "artifact is not built from $sha"
    grep -qx "env=heltec_v2_sx1276" "$d/BUILD_INFO.txt" || die "wrong PlatformIO env"
    grep -q -a "$sha" "$d/firmware.bin"                || die "firmware.bin lacks commit stamp"
    grep -q -a "TX blocked: RX-only firmware" "$d/firmware.bin" || die "RX-only guard string missing"
    grep -q -a "SX1276 direct RX (TX disabled)" "$d/firmware.bin" || die "not the SX1276 RX-only backend"
    local info; info="$(esptool --chip esp32 image_info --version 2 "$d/firmware.bin")"
    grep -q "Chip ID: 0 (ESP32)" <<<"$info"            || die "firmware.bin is not for ESP32 (classic)"
    grep -q "Validation hash: .* (valid)" <<<"$info"   || die "firmware.bin hash invalid"
    "$TOOLS/venv/bin/python" -I "$REPO/tools/check_parts.py" "$d/partitions.bin" || die "partition layout"
    # source-level guard (same as CI) on the exact commit
    git -C "$REPO" show "$sha:src/ramses_radio.cpp" | grep -q 'Serial.println("\[radio\] TX blocked' \
        || die "sendFrame not blocked in source"
    git -C "$REPO" show "$sha:src/main.cpp" | grep -E '^\s*orcon::tick\(' && die "periodic TX active"
    gh api "repos/{owner}/{repo}/commits/$sha/check-runs" --jq '.check_runs[] | "\(.name): \(.conclusion)"' >&2
    echo "VERIFIED $sha  ($d)"
}

flash() {
    local sha; sha="$(fulsha "$1")"
    verify "$sha"; local d; d="$(dist "$sha")"
    esptool --chip esp32 --port "$PORT" flash_id | grep -E "Chip is|MAC|Detected flash size"
    read -r -p "Write commit ${sha:0:7} to the Heltec on $PORT? Type yes: " ok
    [ "$ok" = "yes" ] || die "aborted by user"
    esptool --chip esp32 --port "$PORT" --baud 460800 write_flash \
        0x1000 "$d/bootloader.bin" 0x8000 "$d/partitions.bin" \
        0xe000 "$d/boot_app0.bin" 0x10000 "$d/firmware.bin"
}

monitor() {
    bootstrap
    "$TOOLS/venv/bin/python" -I "$REPO/tools/rxmon.py" "$PORT" "${1:-60}" "${2:-}" "${3:-}"
}

cmd="${1:-}"; shift || true
case "$cmd" in
    fetch)   fetch "$@" ;;
    verify)  verify "$@" ;;
    flash)   flash "$@" ;;
    monitor) monitor "$@" ;;
    *) sed -n '2,13p' "$0"; exit 2 ;;
esac
