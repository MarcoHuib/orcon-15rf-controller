"""rxmon.py <port> <seconds> [logfile] [reset] — read the Heltec V2 serial log,
echo it, and summarise RX: boot banner, syncs vs decoded frames, [rxstat] deltas.

Auto-reset circuit: EN is pulled low only when DTR is released while RTS is
asserted. macOS asserts both on open, so keep both asserted (no reset); to
reset, release DTR for 100 ms (IO0 stays high -> normal boot)."""
import collections
import re
import sys
import time

import serial

port, secs = sys.argv[1], float(sys.argv[2])
logf = open(sys.argv[3], "a") if len(sys.argv) > 3 and sys.argv[3] else None
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = True
s.rts = True
s.open()
if len(sys.argv) > 4 and sys.argv[4] == "reset":
    s.dtr = False
    time.sleep(0.1)
    s.dtr = True

stats, events, fw, buf = [], [], None, b""
end = time.time() + secs
while time.time() < end:
    buf += s.read(512)
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        line = raw.decode("utf-8", "replace").rstrip("\r")
        stamp = time.strftime("%H:%M:%S")
        print(f"{stamp} {line}", flush=True)
        if logf:
            logf.write(f"{stamp} {line}\n")
        m = re.search(r"fw=(\S+)", line)
        if m:
            fw = m.group(1)
        if line.startswith("[rxstat]"):
            stats.append(dict(kv.split("=", 1) for kv in line.split()[1:]))
        m = re.match(r"\[(sync|rx) (\d+) (-?\d+)dBm(?: pre=(\w\w))?\]\s*(.*)", line)
        if m:
            events.append((m.group(1), int(m.group(2)), int(m.group(3)), m.group(4), m.group(5)))

print("\n===== summary =====")
print("firmware:", fw or "(no boot banner seen)")
# A sync counts as decoded when the next event is an [rx] within 500 ms.
syncs = [(i, e) for i, e in enumerate(events) if e[0] == "sync"]
ok = [e for i, e in syncs
      if i + 1 < len(events) and events[i + 1][0] == "rx" and events[i + 1][1] - e[1] < 500]
bad = [e for i, e in syncs if e not in ok]
print(f"syncs: {len(syncs)}  decoded: {len(ok)}  garbled: {len(bad)}")
print("byte before 00 (FF = old decoder would see it):",
      dict(collections.Counter(e[3] for _, e in syncs)))
if ok:
    print("RSSI decoded:", sorted(e[2] for e in ok))
if bad:
    print("RSSI garbled:", sorted(e[2] for e in bad))
for e in events:
    if e[0] == "rx":
        print(f"   [rx {e[1]} {e[2]}dBm] {e[4]}")
if stats:
    a, b = stats[0], stats[-1]
    keys = [k for k in ("uart", "ovf", "sync5", "sync4", "frames", "resets") if k in a]
    print("rxstat first->last:", ", ".join(f"{k} {a[k]}->{b[k]}" for k in keys))
    print("delta:", {k: int(b[k]) - int(a[k]) for k in keys})
    print("radio: op=%s (low 3 bits 5 = RX) lna=%s rxcfg=%s rxbw=%s rssi=%s..%s dBm" % (
        b["op"], b["lna"], b["rxcfg"], b["rxbw"],
        min(int(x["rssi"]) for x in stats), max(int(x["rssi"]) for x in stats)))
    if any(int(x["op"], 16) & 7 != 5 for x in stats):
        print("WARNING: SX1276 left RX mode at some sample:", [x["op"] for x in stats])
