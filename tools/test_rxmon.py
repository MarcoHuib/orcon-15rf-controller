"""test_rxmon.py — rxmon.py: boot segmentation, damaged-line handling, counter cross-check.
(1) live over a pty; (2) synthetic edge cases; (3) synthetic log files in
test/fixtures/rxmon/ that mirror real recordings (fictitious IDs, no hardware data).
Needs pyserial for (1)."""
import importlib.util
import io
import os
import subprocess
import sys
import threading
import time
from contextlib import redirect_stdout

sys.dont_write_bytecode = True          # no __pycache__ next to the tools
here = os.path.dirname(os.path.abspath(__file__))
fixtures = os.path.join(here, "..", "test", "fixtures", "rxmon")
spec = importlib.util.spec_from_file_location("rxmon", os.path.join(here, "rxmon.py"))
rxmon = importlib.util.module_from_spec(spec); spec.loader.exec_module(rxmon)
results = []
def check(name, ok): results.append((name, ok))

FW = "c2cf48f5a7bbadf62e8a384904ac5edd42f1b868"
BANNER = "Orcon 15RF emulator starting... fw=" + FW
ROM = ["ets Jun  8 2016 00:22:57", "", "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)",
       "configsip: 0, SPIWP:0xee", "mode:DIO, clock div:2", "load:0x3fff0030,len:1184", "entry 0x400805e4", ""]
def stat(t, s4, fr, uart=100):
    return (f"[rxstat] t={t} uart={uart} ovf=0 sync5=0 sync4={s4} frames={fr} resets=0 pend=0 pendmax=20 q=0 qmax=1 "
            f"drop=0 taskus=900 stack=2300 rxst=0 err=0 rec=0 lost=0 op=0D lna=20 rxcfg=08 rxbw=01 rssi=-90 "
            f"heap=200000 minheap=190000 loopmax=50000")
SYNC = lambda n, r=-60: f"[sync {n} {1000 + n} {r}dBm pre=D5]"
RX = lambda n, r=-60: f"[rx {n} {1010 + n} {r}dBm] I  --- 29:000202 --:------ 29:000202 31D9 003 000002"

def run(lines):
    boots = []
    for i, l in enumerate(lines, 1):
        rxmon.feed(boots, "20:00:00", l, i)
    return boots
def text(boots):
    f = io.StringIO()
    with redirect_stdout(f):
        rxmon.summary(boots)
    return f.getvalue()

# --- boot segmentation ---------------------------------------------------------
b = run(ROM + [BANNER, stat(10000, 0, 0), SYNC(1), RX(1), stat(20000, 1, 1)])
check("recording starts during boot (ROM output, blank lines) -> 1 boot, banner attached",
      len(b) == 1 and b[0]["banner"] and b[0]["fw"] == FW and "ROM" in b[0]["reason"])
b = run(["", BANNER, stat(10000, 0, 0)])
check("blank line before banner (DTR reset) -> 1 boot, no ghost segment", len(b) == 1 and b[0]["banner"])
b = run([stat(500000, 40, 3), SYNC(41), RX(41), stat(510000, 41, 4)] + ROM + [BANNER, stat(10000, 0, 0), SYNC(1), RX(1), stat(20000, 1, 1)])
check("real reboot mid-recording -> 2 boots, numbers paired within each boot",
      len(b) == 2 and set(b[0]["syncs"]) == {41} and set(b[1]["syncs"]) == {1} and [r[0] for r in b[1]["rx"]] == [1])
b = run([stat(500000, 40, 3), SYNC(41)] + ROM + [stat(10000, 0, 0), SYNC(1)])
check("reboot whose banner was lost -> still 2 boots (ROM reset line)", len(b) == 2 and not b[1]["banner"])
b = run([stat(500000, 40, 3), SYNC(41), stat(510000, 41, 3), stat(10000, 0, 0), SYNC(1)])
check("banner and ROM lines both lost -> reboot inferred from t= going back",
      len(b) == 2 and "inferred" in b[1]["reason"] and set(b[1]["syncs"]) == {1})
check("summary counts reboots = boots - 1", "boots in log: 2 (reboots seen: 1)" in text(b))

# --- damaged lines ---------------------------------------------------------------
b = run([stat(10000, 0, 0), SYNC(1), SYNC(2)[1:], RX(2), stat(20000, 2, 1)])
t = text(b)
check("sync line missing its '[' -> unreliable, frame still counted as decoded",
      len(b[0]["bad"]) == 1 and [r[0] for r in b[0]["rx"]] == [2] and "whose [sync] line is missing/unreliable: [2]" in t)
