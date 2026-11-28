#!/usr/bin/env bash
# QEMU test of the Matter component (docs/MATTER.md, test/qemu/): its memory, its behaviour and the identity on the
# device. Builds test/qemu/probe with the firmware's own configuration layers in the pinned ESP-Matter image plus
# Espressif QEMU (test/qemu/Dockerfile), and boots one image on several emulated 8 MB flashes ("devices"), each boot
# with a scenario (test/qemu/probe/main/probe.cpp):
#   default (the firmware's bridged fan):
#     A: N activation (an identity made on the device; names, control, a user's labels), N restart (the same identity
#        and labels), D one stored byte changed, X damage detected (Matter off, the image confirms), R the owner's new
#        identity, F Orcon's factory wipe (no identity left, the image confirms)
#     B: N a second device with the identical image: its own identity
#     C: I an activation cut off before its digest, N the next activation completes
#   --migration: an image built WITH an identity (a throwaway one, as tools/matter_identity.sh makes it) moves it onto
#     the device; then the universal image replaces it in the app slot (as an OTA does) and keeps that identity, its
#     codes and the user's labels
#   --nobridge: one activation boot with the fan without the bridge (the RAM comparison)
#   --commission: a real commissioner. chip-tool runs beside QEMU on a TAP link (one container, multicast for DNS-SD).
#     First strictly, with this device's PAA as its only trusted root: CHIP's attestation verifier must pass the DAC
#     signature over the commissioner's challenge, the PAA -> PAI -> DAC chain, the vendor and product, the nonce, and
#     then stop at exactly one error, 601: no trusted key for the certification declaration. A self-made declaration
#     has no CSA-rooted signer, so no commissioner can trust it (Apple Home shows "uncertified accessory"). Then with
#     the owner's consent, as Home's "Add anyway" (--bypass-attestation-verifier): the verifier runs again, reports only
#     601, and commissioning completes. The declaration's own signature and content are checked apart (OpenSSL's CMS
#     verify with its signer; chip-cert print-cd: vendor, product, device type, certification type, signer key id).
#     Then chip-tool reads Basic Information and writes FanMode, through Orcon's (simulated) admission. P: a migration
#     build moves its identity onto the device (N), chip-tool commissions it (K), then the app slot is rewritten with
#     the universal image, as an OTA does, and the controller still reads and writes over CASE (L). Q: an identity made
#     on the device (N), commissioned (K), joined by a second commissioner, and selectively revoked. The setup code
#     and the PAA come from the emulated flash (ESP-IDF's
#     nvs_tool): no log line carries a code.
# Checked: every step of every boot; the lowest free internal RAM once Matter is ready, against a floor; the identity
# across boots (kept, unique between devices, new after R, byte-identical after a migration); every generated chain
# with CHIP's own chip-cert (validate-att-cert, apart from the device's mbedTLS). QEMU is a proxy for the board: no
# Wi-Fi, no RF; throwaway identities, inside QEMU's isolated user network or the container's own TAP link only.
# Output (ignored by git): test/qemu/build/<bridge|nobridge|migration|commission>/: serial logs, the chains, the
# chip-tool logs, RESULT.txt.
#   tools/qemu_memtest.sh [--nobridge | --migration | --commission]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cfg=bridge
case ${1:-} in
    --nobridge) cfg=nobridge ;; --migration) cfg=migration ;; --commission) cfg=commission ;; "") ;;
    *) echo "usage: $0 [--nobridge | --migration | --commission]" >&2; exit 2 ;;
esac
# The boots: device:scenario:expected passes:image (uni: the universal image; mig: the migration build). The floor: the
# value this source measured in QEMU minus 2 KB (runs differ by about 1 KB); below it the component or CHIP grew.
case $cfg in
    bridge)    plan="A:N:53:uni A:N:20:uni A:D:1:uni A:X:3:uni A:R:2:uni A:F:3:uni B:N:53:uni C:I:1:uni C:N:53:uni"; floor=171000 ;;
    nobridge)  plan="A:N:46:uni"; floor=171500 ;;
    migration) plan="M:N:54:mig M:N:20:uni"; floor=171000 ;;
    commission) plan="P:N:54:mig P:K:8:mig P:L:7:uni Q:N:53:uni Q:K:8:uni"; floor=171000 ;;
