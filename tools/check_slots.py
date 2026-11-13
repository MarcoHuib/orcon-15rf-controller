"""Offline, fail-closed board state check for the OTA-aware USB recovery (heltec_v2.sh recover).

  check_slots.py check <board-partitions.bin> <otadata.bin> <app0.bin> <app1.bin> <known.txt> [<mac> <pin>]

After a WiFi OTA the board no longer has the pristine USB state (otadata = boot_app0.bin, app1
erased), so the normal precheck refuses it. This check reads the state the way the ESP-IDF
bootloader does: which otadata entry is active and which slot it selects, then identifies the
image in EACH slot by the SHA-256 of exactly its image bytes (length from the ESP image header).
A slot must be erased, the pinned known app1 board state, or an image whose hash is listed in
<known.txt> ("<sha256> <label>" lines: local packages and images installed by tools/orcon.py).
Anything else is unknown and NO-GO. Bytes after an image are reported (a sequential OTA write
erases only the sectors it uses), never executed.
"""
import hashlib
from pathlib import Path
import struct
import sys
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_app1                                        # noqa: E402

STATES = {0: 'new', 1: 'pending-verify', 2: 'valid', 3: 'invalid', 4: 'aborted', 0xFFFFFFFF: 'undefined'}
SLOT_SIZE = 0x330000


def otadata(raw):
    """[(seq, state, usable)] for both entries, and the slot the bootloader starts (0/1).
    Mirrors bootloader_common_ota_select_valid / get_active_otadata (2 OTA apps, no factory)."""
    if len(raw) != 0x2000:
        raise ValueError('otadata: incomplete readback')
    entries = []
    for off in (0, 0x1000):
        seq, state, crc = struct.unpack_from('<I', raw, off)[0], *struct.unpack_from('<II', raw, off + 24)
        valid = seq != 0xFFFFFFFF and crc == zlib.crc32(struct.pack('<I', seq), 0xFFFFFFFF)
        entries.append((seq, state, valid and state not in (3, 4)))
    usable = [e for e in entries if e[2]]
    # Both unusable: no factory app, so the bootloader starts the first bootable OTA slot (app0).
    boot = (max(usable)[0] - 1) % 2 if usable else 0
    return entries, boot


def image_len(raw):
    """Length of the ESP32 app image at the start of raw (segments, checksum, appended SHA-256)."""
    if len(raw) < 24 or raw[0] != 0xE9 or not 1 <= raw[1] <= 16:
        return None
    off = 24
    for _ in range(raw[1]):
        if off + 8 > len(raw):
            return None
        off += 8 + struct.unpack_from('<I', raw, off + 4)[0]
    off += 16 - off % 16                       # checksum byte is the last byte of a 16-byte block
    if raw[23] == 1:
        off += 32                              # hash_appended
    return off if off <= len(raw) else None


def slot(name, raw, known, known_board=False):
    """Classify one slot; raises ValueError for anything not positively identified."""
    if len(raw) != SLOT_SIZE:
        raise ValueError(f'{name}: incomplete partition readback')
    if raw == b'\xff' * SLOT_SIZE:
        return 'EMPTY', None
    if name == 'app1' and hashlib.sha256(raw).hexdigest() == check_app1.KNOWN_BOARD_APP1['sha256']:
        return check_app1.check(raw, SLOT_SIZE, known_board=known_board), None
    n = image_len(raw)
    if n is None:
        raise ValueError(f'{name}: no complete ESP image and not erased (unknown content); NO-GO')
    h = hashlib.sha256(raw[:n]).hexdigest()
    if h not in known:
        raise ValueError(f'{name}: image {h[:16]}… ({n} B) is not a known local package or OTA install; NO-GO')
    tail = '' if raw[n:] == b'\xff' * (SLOT_SIZE - n) else ', old bytes after the image (not executed)'
    return f'IMAGE {known[h]} sha256={h[:16]}… {n} B{tail}', h


def load_known(path):
    out = {}
    for line in Path(path).read_text().splitlines():
        if line.strip():
            h, label = line.split(None, 1)
            if len(h) != 64:
                raise ValueError('known list: bad hash')
            out[h.lower()] = label.strip()
    return out


def check(table, ota, app0, app1, known, known_board=False):
    if check_app1.bounds(table) != (0x340000, SLOT_SIZE):
        raise ValueError('board table: app1 is not at the verified bounds')
    entries, boot = otadata(ota)
    lines = [f'otadata[{i}]: seq={s if s != 0xFFFFFFFF else "-"} state={STATES.get(st, hex(st))} '
             f'{"usable" if u else "ignored"}' for i, (s, st, u) in enumerate(entries)]
    slots = {}
    for name, raw in (('app0', app0), ('app1', app1)):
        desc, h = slot(name, raw, known, known_board)
        slots[name] = h
        lines.append(f'{name}: {desc}')
    if slots[f'app{boot}'] is None:
        raise ValueError(f'otadata selects app{boot}, which holds no known image; NO-GO')
    lines.append(f'BOOTS app{boot}')
    return lines


if __name__ == '__main__':
    try:
        if len(sys.argv) not in (7, 9) or sys.argv[1] != 'check':
            raise ValueError('usage: check_slots.py check <table> <otadata> <app0> <app1> <known.txt> [<mac> <pin>]')
        t, o, a0, a1 = (check_app1.read(p) for p in sys.argv[2:6])
        board = len(sys.argv) == 9 and check_app1.pinned_board(sys.argv[7], sys.argv[8])
        for line in check(t, o, a0, a1, load_known(sys.argv[6]), board):
            print(line)
        print('SLOTS OK')
    except (OSError, ValueError) as exc:
        sys.exit(str(exc))
