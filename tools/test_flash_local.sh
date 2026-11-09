#!/usr/bin/env bash
# test_flash_local.sh <known-good partitions.bin> — the local USB flash path without hardware or
# Actions: the REAL heltec_v2.sh functions (sourced), with esptool stubbed, a fake /dev and
# generated packages. No serial port is opened and nothing is written to any board.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
PART_GOOD="$(cd "$(dirname "${1:?usage: test_flash_local.sh <known-good partitions.bin>}")" && pwd)/$(basename "$1")"
SHA="$(git -C "$REPO" rev-parse HEAD)"
ROLL="$(HELTEC_V2_SOURCE_ONLY=1 bash -c 'source "$1/tools/heltec_v2.sh"; fulsha "$ROLLBACK_COMMIT"' _ "$REPO")"
DRY=heltec_v2_sx1276_tx_poc_dryrun RX=heltec_v2_sx1276
BOOT_GOOD="$(dirname "$PART_GOOD")/bootloader.bin"
ROLL_DIR="$REPO/.flashtools/fw/$ROLL"
export LOCAL_REQUIRED="$(sed -n 's/^LOCAL_REQUIRED="\(.*\)"/\1/p' "$REPO/tools/heltec_v2.sh")"
export REPO W PART_GOOD SHA ROLL BOOT_GOOD ROLL_DIR
mkdir -p "$W/tools/venv/bin" "$W/dev" "$W/dev2"; ln -s "$(command -v python3)" "$W/tools/venv/bin/python"
touch "$W/dev2/cu.usbserial-0001" "$W/dev2/cu.usbserial-0002" "$W/tty"
fails=0

# mkpkg <dir> <commit> <env> <kind: local|ci> <mutation>: a complete package, then one defect.
mkpkg() {
    python3 - "$@" <<'PY'
import hashlib, json, os, shutil, sys
d, sha, env, kind, mut = sys.argv[1:]
shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
def img(kind_byte, hdr, body, size):
    out = bytes([0xE9, kind_byte]) + hdr + body
    return out + b"\x00" * (size - len(out))
markers = {"heltec_v2_sx1276": b"SX1276 direct RX (TX disabled) RX-ONLY TEST FIRMWARE",
           "heltec_v2_sx1276_tx_poc_dryrun": b"SX1276 direct RX (TX disabled) tx-poc-dryrun DRY-RUN ONLY / NO RF"}[env]
fw_hdr = b"\x02\x20" if mut == "fw-header-4mb" else b"\x02\x30"
stamp = b"" if mut == "no-stamp" else sha.encode()
fw_body = (b"\x00" if mut == "not-esp" else b"") + b" " + stamp + b" " + markers
if mut == "rx-image-for-dryrun": fw_body = b" " + stamp + b" SX1276 direct RX (TX disabled) RX-ONLY TEST FIRMWARE"
files = {
    "bootloader.bin": b"X" * 0x7001 if mut == "bootloader-too-big" else open(os.environ["BOOT_GOOD"], "rb").read(),
    "partitions.bin": open(os.environ["PART_GOOD"], "rb").read(),
    "boot_app0.bin": open(os.environ["ROLL_DIR"] + "/boot_app0.bin", "rb").read()[:4096 if mut == "boot-app0-size" else 8192],
    "firmware.bin": b"" if mut == "empty-firmware" else img(0x05, fw_hdr, fw_body, 0x330001 if mut == "firmware-too-big" else 65536),
}
if kind == "ci": files["firmware.bin"] = open(os.environ["ROLL_DIR"] + "/firmware.bin", "rb").read()
if mut == "not-esp": files["firmware.bin"] = b"\x00" + files["firmware.bin"][1:]
if mut == "bad-partitions": p = bytearray(files["partitions.bin"]); p[40] ^= 1; files["partitions.bin"] = bytes(p)
for n, b in files.items(): open(f"{d}/{n}", "wb").write(b)
sums = "".join(f"{hashlib.sha256(b).hexdigest()}  {n}\n" for n, b in files.items())
open(f"{d}/SHA256SUMS.txt", "w").write(sums)
info = [f"commit={sha}", f"env={'heltec_v2_sx1276' if mut == 'wrong-env' else env}", "chip=esp32"]
if kind == "local":
    ev = d + ".evidence"; shutil.rmtree(ev, ignore_errors=True); os.makedirs(f"{ev}/artifacts/{env}")
    names = os.environ["LOCAL_REQUIRED"].split()
    names += [f"image-{env}", f"partitions-{env}"]
    json.dump({"commit": sha, "dirty": "", "checks": [{"name": n, "result": "PASS", "exit": 0} for n in names]}, open(f"{ev}/RESULTS.json", "w"))
    json.dump({"files": [{"name": n, "sha256": hashlib.sha256(files[n]).hexdigest()}
                         for n in ("bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin")]}, open(f"{ev}/artifacts/{env}/BUILD_INFO.json", "w"))
    open(f"{ev}/SOURCE_HASHES.json", "w").write("[]")
    info += ["source_sha256=" + hashlib.sha256(open(f"{ev}/SOURCE_HASHES.json", "rb").read()).hexdigest(),
             "build_sha256=" + hashlib.sha256(open(f"{ev}/artifacts/{env}/BUILD_INFO.json", "rb").read()).hexdigest(),
             "source=local", f"evidence={ev}", "evidence_sha256=" + hashlib.sha256(open(f"{ev}/RESULTS.json", "rb").read()).hexdigest()]
open(f"{d}/BUILD_INFO.txt", "w").write("\n".join(info) + "\n")
if mut == "sha-mismatch": open(f"{d}/firmware.bin", "ab").write(b"\x00")   # changed after SHA256SUMS
PY
}
local_pkg()  { mkpkg "$W/tools/local/$SHA-$DRY" "$SHA" "$DRY" local "${1:-none}"; }
ci_rollback(){ mkpkg "$W/tools/fw/$ROLL" "$ROLL" "$RX" ci "${1:-none}"; }

