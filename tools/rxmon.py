"""rxmon.py — Heltec V2 serial log: echo it and summarise RX per boot.

  rxmon.py <port> <seconds> [logfile] [reset]   read the serial port
  rxmon.py --file <logfile>                     re-analyse a saved log

Boots: a new boot starts at an ESP32 ROM reset line ("ets ..."/"rst:0x..."), at the
firmware banner (same boot when it directly follows a ROM reset), or, when both
were lost, where the firmware's own [rxstat] t= counter goes backwards (labelled
as inferred). Blank lines and other ROM output never open a boot.

Lines: only lines that match their exact format are used. A [rx] line must also
carry as many payload bytes as its length field. Anything that looks like RX
data but does not match is reported as unreliable and ignored; nothing is
reconstructed. Per boot the firmware counters (sync4, frames) cross-check the
log: sync numbers N count every sync, so missing [sync] lines show up as gaps.

Auto-reset circuit: EN is pulled low only when DTR is released while RTS is
asserted. macOS asserts both on open, so keep both asserted (no reset); to
reset, release DTR for 100 ms (IO0 stays high -> normal boot)."""
import collections
import re
import sys
import time


def open_serial(port, timeout=0.2):
    """Keep the proven Heltec V2 DTR/RTS sequence in one place; never reset."""
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, timeout
    s.dtr = True
    s.rts = True
    s.exclusive = True
    s.open()
    return s

ID = r"(?:\d\d:\d{6}|--:------)"
SYNC = re.compile(r"^\[sync (\d+) (\d+) (-?\d+)dBm pre=([0-9A-F]{2})\]$")
RX = re.compile(r"^\[rx (\d+) (\d+) (-?\d+)dBm\] (I |RQ|RP|W ) --- (%s) (%s) (%s) ([0-9A-F]{4}) (\d{3}) ([0-9A-F]*)$"
                % (ID, ID, ID))
BANNER = re.compile(r"^Orcon 15RF emulator starting\.\.\.(?: fw=([0-9a-f]{40}))?$")
ROM_RESET = re.compile(r"^(?:ets [A-Z][a-z]{2} [ \d]\d \d{4}|rst:0x[0-9a-f]+ \()")
STAT_NUM = ("t", "uart", "ovf", "sync5", "sync4", "frames", "resets", "pend")
STAT_HEX = ("op", "lna", "rxcfg", "rxbw")
LOOKS_LIKE_DATA = ("sync ", "rx ", "rxstat", "dBm", "uart=", "sync4=", "emulator starting")
COUNTERS = ("uart", "ovf", "sync5", "sync4", "frames", "resets", "drop", "lost")


def new_boot(start, reason):
    return {"start": start, "reason": reason, "fw": None, "banner": False, "data": False,
            "syncs": {}, "rx": [], "stats": [], "bad": []}


def parse_stat(line):
    parts = line.split()[1:]
    kv = dict(p.split("=", 1) for p in parts if p.count("=") == 1)
    if len(kv) != len(parts):
        return None
    try:
        for k in STAT_NUM:
            int(kv[k])
        for k in STAT_HEX:
            int(kv[k], 16)
    except (KeyError, ValueError):
        return None
    return kv


