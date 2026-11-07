"""test_rxmon.py — rxmon.py pairing: (1) live over a pty: a garbled sync in the same
batch as two good frames; (2) offline (--file): a reboot restarts sync numbers and
counters, which must not be paired or subtracted across boots. Needs pyserial."""
import os
import subprocess
import sys
import tempfile
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
RXMON = os.path.join(here, "rxmon.py")
stat = ("[rxstat] t={} uart={} ovf=0 sync5=0 sync4={} frames={} resets=0 pend=2 pendmax=40 q=0 qmax=2 "
        "drop=0 taskus=900 stack=1800 rxst=0 err=0 rec=0 lost=0 op=0D lna=20 rxcfg=08 rxbw=01 rssi=-90 "
        "heap=1 minheap=1 loopmax=5000")
boot_a = ["Orcon 15RF emulator starting... fw=abc123", stat.format(10001, 100, 0, 0),
          "[sync 1 12000 -88dBm pre=55]", "[sync 2 12010 -44dBm pre=D5]", "[sync 3 12060 -91dBm pre=FF]",
          "[rx 2 12100 -44dBm] I  --- 29:000101 29:000202 --:------ 22F1 003 000104",
          "[rx 3 12101 -91dBm] I  --- 29:000202 --:------ 29:000202 31D9 003 000001",
          stat.format(20001, 900, 3, 2)]
boot_b = ["Orcon 15RF emulator starting... fw=abc123", stat.format(10001, 50, 0, 0),
          "[sync 1 11000 -60dBm pre=FF]", "[sync 2 11500 -83dBm pre=55]",
          "[rx 1 11020 -60dBm] I  --- 29:000202 --:------ 29:000202 31D9 003 000002",
          stat.format(20001, 400, 2, 1)]
checks = {}

# (1) live over a pty
master, slave = os.openpty()
def feed():
    time.sleep(0.8)
    for line in boot_a:
        os.write(master, (line + "\r\n").encode()); time.sleep(0.05)
threading.Thread(target=feed, daemon=True).start()
live = subprocess.run([sys.executable, RXMON, os.ttyname(slave), "2.5"], capture_output=True, text=True).stdout
live = live[live.find("====="):]
checks["live: 3 syncs, 2 decoded, 1 garbled"] = "syncs: 3  decoded: 2  garbled: 1" in live
checks["live: RSSI per own sync"] = "RSSI decoded: [-91, -44]" in live and "RSSI garbled (sender unknown): [-88]" in live
checks["live: phase bytes"] = "{'55': 1, 'D5': 1, 'FF': 1}" in live

# (2) offline, two boots: numbers restart
with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
    for line in boot_a + boot_b:
        f.write("20:00:00 " + line + "\n")
off = subprocess.run([sys.executable, RXMON, "--file", f.name], capture_output=True, text=True).stdout
os.unlink(f.name)
print(live + off)
checks["file: 2 boots, 1 reboot"] = "boots in log: 2 (reboots seen: 1)" in off
checks["file: boot 2 pairs its own sync 1"] = off.count("syncs: 2  decoded: 1  garbled: 1") == 1 and "RSSI decoded: [-60]" in off
checks["file: counters per boot, not across"] = "'uart': 800" in off and "'uart': 350" in off and "-500" not in off
checks["file: totals over boots"] = "'syncs': 5, 'decoded': 3, 'garbled': 2" in off
for name, ok in checks.items():
    print(f"  [{'ok' if ok else 'FAIL'}] {name}")
sys.exit(0 if all(checks.values()) else 1)
