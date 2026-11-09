"""check_tick.py < main.cpp — exit 1 unless every orcon::tick( call sits inside an
"#if !RX_ONLY" block (same rule as the CI guard). Used by heltec_v2.sh verify."""
import sys

lines = sys.stdin.read().splitlines()
bad = []
for i, line in enumerate(lines):
    if line.strip().startswith("orcon::tick("):
        opened = max((j for j in range(i) if lines[j].strip() == "#if !RX_ONLY"), default=-1)
        closed = max((j for j in range(i) if lines[j].strip() == "#endif"), default=-1)
        if opened <= closed:
            bad.append(i + 1)
if bad:
    print("orcon::tick() outside #if !RX_ONLY at main.cpp line(s):", bad)
sys.exit(1 if bad else 0)
