"""check_parts.py <partitions.bin> — print the ESP32 partition table and check
the offsets heltec_v2.sh writes to: app at 0x10000 (ota_0/factory), otadata at
0xe000 and NVS at 0x9000 (kept intact by flashing per-offset)."""
import struct
import sys

raw = open(sys.argv[1], "rb").read()
parts = {}
for off in range(0, len(raw), 32):
    e = raw[off:off + 32]
    if e[:2] != b"\xaa\x50":
        break
    _, ptype, sub, addr, size, label, _ = struct.unpack("<2sBBII16sI", e)
    name = label.rstrip(b"\x00").decode()
    parts[name] = (ptype, sub, addr, size)
    print(f"  {name:10s} type={ptype} sub=0x{sub:02x} @0x{addr:06x} size=0x{size:06x}")

ok = (parts.get("nvs", (None, None, 0))[2] == 0x9000
      and parts.get("otadata", (None, None, 0))[2] == 0xE000
      and any(p[0] == 0 and p[2] == 0x10000 for p in parts.values()))
print("partition layout OK" if ok else "partition layout UNEXPECTED")
sys.exit(0 if ok else 1)
