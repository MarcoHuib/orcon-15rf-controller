#!/usr/bin/env bash
# QEMU memory and behaviour test of the Matter component (docs/MATTER.md, test/qemu/). Builds test/qemu/probe with the
# firmware's own configuration layers in the pinned ESP-Matter image plus Espressif QEMU (test/qemu/Dockerfile), boots it
# on one emulated 8 MB flash, and checks every step and the free internal RAM once Matter is ready:
#   default      the firmware's layout (bridged fan): boot 1 (names, control, a user's labels), boot 2 (labels kept)
#   --nobridge   the same fan without the bridge, one boot (the RAM comparison)
# QEMU is a proxy for the board: Matter runs over emulated Ethernet (no Wi-Fi, no RF), Orcon's admission and the unit
# are simulated, nothing is commissioned. The placeholder identity runs only inside QEMU's isolated user network.
# Output (ignored by git): test/qemu/build/<bridge|nobridge>/ with the serial logs and RESULT.txt.
#   tools/qemu_memtest.sh [--nobridge]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cfg=bridge
case ${1:-} in --nobridge) cfg=nobridge ;; "") ;; *) echo "usage: $0 [--nobridge]" >&2; exit 2 ;; esac
# Expected passes per boot, and the floor for the lowest free internal RAM (byte-addressable) once Matter is ready:
# the value this source measured in QEMU minus 2 KB (runs differ by ~150 B). Below it, the component or CHIP grew.
case $cfg in
    bridge)   boots="1:50 2:17"; floor=171000 ;;
    nobridge) boots="1:43";      floor=172500 ;;
esac
sha12() { if command -v sha256sum > /dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -c1-12; }
image=orcon-qemu-memtest:$(sha12 "$root/test/qemu/Dockerfile")
[ -f "$root/idf/components/arduino/CMakeLists.txt" ] || "$root/tools/fetch_idf_deps.sh"
docker image inspect "$image" > /dev/null 2>&1 || docker build -q -t "$image" "$root/test/qemu" > /dev/null
out=$root/test/qemu/build/$cfg
mkdir -p "$out"
rm -f "$out"/serial-*.log "$out/flash.bin" "$out/RESULT.txt" "$out/idf/sdkconfig"   # defaults only fill a new sdkconfig
layers="../../../idf/sdkconfig.defaults;../../../idf/config/memory.defaults;../../../idf/config/clusters.defaults"
layers="$layers;../../../idf/config/matter.defaults;../../../idf/config/diagnostics.defaults;qemu.defaults"
[ $cfg = nobridge ] && layers="$layers;nobridge.defaults"
docker run --rm -v "$root:/repo" "$image" bash -c "
  set -e
  cd /repo/test/qemu/probe
  idf.py -B /repo/test/qemu/build/$cfg/idf -D SDKCONFIG=/repo/test/qemu/build/$cfg/idf/sdkconfig \
         -D 'SDKCONFIG_DEFAULTS=$layers' build > /repo/test/qemu/build/$cfg/build.log 2>&1 || { tail -40 /repo/test/qemu/build/$cfg/build.log; exit 1; }
  cd /repo/test/qemu/build/$cfg/idf
  python -m esptool --chip esp32 merge_bin -o ../flash.bin --fill-flash-size 8MB @flash_args > /dev/null"
# The probe measures the firmware's own components: its lock is idf/dependencies.lock but for the manifest hash.
diff -q <(grep -v '^manifest_hash:' "$root/idf/dependencies.lock") <(grep -v '^manifest_hash:' "$root/test/qemu/probe/dependencies.lock") \
    > /dev/null || { echo "test/qemu/probe/dependencies.lock differs from idf/dependencies.lock: not the firmware's components" >&2; exit 1; }
for spec in $boots; do   # one flash: boot 2 sees what boot 1 left behind
    n=${spec%%:*}
    docker run --rm -v "$out:/w" --entrypoint /bin/bash "$image" -c "
      qemu-system-xtensa -nographic -machine esp32 -nic user,model=open_eth -drive file=/w/flash.bin,if=mtd,format=raw \
        -serial file:/w/serial-$n.log -monitor none & q=\$!
      for i in \$(seq 900); do grep -q 'PROBE DONE' /w/serial-$n.log 2> /dev/null && break; kill -0 \$q 2> /dev/null || break; sleep 1; done
      sleep 3; kill \$q 2> /dev/null; wait \$q 2> /dev/null || true"
done
python3 -I - "$out" "$cfg" "$floor" "$boots" "$(git -C "$root" rev-parse --short=12 HEAD)" <<'PY'
import re, sys
out, cfg, floor, boots, commit = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4].split(), sys.argv[5]
ok, lines = True, [f"QEMU memory test: {cfg}, source {commit}, floor ready_min >= {floor} B"]
for spec in boots:
    n, want = (int(x) for x in spec.split(":"))
    log = open(f"{out}/serial-{n}.log", errors="replace").read()
    done = re.search(r"PROBE DONE boot=(\d+) pass=(\d+) fail=(\d+)", log)
    start = re.search(r"PROBE matter-start=(\S+)", log)
    heap = re.search(r"PROBE heap ready_free=(\d+) ready_min=(\d+) largest=(\d+)", log)
    fails = re.findall(r"PROBE (\S+) FAIL", log)
    good = (done and int(done[1]) == n and int(done[2]) == want and done[3] == "0" and start and start[1] == "ESP_OK"
            and heap and int(heap[2]) >= floor)
    ok &= bool(good)
    lines.append(f"boot {n}: {'PASS' if good else 'FAIL'} | matter-start={start[1] if start else '-'} | "
                 f"checks {done[2] + '/' + str(want) if done else 'no DONE'} fail={done[3] if done else '-'} | "
                 f"ready_free={heap[1] if heap else '-'} ready_min={heap[2] if heap else '-'} largest={heap[3] if heap else '-'}"
                 + (f" | failed: {', '.join(fails)}" if fails else ""))
lines.append("RESULT: " + ("PASS" if ok else "FAIL"))
open(f"{out}/RESULT.txt", "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
sys.exit(0 if ok else 1)
PY