esac
sha12() { if command -v sha256sum > /dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -c1-12; }
image=orcon-qemu-memtest:$(sha12 "$root/test/qemu/Dockerfile")
[ -f "$root/idf/components/arduino/CMakeLists.txt" ] || "$root/tools/fetch_idf_deps.sh"
docker image inspect "$image" > /dev/null 2>&1 || docker build -q -t "$image" "$root/test/qemu" > /dev/null
layers="../../../idf/sdkconfig.defaults;../../../idf/config/memory.defaults;../../../idf/config/clusters.defaults"
layers="$layers;../../../idf/config/matter.defaults;../../../idf/config/diagnostics.defaults;qemu.defaults"
build() {   # build <dir> <extra layers> [identity dir]: the probe into test/qemu/build/<dir>/idf, merged into ../flash.bin
    local d=$1 lay=$layers$2 id=${3:-}
    local mounts=(-v "$root:/repo") envs=()
    if [ -n "$id" ]; then mounts+=(-v "$id:/identity:ro"); envs+=(-e ORCON_MATTER_IDENTITY=/identity); lay="$lay;/identity/identity.defaults"; fi
    mkdir -p "$root/test/qemu/build/$d"
    # Removed inside the container: it made them (as root on a Linux host). Defaults fill a new sdkconfig only.
    docker run --rm "${mounts[@]}" ${envs[@]+"${envs[@]}"} "$image" bash -c "
      set -e
      rm -f /repo/test/qemu/build/$d/idf/sdkconfig /repo/test/qemu/build/$d/flash.bin
      cd /repo/test/qemu/probe
      idf.py -B /repo/test/qemu/build/$d/idf -D SDKCONFIG=/repo/test/qemu/build/$d/idf/sdkconfig \
             -D 'SDKCONFIG_DEFAULTS=$lay' build > /repo/test/qemu/build/$d/build.log 2>&1 || { tail -40 /repo/test/qemu/build/$d/build.log; exit 1; }
      cd /repo/test/qemu/build/$d/idf
      python -m esptool --chip esp32 merge_bin -o ../flash.bin --fill-flash-size 8MB @flash_args > /dev/null"
}
out=$root/test/qemu/build/$cfg
mkdir -p "$out"
docker run --rm -v "$out:/w" --entrypoint /bin/bash "$image" -c \
    'rm -f /w/serial-*.log* /w/dev-*.bin /w/nvs-* /w/RESULT.txt && rm -rf /w/ct-* /w/trust-*'   # an earlier run's files, chip-tool's fabrics too
idt=""
case $cfg in
    nobridge) build nobridge ";nobridge.defaults" ;;
    bridge) build bridge "" ;;
    migration | commission)
        idt=$(mktemp -d)
        trap 'rm -rf -- "$idt"' EXIT
        OUT="$idt/id" bash "$root/tools/matter_identity.sh" > /dev/null   # a throwaway "existing" identity
        build bridge ""                      # the universal image, as the firmware is built
        build migration "" "$idt/id" ;;      # the same probe with that identity embedded
esac
# The probe measures the firmware's own components: its lock is idf/dependencies.lock but for the manifest hash.
diff -q <(grep -v '^manifest_hash:' "$root/idf/dependencies.lock") <(grep -v '^manifest_hash:' "$root/test/qemu/probe/dependencies.lock") \
    > /dev/null || { echo "test/qemu/probe/dependencies.lock differs from idf/dependencies.lock: not the firmware's components" >&2; exit 1; }