# run <pass|fail> <name> <stdin> <function and args>: real functions, stubbed tools, in a subshell.
run() {
    local want=$1 name=$2 input=$3; shift 3
    : > "$W/esptool.log"
    out="$(printf '%s' "$input" | bash -c '
        HELTEC_V2_SOURCE_ONLY=1 source "$REPO/tools/heltec_v2.sh"
        TOOLS="$W/tools"; DEV_DIR="${DEV_DIR_T:-$W/dev}"
        bootstrap() { :; }
        esptool() {
            echo "$*" >> "$W/esptool.log"
            case "$*" in
                *image_info*) echo "Chip ID: ${CHIPID:-0 (ESP32)}"; echo "Validation hash: 00 (${VALID:-valid})"; echo "Flash size: 8MB"; echo "Checksum: 0x1 (valid)" ;;
                *read_mem*0x3ff5a000*) echo "0x3ff5a000 = 0x00000000" ;;
                *read_mem*0x3ff5a018*) echo "0x3ff5a018 = 0x00000000" ;;
                *flash_id*) printf "%b\n" "${FLASH_ID:-Chip is ESP32-D0WDQ6 (revision v1.0)\nMAC: 02:00:00:00:00:08\nDetected flash size: 8MB}" ;;
                *read_flash*0x10000*) cp "$(dirname "${@: -1}")/firmware.bin" "${@: -1}"; [ "${BAD_READBACK:-}" != 1 ] || printf X >> "${@: -1}" ;;
                *read_flash*0x1000*) cp "$BOOT_GOOD" "${@: -1}" ;;
                *read_flash*0xe000*) cp "$ROLL_DIR/boot_app0.bin" "${@: -1}" ;;
                *write_flash*) [ "${FAIL_WRITE:-}" != 1 ] ;;
                *read_flash*) cp "$PART_GOOD" "${@: -1}" ;;
            esac
        }
        gh() { echo "FORBIDDEN GitHub access" >&2; exit 99; }
        curl() { echo "FORBIDDEN network access" >&2; exit 99; }
        "$@"' _ "$@" 2>&1)"; rc=$?
    got=$([ $rc -eq 0 ] && echo pass || echo fail)
    if [ "$got" = "$want" ]; then mark=ok; else mark=FAIL; fails=$((fails + 1)); fi
    printf '  [%4s] %-46s -> %s: %s\n' "$mark" "$name" "$got" \
        "$({ grep -E 'FAIL:' <<<"$out"; grep -E 'VERIFIED|PLAN|^/|^[0-9a-f]{40}$' <<<"$out"; } | head -1 | cut -c1-110)"
}
assert() {   # assert <name> <condition...>
    local name=$1; shift
    if "$@"; then printf '  [  ok] %s\n' "$name"; else printf '  [FAIL] %s\n' "$name"; fails=$((fails + 1)); fi
}
script() {   # the real script as a user runs it; every case must stop before files/ports/network
    local want=$1 name=$2; shift 2
    out="$(env "$@" 2>&1)"; rc=$?
    got=$([ $rc -eq 0 ] && echo pass || echo fail)
    if [ "$got" = "$want" ]; then mark=ok; else mark=FAIL; fails=$((fails + 1)); fi
    printf '  [%4s] %-46s -> %s (exit %d): %s\n' "$mark" "$name" "$got" "$rc" "$(grep -E 'FAIL:' <<<"$out" | head -1)"
}
H="$REPO/tools/heltec_v2.sh"

