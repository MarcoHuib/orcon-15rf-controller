"""List the keys in an ESP32 NVS partition dump (read with esptool), without showing values.

Checks every page header and entry CRC, then prints one line per key: namespace, key, type, size
and a short SHA-256 of the value. Two dumps can be compared with --diff: which keys were added,
removed or changed. Used to verify a configuration backup before a migration or factory reset
(docs/MIGRATIE_EIGEN_HELTEC.md). Values are never printed: they include WiFi and login secrets.

    python3 tools/nvs_keys.py nvs.bin
    python3 tools/nvs_keys.py --diff before.bin after.bin
"""
import hashlib, struct, sys, zlib

PAGE, ENTRY, ENTRIES = 4096, 32, 126
ACTIVE, FULL = 0xFFFFFFFE, 0xFFFFFFFC
TYPES = {0x01: 'u8', 0x11: 'i8', 0x02: 'u16', 0x12: 'i16', 0x04: 'u32', 0x14: 'i32', 0x08: 'u64', 0x18: 'i64',
         0x21: 'str', 0x41: 'blob', 0x42: 'blob-data', 0x48: 'blob-idx'}


def crc(b):
    return zlib.crc32(b, 0xFFFFFFFF)   # = ESP-IDF crc32_le(0xFFFFFFFF, ...), checked against real dumps


def entries(dump):
    """(page seq, namespace index, type, chunk index, key, value bytes) for every written entry."""
    if len(dump) % PAGE: raise ValueError('not a whole number of 4 KB pages')
    out = []
    for p in range(len(dump) // PAGE):
        pg = dump[p * PAGE:(p + 1) * PAGE]
        state, seq = struct.unpack_from('<II', pg)
        if state not in (ACTIVE, FULL): continue                     # empty, freeing or corrupt pages hold no live data
        if struct.unpack_from('<I', pg, 28)[0] != crc(pg[4:28]): raise ValueError(f'page {p}: header CRC mismatch')
        bitmap, i = pg[32:64], 0
        while i < ENTRIES:
            if (bitmap[i // 4] >> (2 * (i % 4))) & 3 != 2: i += 1; continue   # 2 = written
            e = pg[64 + i * ENTRY:64 + (i + 1) * ENTRY]
            ns, typ, span, chunk = e[0], e[1], e[2], e[3]
            if struct.unpack_from('<I', e, 4)[0] != crc(e[:4] + e[8:]): raise ValueError(f'page {p} entry {i}: CRC mismatch')
            key = e[8:24].split(b'\0')[0].decode('ascii', 'replace')
            if typ in (0x21, 0x42):
                size, dcrc = struct.unpack_from('<H', e, 24)[0], struct.unpack_from('<I', e, 28)[0]
                val = pg[64 + (i + 1) * ENTRY:64 + (i + 1) * ENTRY + size]
                if crc(val) != dcrc: raise ValueError(f'page {p} entry {i} ({key}): data CRC mismatch')
            elif typ == 0x48:
                val = e[24:32]
            else:
                val = e[24:24 + {1: 1, 2: 2, 4: 4, 8: 8}.get(typ & 0x0F, 8)]
            out.append((seq, ns, typ, chunk, key, val))
            i += max(span, 1)
    return out


def keys(dump):
    """{(namespace, key): (type, size, short hash)}: blob chunks joined in order, newest value wins."""
    es = sorted(entries(dump), key=lambda x: x[0])
    names = {e[5][0]: e[4] for e in es if e[1] == 0 and e[2] == 0x01}   # namespace definitions: index -> name
    blobs, out = {}, {}
    for seq, ns, typ, chunk, key, val in es:
        if ns == 0: continue
        name = names.get(ns, f'#{ns}')
        if typ == 0x42: blobs.setdefault((name, key), {})[chunk] = val; continue
        if typ == 0x48: continue
        out[(name, key)] = (TYPES.get(typ, hex(typ)), len(val), hashlib.sha256(val).hexdigest()[:12])
    for k, parts in blobs.items():
        val = b''.join(parts[c] for c in sorted(parts))
        out[k] = ('blob', len(val), hashlib.sha256(val).hexdigest()[:12])
    return out


def main(argv):
    if len(argv) == 3 and argv[0] == '--diff':
        a, b = (keys(open(f, 'rb').read()) for f in argv[1:])
        for k in sorted(set(a) | set(b)):
            mark = '+' if k not in a else '-' if k not in b else '~' if a[k] != b[k] else ' '
            print(f'{mark} {k[0]:<16} {k[1]:<16} {(b.get(k) or a[k])[0]:<5} {(b.get(k) or a[k])[1]:>5} B')
        return 0
    if len(argv) != 1: print(__doc__); return 2
    ks = keys(open(argv[0], 'rb').read())
    for (ns, key), (typ, size, h) in sorted(ks.items()):
        print(f'{ns:<16} {key:<16} {typ:<5} {size:>5} B  sha256:{h}')
    print(f'{len(ks)} keys in {len({k[0] for k in ks})} namespaces; all page and entry CRCs OK')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (OSError, ValueError) as err:
        print(f'NVS dump NOT valid: {err}'); sys.exit(1)