uni_flash=$root/test/qemu/build/$([ $cfg = nobridge ] && echo nobridge || echo bridge)/flash.bin
uni_app=$root/test/qemu/build/$([ $cfg = nobridge ] && echo nobridge || echo bridge)/idf/orcon_qemu_memtest.bin
mig_flash=$root/test/qemu/build/migration/flash.bin
# K, L: QEMU on a TAP link with chip-tool beside it, in one container. The setup code and the PAA are read from the
# device's own Matter NVS in the emulated flash (nvs_tool), handed over in the environment and a trust-store file. A
# migrated identity keeps no PAA on the device: its trust anchors are the original identity's (PAA, CD signer).
tap_boot() {   # tap_boot <device> <scenario> <log>
    local dev=$1 sc=$2 log=$3
    python3 -I - "$out/dev-$dev.bin" "$out/nvs-$dev.bin" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
open(sys.argv[2], "wb").write(d[0x670000:0x670000 + 0xC000])   # Matter's NVS: the head of spiffs (orcon_matter_nvs.cpp)
PY
    docker run --rm --network none -v "$out:/w" --entrypoint /bin/bash "$image" -c \
        "python3 \$IDF_PATH/components/nvs_flash/nvs_partition_tool/nvs_tool.py -d minimal -f json /w/nvs-$dev.bin > /w/nvs-$dev.json"
    local setup
    setup=$(python3 -I - "$out/nvs-$dev.json" "$out/trust-$dev" "${idt:+$idt/id}" <<'PY'
import base64, json, sys
from pathlib import Path
v = {e["key"]: e["data"] for e in json.load(open(sys.argv[1])) if e["namespace"] == "orcon-id"}
t, idd = Path(sys.argv[2]), sys.argv[3]
def pem(name):
    return base64.b64decode("".join(l for l in (Path(idd) / name).read_text().splitlines() if l and not l.startswith("-----")))
if "paa-cert" in v:   # made on the device: its PAA is the only trusted root, and the declaration's signer
    roots = {"paa": base64.b64decode(v["paa-cert"]), "cd": base64.b64decode(v["paa-cert"])}
else:                 # moved onto the device: the original identity's PAA and CD signer
    roots = {"paa": pem("paa-cert.pem"), "cd": pem("cd-signer-cert.pem")}
for sub, der in roots.items():
    (t / sub).mkdir(parents=True, exist_ok=True)
    (t / sub / "root.der").write_bytes(der)
(t / "cd.der").write_bytes(base64.b64decode(v["cd"]))
print(int(v["pin"]), int(v["disc"]))
PY
)
    local pin=${setup% *} disc=${setup#* } mode
    mode=$([ "$sc" = K ] && echo 2 || echo 3)   # K asks stand 2 (Medium), L stand 3 (High)
    docker run --rm --cap-add NET_ADMIN --device /dev/net/tun --sysctl net.ipv6.conf.all.disable_ipv6=0 \
        -e PIN="$pin" -e DISC="$disc" -e SC="$sc" -e DEV="$dev" -e LOG="$log" -e MODE="$mode" -v "$out:/w" \
        --entrypoint /bin/bash "$image" -c '
      : > /w/$LOG.chiptool; : > /w/$LOG.rc; mkdir -p /w/ct-$DEV
      ip tuntap add dev tap0 mode tap || exit 1
      qemu-system-xtensa -nographic -machine esp32 -nic tap,ifname=tap0,script=no,downscript=no,model=open_eth \
        -drive file=/w/dev-$DEV.bin,if=mtd,format=raw -serial file:/w/$LOG -monitor none & q=$!
      # The link comes up once the probe runs its netif: Linux sends its own frames (router solicitation, DAD) at once,
      # and a frame before esp_eth_start() crashes ESP-IDF in esp_netif_free_rx_buffer (QEMU delivers it regardless).
      for i in $(seq 120); do grep -q "PROBE net=" /w/$LOG 2> /dev/null && break; sleep 1; done
      ip link set tap0 up || exit 1
      ct() {   # ct <label> <chip-tool arguments>: its output to the log under "== label", its exit code to .rc
        local label=$1; shift
        echo "== $label" >> /w/$LOG.chiptool
        chip-tool "$@" --storage-directory "${CT_STORE:-/w/ct-$DEV}" --timeout "${CT_TIMEOUT:-240}" >> /w/$LOG.chiptool 2>&1
        echo "$label $?" >> /w/$LOG.rc
      }
      for i in $(seq 900); do grep -qE "PROBE (window-open|operational)" /w/$LOG 2> /dev/null && break; kill -0 $q 2> /dev/null || break; sleep 1; done
      sleep 3
      if [ "$SC" = K ]; then
        ct strict pairing onnetwork-long 0x7a11 "$PIN" "$DISC" --paa-trust-store-path /w/trust-$DEV/paa
        sleep 65   # the refused attempt'"'"'s fail-safe (60 s) expires; the window stays open
        ct consent pairing onnetwork-long 0x7a11 "$PIN" "$DISC" --paa-trust-store-path /w/trust-$DEV/paa --bypass-attestation-verifier true
      fi
      ct read-vendor basicinformation read vendor-name 0x7a11 0
      ct write-fan fancontrol write fan-mode "$MODE" 0x7a11 2
      sleep 2
      ct read-fan fancontrol read fan-mode 0x7a11 2
      if [ "$DEV" = Q ] && [ "$SC" = K ]; then
        ct before operationalcredentials read commissioned-fabrics 0x7a11 0
        ct open-extra pairing open-commissioning-window 0x7a11 1 180 1000 1234
        extra_code=$(grep -Eo "Manual pairing code: \[[0-9]+\]" /w/$LOG.chiptool | tail -1 | tr -cd "0-9")
        sed -i -E "s/(Manual pairing code|SetupQRCode): \[[^]]+\]/\1: [redacted]/g" /w/$LOG.chiptool
        index=""
        if [ -n "$extra_code" ]; then
          mkdir -p /w/ct-beta-$DEV
          CT_STORE=/w/ct-beta-$DEV CT_TIMEOUT=60 ct add-beta pairing code 0x7a12 "$extra_code" \
            --commissioner-name beta --commissioner-vendor-id 65522 --use-only-onnetwork-discovery true \
            --paa-trust-store-path /w/trust-$DEV/paa --bypass-attestation-verifier true
          ct joined operationalcredentials read commissioned-fabrics 0x7a11 0
          CT_STORE=/w/ct-beta-$DEV CT_TIMEOUT=30 ct beta-index operationalcredentials read current-fabric-index 0x7a12 0 --commissioner-name beta
          index=$(grep -Eo "CurrentFabricIndex: [0-9]+" /w/$LOG.chiptool | tail -1 | cut -d " " -f 2)
        fi
        if [ -n "$index" ]; then
          ct remove-beta operationalcredentials remove-fabric "$index" 0x7a11 0
          ct remaining operationalcredentials read commissioned-fabrics 0x7a11 0
          ct alpha-still-operational fancontrol read fan-mode 0x7a11 2
          CT_STORE=/w/ct-beta-$DEV CT_TIMEOUT=12 ct beta-revoked basicinformation read vendor-name 0x7a12 0 --commissioner-name beta
          ct remove-last operationalcredentials remove-fabric 1 0x7a11 0
          CT_TIMEOUT=12 ct alpha-revoked basicinformation read vendor-name 0x7a11 0
        fi
      fi
      for i in $(seq 60); do grep -q "PROBE DONE" /w/$LOG 2> /dev/null && break; sleep 1; done
      sleep 2; kill $q 2> /dev/null; wait $q 2> /dev/null || true'
    [ "$sc" = K ] || return 0
    docker run --rm --network none -v "$out/trust-$dev:/t" --entrypoint /bin/bash "$image" -c '
      openssl x509 -inform DER -in /t/cd/root.der -out /tmp/signer.pem
      openssl cms -verify -inform DER -in /t/cd.der -certfile /tmp/signer.pem -noverify -binary -out /dev/null > /dev/null 2>&1; echo "cms-verify $?"
      openssl x509 -in /tmp/signer.pem -noout -ext subjectKeyIdentifier | tail -1 | tr -d " :" | sed "s/^/signer-skid /"
      chip-cert print-cd /t/cd.der' > "$out/$log.cd" 2>&1
}
n=0
for spec in $plan; do
    IFS=: read -r dev sc want img <<< "$spec"
    n=$((n + 1))
    f=$out/dev-$dev.bin
    if [ ! -f "$f" ]; then   # a new device: the image, no data
        cp "$([ "$img" = mig ] && echo "$mig_flash" || echo "$uni_flash")" "$f"
    elif [ "$img" = uni ] && { [ $cfg = migration ] || [ "$sc" = L ]; }; then   # an OTA to the universal image: only the app slot changes
        python3 -I -c 'import sys
f, app = sys.argv[1], open(sys.argv[2], "rb").read()
d = bytearray(open(f, "rb").read()); assert len(app) <= 0x330000
d[0x10000:0x340000] = b"\xff" * 0x330000; d[0x10000:0x10000 + len(app)] = app
open(f, "wb").write(d)' "$f" "$uni_app"
    fi
    python3 -I -c 'import sys; f = open(sys.argv[1], "r+b"); f.seek(0x6F0000); f.write(sys.argv[2].encode()); f.close()' "$f" "$sc"
    log=serial-$n-$dev$sc.log
    if [ "$sc" = K ] || [ "$sc" = L ]; then tap_boot "$dev" "$sc" "$log"; continue; fi
    docker run --rm -v "$out:/w" --entrypoint /bin/bash "$image" -c "
      qemu-system-xtensa -nographic -machine esp32 -nic user,model=open_eth -drive file=/w/dev-$dev.bin,if=mtd,format=raw \
        -serial file:/w/$log -monitor none & q=\$!
      for i in \$(seq 900); do grep -q 'PROBE DONE' /w/$log 2> /dev/null && break; kill -0 \$q 2> /dev/null || break; sleep 1; done
      sleep 3; kill \$q 2> /dev/null; wait \$q 2> /dev/null || true"
done
python3 -I - "$out" "$cfg" "$floor" "$plan" "$(git -C "$root" rev-parse --short=12 HEAD)" "$image" "$(sha12 "$uni_flash")" "${idt:+$idt/id}" <<'PY'
import base64, re, subprocess, sys
from pathlib import Path
out, cfg, floor, plan, commit, image, flash, idd = Path(sys.argv[1]), sys.argv[2], int(sys.argv[3]), sys.argv[4].split(), sys.argv[5], sys.argv[6], sys.argv[7], sys.argv[8]
ok, lines, boots = True, [f"QEMU test: {cfg}, source {commit}, the universal image {flash} on every device, floor ready_min >= {floor} B"], []
for n, spec in enumerate(plan, 1):
    dev, sc, want, img = spec.split(":")
    log = (out / f"serial-{n}-{dev}{sc}.log").read_text(errors="replace")
    done = re.search(r"PROBE DONE boot=(\S+) pass=(\d+) fail=(\d+)", log)
    ident = re.search(r"PROBE identity state=(\S+) origin=(\d+) serial=(\S+) disc=(\d+) dac=(\w+) qr=(\w+)", log)
    gen = re.search(r"PROBE generate=(\S+) ms=(\d+) heap_before=(\d+) heap_after=(\d+) heap_min=(\d+)", log)
    mig = re.search(r"PROBE migrate=(\S+)", log)
    start = re.search(r"PROBE matter-start=(\S+)", log)
    heap = re.search(r"PROBE heap ready_free=(\d+) ready_min=(\d+) largest=(\d+)", log)
    blobs = {k: h for k, h, n_ in re.findall(r"PROBE blob (\w+)=([0-9a-f]+) n=(\d+)", log) if len(h) == 2 * int(n_)}   # whole only
    fails = re.findall(r"PROBE (\S+) FAIL", log)
    good = bool(done) and int(done[2]) == int(want) and done[3] == "0"
    if sc == "N":
        good = good and bool(start) and start[1] == "ESP_OK" and bool(heap) and int(heap[2]) >= floor
    com = re.search(r"PROBE commission fabrics=(\d+) served=(\d+) ready_min=(\d+) end_min=(\d+) largest=(\d+) chip_stack_free=(\d+)", log)
    ctok = cdok = False
    if sc in "KL":   # chip-tool: each command as expected, the read and the write; K: attestation (see the header)
        raw = (out / f"serial-{n}-{dev}{sc}.log.chiptool").read_text(errors="replace")
        part = {m[1]: m[2] for m in re.finditer(r"^== (\S+)\n(.*?)(?=^== |\Z)", raw, re.M | re.S)}
        rcs = [tuple(l.rsplit(" ", 1)) for l in (out / f"serial-{n}-{dev}{sc}.log.rc").read_text().split("\n") if l]
        want_rc = ([("strict", None), ("consent", "0")] if sc == "K" else []) + [("read-vendor", "0"), ("write-fan", "0"), ("read-fan", "0")]
        multi_admin = dev == "Q" and sc == "K"
        if multi_admin:
            want_rc += [(label, "0") for label in ("before", "open-extra", "add-beta", "joined", "beta-index",
                                                   "remove-beta", "remaining", "alpha-still-operational")]
            want_rc += [("beta-revoked", None), ("remove-last", "0"), ("alpha-revoked", None)]
        ctok = [r[0] for r in rcs] == [w[0] for w in want_rc] and all(w[1] is None and r[1] != "0" or r[1] == w[1] for r, w in zip(rcs, want_rc))
        ctok = ctok and "VendorName: Zelfbouw (niet van Orcon)" in part.get("read-vendor", "") and \
            re.search(rf"FanMode: {'2' if sc == 'K' else '3'}\b", part.get("read-fan", "")) is not None
        if sc == "K":   # every check up to the declaration passed; its signer is the only one no commissioner can know
            only601 = lambda t: re.findall(r"Failed in verifying 'Attestation Information' command received from the device: err (\d+) \(([^)]*)\)", t) == \
                [("601", "Could not find matching trusted verification certificate for the certification declaration's key ID")]
            ctok = ctok and only601(part.get("strict", "")) and "Device commissioning completed with success" not in part.get("strict", "") \
                and only601(part.get("consent", "")) and "Device commissioning completed with success" in part.get("consent", "")
            cd = (out / f"serial-{n}-{dev}{sc}.log.cd").read_text(errors="replace")
            skid = re.search(r"^signer-skid ([0-9A-Fa-f]{40})$", cd, re.M)
            cdok = "cms-verify 0" in cd and bool(skid) and f"SignerKeyId value: hex:{skid[1].upper()}" in cd and all(
                re.search(rf"tag\[Context Specific\]: 0x{t:X}, type: Unsigned Fixed Point \(0x04\), value: {v}\b", cd)
                for t, v in ((0, 1), (1, 65521), (3, 43), (8, 0))) and \
                re.search(r"tag\[Anonymous\]: 0x100, type: Unsigned Fixed Point \(0x04\), value: 32768\b", cd) is not None
            ctok = ctok and cdok
        if multi_admin:
            ctok = ctok and all(
                re.search(rf"CommissionedFabrics: {count}\b", part.get(label, ""))
                for label, count in (("before", 1), ("joined", 2), ("remaining", 1))
            ) and "Device commissioning completed with success" in part.get("add-beta", "") and \
                re.search(r"CurrentFabricIndex: 2\b", part.get("beta-index", "")) is not None and all(
                    re.search(r"statusCode: 0\b", part.get(label, "")) for label in ("remove-beta", "remove-last")
                )
        good = good and ctok and bool(com) and com[1] == "1"
    ok &= good
    boots.append(dict(dev=dev, sc=sc, img=img, ident=ident, gen=gen, mig=mig, blobs=blobs, n=n))
    lines.append(f"{dev} {sc} {img} {'PASS' if good else 'FAIL'} | checks {done[2] + '/' + want if done else 'no DONE'} fail={done[3] if done else '-'}"
                 + (f" | ready_free={heap[1]} ready_min={heap[2]}" if heap else "")
                 + (f" | identity {ident[1]} origin {ident[2]} {ident[3]} dac {ident[5]}" if ident else "")
                 + (f" | made in {gen[2]} ms, heap {gen[3]} -> {gen[4]}, lowest {gen[5]}" if gen else "")
                 + (f" | moved onto the device: {mig[1]}" if mig else "")
                 + (f" | chip-tool {'attestation up to the declaration (601 only), consent, commissioned, declaration apart ' + ('PASS' if cdok else 'FAIL') + ', ' if sc == 'K' else 'over CASE, '}"
                    f"read and write {'PASS' if ctok else 'FAIL'}" if sc in "KL" else "")
                 + (f" | memory ready_min={com[3]} end_min={com[4]} largest={com[5]}, CHIP stack free {com[6]} B" if com else "")
                 + (f" | failed: {', '.join(fails)}" if fails else ""))
def check(name, cond):
    global ok
    ok &= bool(cond)
    lines.append(f"{name}: {'PASS' if cond else 'FAIL'}")
if cfg == "bridge":
    a1, a2 = boots[0], boots[1]
    check("A's identity made on the device, then kept across the restart (same key, codes, serial)",
          a1["gen"] and not a2["gen"] and a1["ident"] and a2["ident"] and a1["ident"].groups() == a2["ident"].groups())
    news = [b for b in boots if b["ident"] and (b["gen"] or b["sc"] == "R")]
    keys, codes = [b["ident"][5] for b in news], [b["ident"][6] for b in news]
    check(f"unique identities from the identical image ({len(news)}: A, A after R, B, C): keys and codes all differ",
          len(news) == 4 and len(set(keys)) == len(keys) and len(set(codes)) == len(codes))
if cfg == "commission":
    p1, pk, pl, q1, qk = boots
    check("P: the migrated identity commissioned and still the same after the update to the universal image",
          p1["mig"] and p1["mig"][1] == "ESP_OK" and all(b["ident"] for b in (p1, pk, pl)) and
          p1["ident"].groups() == pk["ident"].groups() == pl["ident"].groups())
    check("Q: the identity made on the device is the one commissioned", q1["gen"] and q1["ident"] and qk["ident"] and
          q1["ident"].groups() == qk["ident"].groups())
if cfg == "migration":
    m1, m2 = boots[0], boots[1]
    check("the migration build moved its identity onto the device, the universal image kept it (same key, codes, serial)",
          m1["mig"] and m1["mig"][1] == "ESP_OK" and m1["ident"] and m1["ident"][2] == "2" and not m2["mig"] and not m2["gen"]
          and m2["ident"] and m1["ident"].groups() == m2["ident"].groups())
    def der(p):
        t = Path(p).read_text()
        return base64.b64decode("".join(l for l in t.splitlines() if l and not l.startswith("-----")))
    same = all(b in m2["blobs"] for b in ("dac", "pai", "cd")) and bytes.fromhex(m2["blobs"]["dac"]) == der(f"{idd}/dac-cert.pem") \
        and bytes.fromhex(m2["blobs"]["pai"]) == der(f"{idd}/pai-cert.pem") and bytes.fromhex(m2["blobs"]["cd"]) == Path(f"{idd}/cd.der").read_bytes()
    check("the moved DAC, PAI and certification declaration are byte-identical to the original identity", same)
    mark = b"ORCON-MATTER-EMBEDDED-IDENTITY v1"   # orcon_matter_identity_marker: what Orcon's factory reset looks for
    apps = [(p / "idf" / "orcon_qemu_memtest.bin").read_bytes() for p in (out, out.parent / "bridge")]
    check("the migration image carries the embedded-identity marker, the universal image does not",
          mark in apps[0] and mark not in apps[1])
# Every generated chain through CHIP's chip-cert (OpenSSL), apart from the device's own mbedTLS and CHIP checks.
chains = [b for b in boots if all(k in b["blobs"] for k in ("paa", "pai", "dac"))]
for b in chains:
    d = out / f"chain-{b['n']}-{b['dev']}{b['sc']}"
    d.mkdir(exist_ok=True)
    for k in ("paa", "pai", "dac", "cd"):
        if k in b["blobs"]:
            (d / f"{k}.der").write_bytes(bytes.fromhex(b["blobs"][k]))
    r = subprocess.run(["docker", "run", "--rm", "--network", "none", "-v", f"{d}:/c:ro", "--entrypoint", "/bin/bash", image, "-c",
                        "chip-cert validate-att-cert -d /c/dac.der -i /c/pai.der -a /c/paa.der"], capture_output=True, text=True)
    b["chipcert"] = r.returncode == 0
if cfg == "bridge":   # a migrated identity has no PAA on the device: its chain is the original one (checked above)
    check(f"every chain a boot showed is valid for chip-cert validate-att-cert ({len(chains)})",
          chains and all(b["chipcert"] for b in chains) and len(chains) == sum(1 for b in boots if b["ident"]))
lines.append("RESULT: " + ("PASS" if ok else "FAIL"))
(out / "RESULT.txt").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
sys.exit(0 if ok else 1)
PY
