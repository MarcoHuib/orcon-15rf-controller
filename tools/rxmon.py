"""rxmon.py — Heltec V2 serial log: echo it and summarise RX per boot.

  rxmon.py <port> <seconds> [logfile] [reset]   read the serial port
  rxmon.py --file <logfile>                     re-analyse a saved log

Sync numbers and [rxstat] counters restart at every boot, so syncs are paired
with frames ([sync N ...] <-> [rx N ...]) and counters are compared WITHIN one
boot; a boot starts at the firmware banner ("emulator starting").

Auto-reset circuit: EN is pulled low only when DTR is released while RTS is
asserted. macOS asserts both on open, so keep both asserted (no reset); to
reset, release DTR for 100 ms (IO0 stays high -> normal boot)."""
import collections
import re
import sys
import time

SYNC = re.compile(r"\[sync (\d+) (\d+) (-?\d+)dBm pre=(\w\w)\]")
RX = re.compile(r"\[rx (\d+) (\d+) (-?\d+)dBm\]\s*(.*)")
COUNTERS = ("uart", "ovf", "sync5", "sync4", "frames", "resets", "drop", "lost")


def new_boot(start, fw=None, banner=False):
    return {"start": start, "fw": fw, "banner": banner, "syncs": {}, "rx": [], "stats": []}


def feed(boots, stamp, line):
    if "emulator starting" in line:
        m = re.search(r"fw=(\S+)", line)
        boots.append(new_boot(stamp, m.group(1) if m else None, banner=True))
        return
    if not boots:
        boots.append(new_boot(stamp))          # log began while the firmware was running
    b = boots[-1]
    m = SYNC.match(line)
    if m:
        b["syncs"][int(m.group(1))] = (stamp, int(m.group(3)), m.group(4))
        return
    m = RX.match(line)
    if m:
        b["rx"].append((int(m.group(1)), stamp, int(m.group(3)), m.group(4)))
        return
    if line.startswith("[rxstat]"):
        b["stats"].append(dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv))


def summary(boots):
    print("\n===== summary =====")
    banners = sum(1 for b in boots if b["banner"])
    reboots = banners - (1 if boots and boots[0]["banner"] else 0)   # a log that starts at a boot: not a reboot
    print(f"boots in log: {len(boots)} (reboots seen: {reboots})")
    tot = collections.Counter()
    for k, b in enumerate(boots):
        done = {n for n, *_ in b["rx"] if n}
        ok = [b["syncs"][n] for n in sorted(done) if n in b["syncs"]]
        bad = [s for n, s in sorted(b["syncs"].items()) if n not in done]
        fw = b["fw"] or ("(no fw= in banner)" if b["banner"] else "(started before the log)")
        print(f"\n-- boot {k + 1} from {b['start']}, firmware {fw}")
        print(f"syncs: {len(b['syncs'])}  decoded: {len(ok)}  garbled: {len(bad)}")
        print("byte before 00 (FF = old decoder would see it):",
              dict(collections.Counter(s[2] for s in b["syncs"].values())))
        if ok:
            print("RSSI decoded:", sorted(s[1] for s in ok))
        if bad:
            print("RSSI garbled (sender unknown):", sorted(s[1] for s in bad))
        for n, stamp, rssi, frame in b["rx"]:
            print(f"   [rx {n} {rssi}dBm] {frame}")
        tot.update(syncs=len(b["syncs"]), decoded=len(ok), garbled=len(bad))
        st = b["stats"]
        if st:
            a, z = st[0], st[-1]
            keys = [c for c in COUNTERS if c in a and c in z]
            delta = {c: int(z[c]) - int(a[c]) for c in keys}
            print("rxstat first->last:", ", ".join(f"{c} {a[c]}->{z[c]}" for c in keys))
            print("delta:", delta)
            for c in ("pendmax", "qmax", "taskus", "loopmax"):
                if c in z:
                    print(f"max {c}: {max(int(x[c]) for x in st if c in x)}", end="  ")
            if "stack" in z:
                print(f"min stack free: {min(int(x['stack']) for x in st if 'stack' in x)} B  min heap: "
                      f"{z.get('minheap')} B  rx state: {z.get('rxst')} err {z.get('err')} rec {z.get('rec')}")
            print("radio: op=%s (low 3 bits 5 = RX) lna=%s rxcfg=%s rxbw=%s rssi=%s..%s dBm" % (
                z["op"], z["lna"], z["rxcfg"], z["rxbw"],
                min(int(x["rssi"]) for x in st), max(int(x["rssi"]) for x in st)))
            if any(int(x["op"], 16) & 7 != 5 for x in st):
                print("WARNING: SX1276 left RX mode at some sample:", [x["op"] for x in st])
            for c in ("ovf", "drop", "resets"):
                tot[c] += delta.get(c, 0)
    print(f"\n== total over boots: {dict(tot)}")


def from_file(path):
    boots = []
    for raw in open(path, encoding="utf-8", errors="replace"):
        stamp, _, line = raw.rstrip("\n").partition(" ")
        feed(boots, stamp, line)
    return boots


def from_serial(port, secs, logpath, reset):
    import serial
    logf = open(logpath, "a", buffering=1) if logpath else None   # line-buffered: readable while running
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = True
    s.rts = True
    s.open()
    if reset:
        s.dtr = False
        time.sleep(0.1)
        s.dtr = True
    boots, buf, end = [], b"", time.time() + secs
    while time.time() < end:
        buf += s.read(512)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").rstrip("\r")
            stamp = time.strftime("%H:%M:%S")
            print(f"{stamp} {line}", flush=True)
            if logf:
                logf.write(f"{stamp} {line}\n")
            feed(boots, stamp, line)
    return boots


if __name__ == "__main__":
    if sys.argv[1] == "--file":
        summary(from_file(sys.argv[2]))
    else:
        summary(from_serial(sys.argv[1], float(sys.argv[2]),
                            sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None,
                            len(sys.argv) > 4 and sys.argv[4] == "reset"))
