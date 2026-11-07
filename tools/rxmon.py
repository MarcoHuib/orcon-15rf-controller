"""rxmon.py <port> <seconds> [logfile] [reset] — read the Heltec V2 serial log,
echo it, and summarise RX: boot banner, decoded frames, [rxstat] deltas.
Opens the port with DTR/RTS released so the board is NOT reset, unless the
4th argument is 'reset' (RTS pulse = EN low, IO0 high -> normal boot)."""
import re
import sys
import time

import serial

port, secs = sys.argv[1], float(sys.argv[2])
logf = open(sys.argv[3], "a") if len(sys.argv) > 3 and sys.argv[3] else None
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = False
s.rts = False
s.open()
if len(sys.argv) > 4 and sys.argv[4] == "reset":
    s.rts = True
    time.sleep(0.1)
    s.rts = False

stats, frames, fw, buf = [], [], None, b""
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
        elif line.startswith("[rx "):
            frames.append(line)

print("\n===== summary =====")
print("firmware:", fw or "(no boot banner seen)")
print(f"decoded frames printed: {len(frames)}")
for f in frames:
    print("  ", f)
if stats:
    a, b = stats[0], stats[-1]
    keys = ["uart", "ferr", "ovf", "sync5", "sync4", "frames", "resets"]
    print("rxstat first->last:", ", ".join(f"{k} {a[k]}->{b[k]}" for k in keys))
    d = {k: int(b[k]) - int(a[k]) for k in keys}
    print("delta:", d)
    print("radio: op=%s (low 3 bits 5 = RX) lna=%s rxcfg=%s rxbw=%s rssi=%s..%s dBm" % (
        b["op"], b["lna"], b["rxcfg"], b["rxbw"],
        min(int(x["rssi"]) for x in stats), max(int(x["rssi"]) for x in stats)))
    if any(int(x["op"], 16) & 7 != 5 for x in stats):
        print("WARNING: SX1276 left RX mode at some sample:", [x["op"] for x in stats])
