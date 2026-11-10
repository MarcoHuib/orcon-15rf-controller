"""Offline, fail-closed app1 gate for the exact table checked by precheck.

  check_app1.py bounds <board-partitions.bin>        -> "<offset> <size>" of app1
  check_app1.py check <board-partitions.bin> <app1.bin> [<reported-MAC> <board.mac pin file>]

Both modes re-validate the board's REAL partition table first; the app1 bounds
come from that table, never from a constant or the package being flashed.
Read the WHOLE slot, not just the magic byte: malformed/partially erased content
is unknown too. The only nonempty allowlist entry is the independently pinned
historical RX-only image, never the image/manifest currently being flashed.
"""
import contextlib
import hashlib
import io
import re
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True   # -I ignores PYTHONDONTWRITEBYTECODE; keep the repo tree clean
sys.path.insert(0, str(Path(__file__).resolve().parent))   # run with python -I
import check_parts                                            # noqa: E402
from check_flash_package import ROLLBACK, ROLLBACK_APP        # noqa: E402

REFERENCE = Path(__file__).resolve().parents[1] / '.flashtools/fw' / ROLLBACK / 'firmware.bin'
# The ONE accepted exception, observed on the pinned Heltec V2 by the 2026-11-10 precheck
# (evidence 20261110T103905Z): app1 erased except 16 bytes at the start of its last sector.
# No image header, so the bootloader can never start it. Valid only with this exact hash,
# layout and table, and only for the board in the local MAC pin (the MAC is never committed).
KNOWN_BOARD_APP1 = {
    'sha256': '937d903c5f3dcc1248708a26b84df9207e145de02f76f0abda7016ef730d38a9',
    'bounds': (0x340000, 0x330000),
    'window': (0x32F000, 0x32F010),   # slot-relative: 0x66F000-0x66F00F
}


def bounds(table):
    """(offset, size) of app1 from a board table that is exactly the verified layout."""
    with contextlib.redirect_stdout(io.StringIO()):
        problems = check_parts.check(table)
    if problems:
        raise ValueError('app1: board partition table not verified: ' + '; '.join(problems))
    rows = [struct.unpack_from('<2sBBII', table, o) for o in range(0, len(table) - 31, 32)]
    app1 = [(r[3], r[4]) for r in rows if r[0] == b'\xaa\x50' and r[1:3] == (0, 0x11)]
    if len(app1) != 1:
        raise ValueError('app1: board table has no unique app1 (ota_1) entry')
    return app1[0]


def check(raw, size, reference=REFERENCE, known_board=False):
    """known_board: caller verified the exact KNOWN bounds and the pinned board MAC."""
    if len(raw) != size:
        raise ValueError('app1: incomplete partition readback')
    if raw == b'\xff' * size:
        return 'EMPTY (entire slot erased; no bootable app1 fallback)'
    if hashlib.sha256(raw).hexdigest() == KNOWN_BOARD_APP1['sha256']:
        a, b = KNOWN_BOARD_APP1['window']
        if not known_board or size != KNOWN_BOARD_APP1['bounds'][1]:
            raise ValueError('app1: known non-bootable state accepted only on the pinned board/table; NO-GO')
        if raw[:a] != b'\xff' * a or raw[b:] != b'\xff' * (size - b) or b'\xff' in raw[a:b] or raw[0] == 0xe9:
            raise ValueError('app1: known state layout mismatch; NO-GO')
        return (f'KNOWN NON-BOOTABLE board state SHA256={KNOWN_BOARD_APP1["sha256"]} '
                f'(16 bytes at slot+0x{a:x}, rest erased, no image header); RECHECK-SECTOR 0x{a & ~0xfff:x}')
    kind = 'potential ESP image' if raw[0] == 0xe9 else 'unknown/corrupt data'
    reference = Path(reference)
    if reference.is_symlink() or not reference.is_file():
        raise ValueError('app1: independently approved RX-only reference unavailable')
    safe = reference.read_bytes()
    if hashlib.sha256(safe).hexdigest() != ROLLBACK_APP:
        raise ValueError('app1: approved RX-only reference hash mismatch')
    if not 0 < len(safe) <= size or raw[:len(safe)] != safe:
        raise ValueError(f'app1: {kind} does not match the full approved RX-only image; NO-GO')
    if raw[len(safe):] != b'\xff' * (size - len(safe)):
        raise ValueError('app1: non-erased trailing content; NO-GO')
    return f'SAFE RX-only {ROLLBACK} SHA256={ROLLBACK_APP} bytes={len(safe)}; erased tail'


def pinned_board(reported, pin_file):
    """The reported MAC must equal the LOCAL pin file itself, not an environment override."""
    pin = Path(pin_file)
    if pin.is_symlink() or not pin.is_file():
        return False
    norm = lambda m: m.strip().lower()
    return bool(re.fullmatch(r'([0-9a-f]{2}:){5}[0-9a-f]{2}', norm(reported))) and norm(reported) == norm(pin.read_text())


def read(name):
    path = Path(name)
    if path.is_symlink():
        raise ValueError(f'app1: symlink {name} refused')
    return path.read_bytes()


if __name__ == '__main__':
    try:
        mode, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ('', [])
        if mode == 'bounds' and len(args) == 1:
            offset, size = bounds(read(args[0]))
            print(f'0x{offset:x} 0x{size:x}')
        elif mode == 'check' and len(args) in (2, 4):
            raw = read(args[1])
            print('APP1 partition SHA256=' + hashlib.sha256(raw).hexdigest())
            found = bounds(read(args[0]))
            board = len(args) == 4 and found == KNOWN_BOARD_APP1['bounds'] and pinned_board(args[2], args[3])
            print('APP1 OK:', check(raw, found[1], known_board=board))
        else:
            raise ValueError('usage: check_app1.py bounds <table> | check <table> <app1.bin> [<mac> <pin>]')
    except (OSError, ValueError) as exc:
        sys.exit(str(exc))