def feed(boots, stamp, line, lineno=0):
    cur = boots[-1] if boots else None
    if ROM_RESET.match(line):
        if cur is None or cur["banner"] or cur["data"]:
            boots.append(new_boot(stamp, "ESP32 reset (ROM output)"))
        return
    if "emulator starting" in line and BANNER.match(line):
        fw = BANNER.match(line).group(1)
        if cur is not None and not cur["banner"] and not cur["data"]:
            cur["banner"], cur["fw"] = True, fw            # banner of the reset just seen
        else:
            boots.append(new_boot(stamp, "firmware banner"))
            boots[-1]["banner"], boots[-1]["fw"] = True, fw
        return
    sync, rx = SYNC.match(line), RX.match(line)
    stat = parse_stat(line) if line.startswith("[rxstat] ") else None
    if rx and len(rx.group(10)) != 2 * int(rx.group(9)):
        rx = None                                          # payload shorter/longer than its length
    if not (sync or rx or stat):
        if "\ufffd" in line or any(k in line for k in LOOKS_LIKE_DATA):
            if cur is None:
                boots.append(new_boot(stamp, "log started while running"))
                cur = boots[-1]
            cur["bad"].append((lineno, line))
        return
    if cur is None:
        boots.append(new_boot(stamp, "log started while running"))
        cur = boots[-1]
    if stat and cur["stats"] and int(stat["t"]) < int(cur["stats"][-1]["t"]):
        boots.append(new_boot(stamp, "reboot inferred: firmware t= counter went back"))
        cur = boots[-1]
    cur["data"] = True
    if sync:
        n = int(sync.group(1))
        if n in cur["syncs"]:
            cur["bad"].append((lineno, line + "   <- duplicate sync number"))
        else:
            cur["syncs"][n] = (stamp, int(sync.group(3)), sync.group(4))
    elif rx:
        cur["rx"].append((int(rx.group(1)), stamp, int(rx.group(3)), line.split("] ", 1)[1]))
    else:
        cur["stats"].append(stat)


def check_counters(b):
    """Firmware counters vs log lines between the first and last [rxstat] of the boot."""
    st = b["stats"]
    if len(st) < 2:
        return "counter check: fewer than 2 [rxstat] lines in this boot"
    s0, s1 = int(st[0]["sync4"]), int(st[-1]["sync4"])
    f0, f1 = int(st[0]["frames"]), int(st[-1]["frames"])
    d0, d1 = int(st[0].get("drop", 0)), int(st[-1].get("drop", 0))
    in_win = [n for n in b["syncs"] if s0 < n <= s1]
    missing = (s1 - s0) - len(in_win)
    rx_win = sum(1 for n, *_ in b["rx"] if s0 < n <= s1)
    exp = (f1 - f0) - (d1 - d0)
    bad_sync = sum(1 for _, l in b["bad"] if "sync " in l)
    bad_rx = sum(1 for _, l in b["bad"] if "rx " in l or "dBm]" in l)
    diff = exp - rx_win
    if missing == 0 and diff == 0:
        verdict = "consistent"
    elif (missing or diff) and 0 <= missing <= bad_sync and 0 <= diff <= bad_rx:
        verdict = f"difference explained by {bad_sync + bad_rx} unreliable line(s) (not reconstructed)"
    elif missing == 0 and abs(diff) <= 1:
        verdict = "consistent (+-1 frame straddling an [rxstat] line)"
    else:
        verdict = "MISMATCH"
    return (f"counter check (syncs {s0 + 1}..{s1}): firmware counted {s1 - s0} syncs, log has {len(in_win)} "
            f"[sync] lines ({missing} missing); firmware decoded {exp} frames (after drops), log has {rx_win} "
            f"[rx] lines -> {verdict}")


