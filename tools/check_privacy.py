"""check_privacy.py [files...] — fail on data that must not be published (default: all
tracked files). The repository is public: real Orcon device IDs, MAC addresses,
LAN addresses and hardware recordings stay local. Allowed are the placeholder/example
values that the original sources already use, synthetic IDs (serial 000000..000999),
locally administered example MACs (02:00:00:...), RFC 5737 documentation addresses."""
import re
import subprocess
import sys

ALLOWED_IDS = {"29:012345", "29:074565", "32:178990", "32:179422", "63:262143"}   # pre-existing examples
ALLOWED_IPS = {"0.0.0.0", "127.0.0.1", "10.0.0.5", "192.168.1.10", "192.168.4.1",
               "192.168.2.137", "192.168.2.139", "192.168.2.140"}                    # pre-existing defaults/examples
ID = re.compile(r"\b(\d\d):(\d{6})\b")
MAC = re.compile(r"\b[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}\b")
IP = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
SELF = "tools/check_privacy.py"

files = sys.argv[1:] or subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True).stdout.split()
problems = []
for f in files:
    if f == SELF:
        continue
    if f.startswith("test/fixtures/") and f.endswith(".log") and not f.split("/")[-1].startswith("synthetic-"):
        problems.append(f"{f}: fixture logs must be synthetic (synthetic-*.log)")
    try:
        text = open(f, encoding="utf-8", errors="replace").read()
    except (IsADirectoryError, FileNotFoundError):
        continue
    for n, line in enumerate(text.splitlines(), 1):
        for m in ID.finditer(line):
            if m.group(0) not in ALLOWED_IDS and int(m.group(2)) > 999:
                problems.append(f"{f}:{n}: device ID {m.group(1)}:****** is not synthetic")
        for m in MAC.finditer(line):
            if not m.group(0).lower().startswith("02:00:00:"):
                problems.append(f"{f}:{n}: MAC address (use 02:00:00:xx:xx:xx)")
        for m in IP.finditer(line):
            ip = m.group(0)
            if ip not in ALLOWED_IPS and not ip.startswith(("192.0.2.", "198.51.100.", "203.0.113.")):
                if all(0 <= int(p) <= 255 for p in ip.split(".")):
                    problems.append(f"{f}:{n}: IPv4 address (use 192.0.2.x)")
for p in problems:
    print("PRIVACY:", p)
print(f"checked {len(files)} files: {'OK' if not problems else str(len(problems)) + ' problem(s)'}")
sys.exit(1 if problems else 0)