echo "argument validation (real script):"
script fail "no command (usage, exit 2)"           bash "$H"
script fail "verify without commit"                bash "$H" verify
script fail "plan without commit"                  bash "$H" plan
script fail "verify unknown commit"                bash "$H" verify no-such-commit
script fail "LOCAL_ENV V3 build"                   LOCAL_ENV=heltec_v3_cc1101_ota bash "$H" plan HEAD
script fail "LOCAL_ENV reserved RF build"          LOCAL_ENV=heltec_v2_sx1276_tx_poc bash "$H" plan HEAD
script fail "package without arguments"            bash "$H" package
script fail "package unknown evidence dir"         bash "$H" package "$W/none" "$DRY"
script fail "package V3 environment"               bash "$H" package "$W" heltec_v3_cc1101_ota
script pass "ports with no USB serial device"      DEV_DIR="$W/dev" bash "$H" ports

echo "serial port selection:"
run fail "no port found"                     "" pick_port
DEV_DIR_T="$W/dev2" run fail "two ports, none chosen" "" pick_port
mkdir -p "$W/dev1"; touch "$W/dev1/cu.usbserial-0001"
DEV_DIR_T="$W/dev1" run pass "exactly one port detected"  "" pick_port
PORT="$W/tty" run pass "PORT set and present"              "" pick_port
PORT=/nonexistent/tty run fail "PORT set but missing"      "" pick_port

echo "verify (local dry-run package):"
local_pkg; LOCAL_ENV=$DRY run pass "valid package"                  "" verify "$SHA"
for m in sha-mismatch no-stamp wrong-env rx-image-for-dryrun not-esp empty-firmware fw-header-4mb \
         bootloader-too-big firmware-too-big boot-app0-size bad-partitions; do
    local_pkg "$m"; LOCAL_ENV=$DRY run fail "defect: $m"            "" verify "$SHA"
done
local_pkg; LOCAL_ENV=$DRY VALID=invalid run fail "image hash invalid (esptool)"     "" verify "$SHA"
local_pkg; LOCAL_ENV=$DRY CHIPID="9 (ESP32-S3)" run fail "image for another chip"  "" verify "$SHA"
rm -rf "$W/tools/local"; LOCAL_ENV=$DRY run fail "no package for commit"            "" verify "$SHA"

echo "plan (dry run of the flash command, no board):"
local_pkg; LOCAL_ENV=$DRY run pass "plan dry-run package" "" plan "$SHA"
P="$W/tools/local/$SHA-$DRY"
assert "application-only write command" grep -qF "write_flash 0x10000 $P/firmware.bin" <<<"$out"
assert "no erase in the command"             bash -c '! grep -qiE "erase|-e( |$)" <<<"$1"' _ "$(grep 'esptool --chip' <<<"$out")"
assert "plan opened no port"                 bash -c '! grep -qE "flash_id|read_flash|write_flash|--port" "$W/esptool.log"'

