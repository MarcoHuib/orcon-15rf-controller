"""Local test for tools/nvs_keys.py with a synthetic NVS page (built here, no real device data)."""
import struct, subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nvs_keys as nk

ok = fail = 0
def check(cond, name):
    global ok, fail
    if cond: ok += 1; print('  [ok]', name)
    else: fail += 1; print('  [FAIL]', name)


def entry(ns, typ, span, chunk, key, data):
    e = bytearray(32)
    e[0:4] = bytes([ns, typ, span, chunk]); e[8:8 + len(key)] = key.encode(); e[24:32] = data.ljust(8, b'\xff')
    e[4:8] = struct.pack('<I', nk.crc(bytes(e[:4]) + bytes(e[8:])))
    return bytes(e)


def page(seq, items):
    """items: lists of 32-byte entries (a string spans several)."""
    pg = bytearray(b'\xff' * nk.PAGE)
    hdr = bytearray(b'\xff' * 32); struct.pack_into('<II', hdr, 0, nk.ACTIVE, seq); hdr[8] = 0xFE
    struct.pack_into('<I', hdr, 28, nk.crc(bytes(hdr[4:28]))); pg[0:32] = hdr
    slot = 0
    for chunk in items:
        for j in range(len(chunk) // 32):
            i = slot + j
            pg[64 + i * 32:64 + (i + 1) * 32] = chunk[j * 32:(j + 1) * 32]
            pg[32 + i // 4] &= ~(1 << (2 * (i % 4))) & 0xFF                 # bits 10 = written
        slot += len(chunk) // 32
    return bytes(pg)


def string(ns, key, text):
    raw = text.encode() + b'\0'
    data = struct.pack('<HHI', len(raw), 0xFFFF, nk.crc(raw))
    body = raw.ljust(((len(raw) + 31) // 32) * 32, b'\xff')
    return entry(ns, 0x21, 1 + len(body) // 32, 0xFF, key, data) + body


nsdef = entry(0, 0x01, 1, 0xFF, 'orcon', bytes([1]))
dump = page(1, [nsdef, entry(1, 0x01, 1, 0xFF, 'mode', bytes([2])), string(1, 'ssid', 'Testnet')]) + b'\xff' * nk.PAGE
ks = nk.keys(dump)
check(set(ks) == {('orcon', 'mode'), ('orcon', 'ssid')}, 'namespace and keys found')
check(ks[('orcon', 'ssid')][:2] == ('str', 8), 'string type and size (with terminator)')
newer = dump[:nk.PAGE] + page(2, [nsdef, entry(1, 0x01, 1, 0xFF, 'mode', bytes([3]))])
check(nk.keys(newer)[('orcon', 'mode')] != ks[('orcon', 'mode')], 'a newer page wins')
bad = bytearray(dump); bad[64 + 32 + 24] ^= 1                                # flip a value bit of "mode"
try: nk.keys(bytes(bad)); check(False, 'damaged entry refused')
except ValueError: check(True, 'damaged entry refused')
with tempfile.TemporaryDirectory() as d:
    a, b = Path(d) / 'a.bin', Path(d) / 'b.bin'
    a.write_bytes(dump); b.write_bytes(newer)
    out = subprocess.run([sys.executable, str(Path(nk.__file__)), a], capture_output=True, text=True)
    check(out.returncode == 0 and 'Testnet' not in out.stdout and 'all page and entry CRCs OK' in out.stdout, 'CLI lists keys, never values')
    out = subprocess.run([sys.executable, str(Path(nk.__file__)), '--diff', a, b], capture_output=True, text=True)
    check('~ orcon' in out.stdout and 'mode' in out.stdout, 'diff marks the changed key')
print(f'nvs keys: {ok} checks, {fail} failures'); sys.exit(1 if fail else 0)
