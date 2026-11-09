#!/usr/bin/env bash
# test_precheck.sh — run the real precheck/ci_ok/expected_mac functions of heltec_v2.sh
# against simulated esptool/gh output, and check_parts.py against bad tables.
# Only the good case may pass; every bad case must stop (exit 1) before any write.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
PART_GOOD="${1:?usage: test_precheck.sh <known-good partitions.bin>}"
fails=0
mkdir -p "$W/tools/venv/bin" "$W/d"; ln -s "$(command -v python3)" "$W/tools/venv/bin/python"
cp "$PART_GOOD" "$W/d/partitions.bin"

python3 - "$W" <<'PY'                      # build bad tables (each with a VALID md5 entry)
import hashlib, struct, sys
w = sys.argv[1]; good = open(f"{w}/d/partitions.bin", "rb").read()
def table(entries, extra=b""):
    body = b"".join(entries)
    md5 = b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(body).digest()
    out = body + md5 + extra
    return out + b"\xff" * (3072 - len(out))
ent = [good[i:i + 32] for i in range(0, 6 * 32, 32)]
spiffs = bytearray(ent[4]); struct.pack_into("<I", spiffs, 8, 0x170000)            # spiffs size
flags = bytearray(ent[0]); struct.pack_into("<I", flags, 28, 1)                    # nvs flags only
bad = {
    "layout": table(ent[:4] + [bytes(spiffs)] + ent[5:]),
    "flags": table([bytes(flags)] + ent[1:]),
    "extra": table(ent + [ent[5][:2] + b"\x01\x99" + ent[5][4:]]),
    "md5": good[:6 * 32] + b"\xeb\xeb" + b"\xff" * 14 + b"\x00" * 16 + good[7 * 32:],
    "trail": good[:7 * 32] + b"\x00" + good[7 * 32 + 1:],
    "short": good[:2048],
}
for k, v in bad.items(): open(f"{w}/{k}.bin", "wb").write(v)
PY

cat > "$W/run.sh" <<EOF
set -euo pipefail
REPO="$REPO"; TOOLS="$W/tools"; PORT=/dev/fake
$(sed -n '/^die()/p; /^ci_ok()/,/^}/p; /^expected_mac()/,/^}/p; /^precheck()/,/^}/p' "$REPO/tools/heltec_v2.sh")
verify() { :; }; fulsha() { echo "\$1"; }; dist() { echo "$W/d"; }
esptool() {
  case "\$*" in
    *image_info*) echo "Flash size: \${IMG_FS:-8MB}" ;;
    *flash_id*)   printf '%b\n' "\$FLASH_ID"; return \${RC:-0} ;;
    *read_flash*) cp "\${DEVPARTS:-$W/d/partitions.bin}" "\${@: -1}" ;;
  esac
}
gh() { printf '%b' "\${CI_RUNS:-}"; return \${GH_RC:-0}; }
if [ -n "\${BOARDMAC:-}" ]; then echo 02:00:00:00:00:08 > "\$TOOLS/board.mac"; else rm -f "\$TOOLS/board.mac"; fi
\${WHAT:-precheck} 0123456789abcdef0123456789abcdef01234567
EOF