echo "rollback selection:"
ci_rollback; run pass "rollback = historical RX-only image" "" target rollback
assert "rollback resolves to $ROLL"          grep -qx "$ROLL" <<<"$out"
LOCAL_ENV=$DRY run fail "rollback with LOCAL_ENV dry-run"   "" target rollback
LOCAL_ENV=$RX run fail "rollback with LOCAL_ENV RX-only"    "" target rollback
run pass "plan rollback"                     "" plan rollback
assert "rollback plan is the RX-only env"    grep -q "PLAN ${ROLL:0:7} $RX" <<<"$out"
mkpkg "$W/tools/fw/$ROLL" "$ROLL" "$DRY" local none; perl -pi -e 's/^env=.*/env=heltec_v2_sx1276/' "$W/tools/fw/$ROLL/BUILD_INFO.txt"
run fail "rollback image containing the TX-PoC" "" plan rollback

echo "flash (simulated board; confirmation required):"
local_pkg
LOCAL_ENV=$DRY PORT="$W/tty" HELTEC_MAC=02:00:00:00:00:08 run fail "answer no"   $'no\n' flash "$SHA"
assert "no write without yes"                bash -c '! grep -q write_flash "$W/esptool.log"'
LOCAL_ENV=$DRY PORT="$W/tty" HELTEC_MAC=02:00:00:00:00:08 run fail "empty answer" "" flash "$SHA"
assert "no write on empty answer"            bash -c '! grep -q write_flash "$W/esptool.log"'
LOCAL_ENV=$DRY PORT="$W/tty" HELTEC_MAC=02:00:00:00:00:09 run fail "wrong board, yes" $'yes\n' flash "$SHA"
assert "no write to a wrong board"           bash -c '! grep -q write_flash "$W/esptool.log"'
LOCAL_ENV=$DRY PORT="$W/tty" HELTEC_MAC=02:00:00:00:00:08 run pass "right board, yes" $'yes\n' flash "$SHA"
assert "exactly one app-only write, no erase-all" python3 -c '
import sys
w = [l.split() for l in open(sys.argv[1]) if "write_flash" in l]
assert len(w) == 1, w
a = w[0][w[0].index("write_flash") + 1:]
assert len(a) == 2 and a[0] == "0x10000", a
assert not any("erase" in x or x == "-e" for x in w[0]), w' "$W/esptool.log"

# Missing and incomplete manifests must fail, not merely whatever shasum happens to cover.
for mutation in omitted duplicate bootdata; do
    local_pkg
    case "$mutation" in
      omitted) sed -i.bak '/boot_app0.bin/d' "$P/SHA256SUMS.txt" ;;
      duplicate) head -1 "$P/SHA256SUMS.txt" >> "$P/SHA256SUMS.txt" ;;
      bootdata) printf X | dd of="$P/boot_app0.bin" bs=1 seek=40 conv=notrunc 2>/dev/null
                (cd "$P" && shasum -a 256 bootloader.bin partitions.bin boot_app0.bin firmware.bin > SHA256SUMS.txt) ;;
    esac
    LOCAL_ENV=$DRY run fail "manifest corruption: $mutation" "" verify "$SHA"
done
local_pkg
LOCAL_ENV=$DRY PORT="$W/tty" HELTEC_MAC=02:00:00:00:00:08 FAIL_WRITE=1 run fail "app write failure" $'yes\n' flash "$SHA"
assert "failure reached the actual app write" grep -q write_flash "$W/esptool.log"
assert "failed write never normal-boots" bash -c '! grep -q hard_reset "$W/esptool.log"'
LOCAL_ENV=$DRY PORT="$W/tty" HELTEC_MAC=02:00:00:00:00:08 BAD_READBACK=1 run fail "readback mismatch" $'yes\n' flash "$SHA"
assert "failure reached app readback" grep -q "read_flash 0x10000" "$W/esptool.log"
assert "bad readback never normal-boots" bash -c '! grep -q hard_reset "$W/esptool.log"'
echo; [ $fails -eq 0 ] && echo "ALL PASSED (0 failures)" || echo "FAILED ($fails failures)"
exit $((fails > 0))
