"""Offline, fail-closed app1 gate for the exact table checked by precheck.

  check_app1.py bounds <board-partitions.bin>        -> "<offset> <size>" of app1
  check_app1.py check <board-partitions.bin> <app1.bin>

Both modes re-validate the board's REAL partition table first; the app1 bounds
come from that table, never from a constant or the package being flashed.
Read the WHOLE slot, not just the magic byte: malformed/partially erased content
is unknown too. The only nonempty allowlist entry is the independently pinned
historical RX-only image, never the image/manifest currently being flashed.
"""
import contextlib
import hashlib
import io
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True   # -I ignores PYTHONDONTWRITEBYTECODE; keep the repo tree clean
sys.path.insert(0, str(Path(__file__).resolve().parent))   # run with python -I
import check_parts                                            # noqa: E402
from check_flash_package import ROLLBACK, ROLLBACK_APP        # noqa: E402

REFERENCE = Path(__file__).resolve().parents[1] / '.flashtools/fw' / ROLLBACK / 'firmware.bin'


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


def check(raw, size, reference=REFERENCE):
    if len(raw) != size:
        raise ValueError('app1: incomplete partition readback')
    if raw == b'\xff' * size:
        return 'EMPTY (entire slot erased; no bootable app1 fallback)'
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
        elif mode == 'check' and len(args) == 2:
            raw = read(args[1])
            print('APP1 partition SHA256=' + hashlib.sha256(raw).hexdigest())
            print('APP1 OK:', check(raw, bounds(read(args[0]))[1]))
        else:
            raise ValueError('usage: check_app1.py bounds <table> | check <table> <app1.bin>')
    except (OSError, ValueError) as exc:
        sys.exit(str(exc))
