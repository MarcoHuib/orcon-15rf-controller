"""check_parts.py <partitions.bin> — print the ESP32 partition table and require the
exact 8 MB layout of the Heltec V2 build (every entry: name, type, subtype, offset,
size, flags), a valid MD5 entry and 0xFF padding. heltec_v2.sh writes 0x1000/0x8000/0xe000/
0x10000 and relies on NVS at 0x9000 staying where it is."""
import hashlib
import struct
import sys

EXPECTED = [  # name, type, subtype, offset, size, flags  (default_8MB.csv, espressif32@6.7.0)
    ("nvs", 1, 0x02, 0x009000, 0x005000, 0),
    ("otadata", 1, 0x00, 0x00E000, 0x002000, 0),
    ("app0", 0, 0x10, 0x010000, 0x330000, 0),
    ("app1", 0, 0x11, 0x340000, 0x330000, 0),
    ("spiffs", 1, 0x82, 0x670000, 0x180000, 0),
    ("coredump", 1, 0x03, 0x7F0000, 0x010000, 0),
]


def check(raw):
    """Return a list of problems (empty = table is exactly the expected one)."""
    problems, parts, off = [], [], 0
    while off + 32 <= len(raw) and raw[off:off + 2] == b"\xaa\x50":
        _, ptype, sub, addr, size, label, flags = struct.unpack("<2sBBII16sI", raw[off:off + 32])
        parts.append((label.split(b"\x00", 1)[0].decode("ascii", "replace"), ptype, sub, addr, size, flags))
        off += 32
    for p in parts:
        print(f"  {p[0]:10s} type={p[1]} sub=0x{p[2]:02x} @0x{p[3]:06x} size=0x{p[4]:06x} flags={p[5]}")
    if len(raw) != 3072:
        problems.append(f"size {len(raw)} != 3072")
    if parts != EXPECTED:
        problems.append("entries differ from the expected 8 MB layout")
    md5 = raw[off:off + 32]
    if md5[:2] != b"\xeb\xeb" or md5[2:16] != b"\xff" * 14 or md5[16:] != hashlib.md5(raw[:off]).digest():
        problems.append("MD5 entry missing or wrong")
    if raw[off + 32:] != b"\xff" * (len(raw) - off - 32):
        problems.append("data after the MD5 entry (expected 0xFF padding)")
    return problems


if __name__ == "__main__":
    bad = check(open(sys.argv[1], "rb").read())
    print("partition layout OK" if not bad else "partition layout UNEXPECTED: " + "; ".join(bad))
    sys.exit(1 if bad else 0)
