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
# Checked: every step of every boot; the lowest free internal RAM once Matter is ready, against a floor; the identity
# across boots (kept, unique between devices, new after R, byte-identical after a migration); every generated chain
# with CHIP's own chip-cert (validate-att-cert, apart from the device's mbedTLS). QEMU is a proxy for the board: no
# Wi-Fi, no RF, nothing commissioned; throwaway identities, inside QEMU's isolated user network only.
# Output (ignored by git): test/qemu/build/<bridge|nobridge|migration>/: serial logs, the chains, RESULT.txt.
#   tools/qemu_memtest.sh [--nobridge | --migration]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cfg=bridge
case ${1:-} in --nobridge) cfg=nobridge ;; --migration) cfg=migration ;; "") ;; *) echo "usage: $0 [--nobridge | --migration]" >&2; exit 2 ;; esac
# The boots: device:scenario:expected passes:image (uni: the universal image; mig: the migration build). The floor: the
# value this source measured in QEMU minus 2 KB (runs differ by about 1 KB); below it the component or CHIP grew.
case $cfg in
    bridge)    plan="A:N:53:uni A:N:20:uni A:D:1:uni A:X:3:uni A:R:2:uni A:F:3:uni B:N:53:uni C:I:1:uni C:N:53:uni"; floor=171000 ;;
    nobridge)  plan="A:N:46:uni"; floor=171500 ;;
    migration) plan="M:N:54:mig M:N:20:uni"; floor=171000 ;;
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
docker run --rm -v "$out:/w" --entrypoint /bin/bash "$image" -c 'rm -f /w/serial-*.log /w/dev-*.bin /w/RESULT.txt'   # files of an earlier run
idt=""
case $cfg in
    nobridge) build nobridge ";nobridge.defaults" ;;
    bridge) build bridge "" ;;
    migration)
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
n=0
for spec in $plan; do
    IFS=: read -r dev sc want img <<< "$spec"
    n=$((n + 1))
    f=$out/dev-$dev.bin
    if [ ! -f "$f" ]; then   # a new device: the image, no data
        cp "$([ "$img" = mig ] && echo "$out/flash.bin" || echo "$uni_flash")" "$f"
    elif [ "$img" = uni ] && [ $cfg = migration ]; then   # an OTA to the universal image: only the app slot changes
        python3 -I -c 'import sys
f, app = sys.argv[1], open(sys.argv[2], "rb").read()
d = bytearray(open(f, "rb").read()); assert len(app) <= 0x330000
d[0x10000:0x340000] = b"\xff" * 0x330000; d[0x10000:0x10000 + len(app)] = app
open(f, "wb").write(d)' "$f" "$uni_app"
    fi
    python3 -I -c 'import sys; f = open(sys.argv[1], "r+b"); f.seek(0x6F0000); f.write(sys.argv[2].encode()); f.close()' "$f" "$sc"
    log=serial-$n-$dev$sc.log
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
    ok &= good
    boots.append(dict(dev=dev, sc=sc, img=img, ident=ident, gen=gen, mig=mig, blobs=blobs, n=n))
    lines.append(f"{dev} {sc} {img} {'PASS' if good else 'FAIL'} | checks {done[2] + '/' + want if done else 'no DONE'} fail={done[3] if done else '-'}"
                 + (f" | ready_free={heap[1]} ready_min={heap[2]}" if heap else "")
                 + (f" | identity {ident[1]} origin {ident[2]} {ident[3]} dac {ident[5]}" if ident else "")
                 + (f" | made in {gen[2]} ms, heap {gen[3]} -> {gen[4]}, lowest {gen[5]}" if gen else "")
                 + (f" | moved onto the device: {mig[1]}" if mig else "")
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
if cfg != "migration":   # a migrated identity has no PAA on the device: its chain is the original one (checked above)
    check(f"every chain a boot showed is valid for chip-cert validate-att-cert ({len(chains)})",
          chains and all(b["chipcert"] for b in chains) and len(chains) == sum(1 for b in boots if b["ident"]))
lines.append("RESULT: " + ("PASS" if ok else "FAIL"))
(out / "RESULT.txt").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
sys.exit(0 if ok else 1)
PY
