"""test_rxmon.py — feed rxmon.py synthetic firmware output over a pty and check that
syncs and frames are paired by sync number (garbled sync in the same batch as two
good frames, [rx] lines arriving after a later sync). Needs pyserial."""
import os
import subprocess
import sys
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
master, slave = os.openpty()
stat = ("[rxstat] t={} uart=100 ovf=0 sync5=0 sync4={} frames={} resets=0 pend=2 pendmax=40 q=0 qmax=2 "
        "drop=0 taskus=900 stack=1800 rxst=0 err=0 rec=0 lost=0 op=0D lna=20 rxcfg=08 rxbw=01 rssi=-90 "
        "heap=1 minheap=1 loopmax=5000")
lines = ["Orcon 15RF emulator starting... fw=abc123", stat.format(10001, 0, 0),
         "[sync 1 12000 -88dBm pre=55]", "[sync 2 12010 -44dBm pre=D5]", "[sync 3 12060 -91dBm pre=FF]",
         "[rx 2 12100 -44dBm] I  --- 29:000101 29:000202 --:------ 22F1 003 000104",
         "[rx 3 12101 -91dBm] I  --- 29:000202 --:------ 29:000202 31D9 003 000001",
         stat.format(20001, 3, 2)]


def feed():
    time.sleep(0.8)
    for line in lines:
        os.write(master, (line + "\r\n").encode())
        time.sleep(0.05)


threading.Thread(target=feed, daemon=True).start()
out = subprocess.run([sys.executable, os.path.join(here, "rxmon.py"), os.ttyname(slave), "2.5"],
                     capture_output=True, text=True).stdout
summary = out[out.find("====="):]
print(summary)
checks = {
    "firmware banner": "firmware: abc123" in summary,
    "3 syncs, 2 decoded, 1 garbled": "syncs: 3  decoded: 2  garbled: 1" in summary,
    "RSSI per own sync": "RSSI decoded: [-91, -44]" in summary and "RSSI garbled: [-88]" in summary,
    "phase bytes": "{'55': 1, 'D5': 1, 'FF': 1}" in summary,
}
for name, ok in checks.items():
    print(f"  [{'ok' if ok else 'FAIL'}] {name}")
sys.exit(0 if all(checks.values()) else 1)