ID='Chip is ESP32-D0WDQ6 (revision v1.0)\nMAC: 02:00:00:00:00:08\nDetected flash size: 8MB'
expect() {   # expect <pass|fail> <name> env...
    local want=$1 name=$2; shift 2
    out="$(env "$@" bash "$W/run.sh" 2>&1)"; rc=$?
    got=$([ $rc -eq 0 ] && echo pass || echo fail)
    if [ "$got" = "$want" ]; then mark=ok; else mark=FAIL; fails=$((fails + 1)); fi
    printf '  [%4s] %-44s -> %s: %s\n' "$mark" "$name" "$got" "$(grep -E 'PRECHECK OK|FAIL:' <<<"$out" | head -1)"
}
echo "precheck (real functions, simulated board):"
expect pass "good board"                     HELTEC_MAC=02:00:00:00:00:08 FLASH_ID="$ID"
expect pass "MAC from .flashtools/board.mac" FLASH_ID="$ID" BOARDMAC=1
expect fail "ESP32-S3 board"                 HELTEC_MAC=02:00:00:00:00:08 FLASH_ID="Chip is ESP32-S3 (QFN56) (revision v0.2)\nMAC: 02:00:00:00:00:08\nDetected flash size: 8MB"
expect fail "ESP32-D0WD-V3 (other model)"    HELTEC_MAC=02:00:00:00:00:08 FLASH_ID="Chip is ESP32-D0WD-V3 (revision v3.0)\nMAC: 02:00:00:00:00:08\nDetected flash size: 8MB"
expect fail "other board (MAC mismatch)"     HELTEC_MAC=02:00:00:00:00:09 FLASH_ID="$ID"
expect fail "expected MAC not configured"    FLASH_ID="$ID"
expect fail "4MB flash"                      HELTEC_MAC=02:00:00:00:00:08 FLASH_ID="${ID/8MB/4MB}"
expect fail "flash size Unknown"             HELTEC_MAC=02:00:00:00:00:08 FLASH_ID="${ID/8MB/Unknown}"
expect fail "flash size line missing"        HELTEC_MAC=02:00:00:00:00:08 FLASH_ID="${ID%\\n*}"
expect fail "esptool error (port busy)"      HELTEC_MAC=02:00:00:00:00:08 FLASH_ID="A fatal error occurred" RC=2
expect fail "image header not 8MB"           HELTEC_MAC=02:00:00:00:00:08 FLASH_ID="$ID" IMG_FS=4MB
for t in layout flags extra md5 trail short; do
    expect fail "board partition table: $t"  HELTEC_MAC=02:00:00:00:00:08 FLASH_ID="$ID" DEVPARTS="$W/$t.bin"
done
echo "ci_ok:"
S='completed/success'
expect pass "all check runs green"           WHAT=ci_ok CI_RUNS="$S Build A\n$S Build B\n"
expect fail "one check run failed"           WHAT=ci_ok CI_RUNS="$S Build A\ncompleted/failure Build B\n"
expect fail "check run still running"        WHAT=ci_ok CI_RUNS="in_progress/null Build A\n"
expect fail "no check runs"                  WHAT=ci_ok CI_RUNS=""
expect fail "GitHub API error"               WHAT=ci_ok GH_RC=1
echo "check_parts.py on the artifact table:"
for t in good layout flags extra md5 trail short; do
    f="$W/$t.bin"; [ $t = good ] && f="$W/d/partitions.bin"
    want=fail; [ $t = good ] && want=pass
    python3 "$REPO/tools/check_parts.py" "$f" >/dev/null 2>&1 && got=pass || got=fail
    if [ $got = $want ]; then mark=ok; else mark=FAIL; fails=$((fails + 1)); fi
    printf '  [%4s] %-44s -> %s\n' "$mark" "$t" "$got"
done