check("... and the counter check explains it instead of guessing", "explained by 1 unreliable line" in t and "garbled: 1" in t)
b = run([stat(10000, 0, 0), SYNC(1), RX(1)[:-2], stat(20000, 1, 1)])
t = text(b)
check("truncated [rx] line (payload shorter than its length) -> unreliable, not decoded",
      len(b[0]["bad"]) == 1 and not b[0]["rx"] and "garbled: 1 (or 0..1: 1 unreliable [rx]-like line(s))" in t)
check("... counter check explained, not MISMATCH", "explained by 1 unreliable line" in t)
b = run([stat(10000, 0, 0), "[rx 1 1011 -6�dBm] I  --- 29:2", stat(20000, 0, 0)[:60]])
check("replacement characters / cut [rxstat] -> unreliable and not used", len(b[0]["bad"]) == 2 and len(b[0]["stats"]) == 1)
b = run([stat(10000, 0, 0), SYNC(1), RX(1), SYNC(2), SYNC(3), stat(20000, 3, 3)])
check("frames counter disagrees with intact lines -> MISMATCH", "-> MISMATCH" in text(b))

# --- synthetic log files (shape of real recordings; fictitious IDs) ---------------
def fixture(name):
    boots = rxmon.from_file(os.path.join(fixtures, name))
    return boots, text(boots)
b, t = fixture("synthetic-boot-with-reset.log")
check("boot-with-reset (blank line, banner): 1 boot, 0 reboots, consistent",
      "boots in log: 1 (reboots seen: 0)" in t and b[0]["fw"] == "0" * 8 + "1" * 32 and "-> consistent" in t)
b, t = fixture("synthetic-rom-boot-oldformat.log")
check("rom-boot-oldformat (ROM lines + banner): 1 boot; old-format [rx] lines flagged, counters MISMATCH",
      "boots in log: 1 (reboots seen: 0)" in t and len(b[0]["bad"]) == 3 and not b[0]["rx"] and "-> MISMATCH" in t)
b, t = fixture("synthetic-reboot-mid-recording.log")
check("reboot-mid-recording: 2 boots, numbers restart, both consistent, 6 decoded in boot 2",
      "boots in log: 2 (reboots seen: 1)" in t and t.count("-> consistent") == 2 and len(b[1]["rx"]) == 6
      and set(b[0]["syncs"]) == {40, 41, 42, 43} and min(b[1]["syncs"]) == 1)
b, t = fixture("synthetic-damaged-sync-line.log")
check("damaged-sync-line ('sync 12' lost its '['): 1 decoded, 1 unreliable, explained by counters",
      len(b[0]["rx"]) == 1 and len(b[0]["bad"]) == 1 and "missing/unreliable: [12]" in t and "explained by 1 unreliable line" in t)
fx = sorted(os.listdir(fixtures))
check("fixtures are synthetic only", all(f.startswith("synthetic-") for f in fx if f.endswith(".log")))

# --- live over a pty ---------------------------------------------------------------
live_lines = [BANNER, stat(10001, 0, 0), "[sync 1 12000 -88dBm pre=55]", "[sync 2 12010 -44dBm pre=D5]",
              "[sync 3 12060 -91dBm pre=FF]",
              "[rx 2 12100 -44dBm] I  --- 29:000101 29:000202 --:------ 22F1 003 000104",
              "[rx 3 12101 -91dBm] I  --- 29:000202 --:------ 29:000202 31D9 003 000001", stat(20001, 3, 2)]
master, slave = os.openpty()
def feed():
    time.sleep(0.8)
    for line in live_lines:
        os.write(master, (line + "\r\n").encode()); time.sleep(0.05)
threading.Thread(target=feed, daemon=True).start()
live = subprocess.run([sys.executable, os.path.join(here, "rxmon.py"), os.ttyname(slave), "2.5"],
                      capture_output=True, text=True).stdout
check("live: 3 syncs, 2 decoded, 1 garbled, consistent",
      "syncs (valid lines): 3  decoded: 2  garbled: 1" in live and "-> consistent" in live)
check("live: RSSI per own sync", "RSSI decoded: [-91, -44]" in live and "RSSI garbled (sender unknown): [-88]" in live)

for name, ok in results:
    print(f"  [{'ok' if ok else 'FAIL'}] {name}")
fails = sum(not ok for _, ok in results)
print(f"\n{'ALL PASSED' if not fails else 'FAILED'} ({fails} failures)")
sys.exit(1 if fails else 0)