def summary(boots):
    print("\n===== summary =====")
    print(f"boots in log: {len(boots)} (reboots seen: {max(0, len(boots) - 1)})")
    tot = collections.Counter()
    for k, b in enumerate(boots):
        rx_nums = {n for n, *_ in b["rx"]}
        nos = [n for n in rx_nums if n not in b["syncs"]]
        garbled = [s for n, s in sorted(b["syncs"].items()) if n not in rx_nums]
        bad_rx = sum(1 for _, l in b["bad"] if "rx " in l or "dBm]" in l)
        fw = b["fw"] or ("(no fw= in banner)" if b["banner"] else "(not in log)")
        print(f"\n-- boot {k + 1} from {b['start']} ({b['reason']}), firmware {fw}")
        lo = max(0, len(garbled) - bad_rx)          # a sync whose [rx] line was damaged looks garbled
        g = f"{len(garbled)}" + (f" (or {lo}..{len(garbled)}: {bad_rx} unreliable [rx]-like line(s))" if bad_rx and lo != len(garbled) else
                                 f" ({bad_rx} unreliable [rx]-like line(s))" if bad_rx else "")
        print(f"syncs (valid lines): {len(b['syncs'])}  decoded: {len(b['rx'])}  garbled: {g}")
        if nos:
            print(f"decoded frames whose [sync] line is missing/unreliable: {sorted(nos)} (RSSI from the [rx] line only)")
        print("byte before 00 (FF = old decoder would see it):",
              dict(collections.Counter(s[2] for s in b["syncs"].values())))
        if b["rx"]:
            print("RSSI decoded:", sorted(r for _, _, r, _ in b["rx"]))
        if garbled:
            print("RSSI garbled (sender unknown):", sorted(s[1] for s in garbled))
        for n, stamp, rssi, frame in b["rx"]:
            print(f"   [rx {n} {rssi}dBm] {frame}")
        if b["bad"]:
            print(f"UNRELIABLE lines ignored: {len(b['bad'])}")
            for lineno, line in b["bad"][:10]:
                print(f"   line {lineno}: {line[:110]!r}")
        st = b["stats"]
        if st:
            a, z = st[0], st[-1]
            keys = [c for c in COUNTERS if c in a and c in z]
            delta = {c: int(z[c]) - int(a[c]) for c in keys}
            print("rxstat first->last:", ", ".join(f"{c} {a[c]}->{z[c]}" for c in keys))
            print("delta:", delta)
            print(check_counters(b))
            for c in ("pendmax", "qmax", "taskus", "loopmax"):
                if c in z:
                    print(f"max {c}: {max(int(x[c]) for x in st if c in x)}", end="  ")
            if "stack" in z:
                print(f"min stack free: {min(int(x['stack']) for x in st if 'stack' in x)} B  min heap: "
                      f"{z.get('minheap')} B  rx state: {z.get('rxst')} err {z.get('err')} rec {z.get('rec')}")
            print("radio: op=%s (low 3 bits 5 = RX) lna=%s rxcfg=%s rxbw=%s rssi=%s..%s dBm" % (
                z["op"], z["lna"], z["rxcfg"], z["rxbw"],
                min(int(x["rssi"]) for x in st if "rssi" in x), max(int(x["rssi"]) for x in st if "rssi" in x)))
            if any(int(x["op"], 16) & 7 != 5 for x in st):
                print("WARNING: SX1276 left RX mode at some sample:", [x["op"] for x in st])
            for c in ("ovf", "drop", "resets"):
                tot[c] += delta.get(c, 0)
        tot.update(syncs=len(b["syncs"]), decoded=len(b["rx"]), unreliable=len(b["bad"]))
    print(f"\n== total over boots: {dict(tot)}")


def from_file(path):
    boots = []
    with open(path, encoding="utf-8", errors="replace") as source:
        for lineno, raw in enumerate(source, 1):
            stamp, _, line = raw.rstrip("\n").partition(" ")
            feed(boots, stamp, line, lineno)
    return boots


def from_serial(port, secs, logpath, reset):
    logf = open(logpath, "a", buffering=1) if logpath else None   # line-buffered: readable while running
    s = open_serial(port)
    if reset:
        s.dtr = False
        time.sleep(0.1)
        s.dtr = True
    boots, buf, end, lineno = [], b"", time.time() + secs, 0
    while time.time() < end:
        buf += s.read(512)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").rstrip("\r")
            stamp = time.strftime("%H:%M:%S")
            lineno += 1
            print(f"{stamp} {line}", flush=True)
            if logf:
                logf.write(f"{stamp} {line}\n")
            feed(boots, stamp, line, lineno)
    # an unterminated last line is incomplete: not analysed
    s.close()
    if logf:
        logf.close()
    return boots


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "soak":
        from pathlib import Path
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from rxmon_soak import main
        main(sys.argv[2:])
    elif sys.argv[1] == "--file":
        summary(from_file(sys.argv[2]))
    else:
        summary(from_serial(sys.argv[1], float(sys.argv[2]),
                            sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None,
                            len(sys.argv) > 4 and sys.argv[4] == "reset"))