# Local (non-Actions) packages: real local_ok/markers functions on simulated evidence.
L="$W/local"; mkdir -p "$L"
cat > "$W/local.sh" <<EOF
set -euo pipefail
$(sed -n '/^die()/p; /^LOCAL_REQUIRED=/p; /^local_ok()/,/^}/p; /^markers()/,/^}/p' "$REPO/tools/heltec_v2.sh")
\${WHAT:-local_ok} "\$@"
EOF
mkpkg() {   # mkpkg <name> <mutation>: evidence dir + package dir, then one mutation
    local e="$L/$1/ev" d="$L/$1/pkg" sha=0123456789abcdef0123456789abcdef01234567 env=heltec_v2_sx1276_tx_poc_dryrun
    mkdir -p "$e/artifacts/$env" "$d"
    for n in bootloader partitions firmware; do echo "$n" > "$d/$n.bin"; done
    python3 - "$e" "$d" "$sha" "$env" "$2" <<'PY'
import hashlib, json, sys
e, d, sha, env, mut = sys.argv[1:]
names = "codec frames rx log orcon_rx orcon_v3 rxpipe netcfg oled tx_poc safety-and-negative-compiles privacy poc-web".split()
names += [f"firmware-{env}", f"image-{env}", f"partitions-{env}"]
checks = [{"name": n, "result": "PASS"} for n in names]
if mut == "fail-row": checks.append({"name": "rx", "result": "FAIL"})
if mut == "not-tested": checks.append({"name": "asan", "result": "NOT TESTED"})
if mut == "missing-firmware": checks = [c for c in checks if c["name"] != f"firmware-{env}"]
r = {"commit": "f" * 40 if mut == "commit" else sha, "dirty": " M src/x.cpp" if mut == "dirty" else "", "checks": checks}
open(f"{e}/RESULTS.json", "w").write(json.dumps(r))
files = [{"name": n, "sha256": hashlib.sha256(open(f"{d}/{n}", "rb").read()).hexdigest()} for n in ("bootloader.bin", "partitions.bin", "firmware.bin")]
if mut == "artifact-hash": files[2]["sha256"] = "0" * 64
open(f"{e}/artifacts/{env}/BUILD_INFO.json", "w").write(json.dumps({"files": files}))
PY
    { echo "commit=$sha"; echo "env=$env"; echo "evidence=$e"
      echo "evidence_sha256=$(shasum -a 256 "$e/RESULTS.json" | cut -d' ' -f1)"; } > "$d/BUILD_INFO.txt"
    [ "$2" = evidence-edited ] && echo " " >> "$e/RESULTS.json"
    echo "$d $sha"
}
lexpect() {   # lexpect <pass|fail> <name> <args...>
    local want=$1 name=$2; shift 2
    out="$(env "${LENV[@]}" bash "$W/local.sh" "$@" 2>&1)"; rc=$?
    got=$([ $rc -eq 0 ] && echo pass || echo fail)
    if [ "$got" = "$want" ]; then mark=ok; else mark=FAIL; fails=$((fails + 1)); fi
    printf '  [%4s] %-44s -> %s: %s\n' "$mark" "$name" "$got" "$(grep -E 'local evidence OK|FAIL:' <<<"$out" | head -1)"
}
echo "local_ok (local build evidence instead of Actions):"
LENV=(WHAT=local_ok)
lexpect pass "clean evidence, all PASS"        $(mkpkg good none)
for m in dirty commit fail-row not-tested missing-firmware artifact-hash evidence-edited; do
    lexpect fail "evidence: $m"                $(mkpkg "$m" "$m")
done
echo "markers (image contents per environment):"
printf 'SX1276 direct RX (TX disabled) RX-ONLY TEST FIRMWARE' > "$W/rx.bin"
printf 'SX1276 direct RX (TX disabled) tx-poc-dryrun DRY-RUN ONLY / NO RF' > "$W/dry.bin"
printf 'SX1276 direct RX (TX disabled) RX-ONLY TEST FIRMWARE tx-poc-dryrun' > "$W/rxpoc.bin"
printf 'SX1276 direct RX (TX disabled) tx-poc-dryrun DRY-RUN ONLY / NO RF ArduinoOTA' > "$W/ota.bin"
LENV=(WHAT=markers)
lexpect pass "RX-only image"                   "$W/rx.bin" heltec_v2_sx1276
lexpect pass "dry-run image"                   "$W/dry.bin" heltec_v2_sx1276_tx_poc_dryrun
lexpect fail "RX-only env, dry-run image"      "$W/dry.bin" heltec_v2_sx1276
lexpect fail "RX-only image with PoC inside"   "$W/rxpoc.bin" heltec_v2_sx1276
lexpect fail "dry-run env, RX-only image"      "$W/rx.bin" heltec_v2_sx1276_tx_poc_dryrun
lexpect fail "image with OTA"                  "$W/ota.bin" heltec_v2_sx1276_tx_poc_dryrun
lexpect fail "reserved RF env"                 "$W/dry.bin" heltec_v2_sx1276_tx_poc
lexpect fail "V3 env"                          "$W/dry.bin" heltec_v3_cc1101_ota

echo; [ $fails -eq 0 ] && echo "ALL PASSED (0 failures)" || echo "FAILED ($fails failures)"
exit $((fails > 0))
