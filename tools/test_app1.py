"""Synthetic full-slot app1 tests, using the already-local independent trust anchor."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_app1 import KNOWN_BOARD_APP1, REFERENCE, bounds, check as check_slot  # noqa: E402

TABLE = (REFERENCE.parent / 'partitions.bin').read_bytes()   # pinned in check_flash_package
APP1_SIZE = 0x330000


def check(raw, reference=REFERENCE):
    return check_slot(raw, APP1_SIZE, reference)


def table(entries):
    body = b''.join(entries)
    out = body + b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(body).digest()
    return out + b'\xff' * (3072 - len(out))


class App1Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.safe = REFERENCE.read_bytes()
        cls.slot = cls.safe + b'\xff' * (APP1_SIZE - len(cls.safe))

    def test_empty_entire_slot(self):
        self.assertIn('EMPTY', check(b'\xff' * APP1_SIZE))

    def test_safe_entire_image_and_tail(self):
        self.assertIn('SAFE RX-only', check(self.slot))

    def test_valid_but_unknown_image(self):
        # Header-only change leaves the ESP payload checksum intact; regenerate
        # the appended image SHA too. A valid image is not a safety attestation.
        unknown = bytearray(self.safe)
        unknown[2] ^= 1
        unknown[-32:] = hashlib.sha256(unknown[:-32]).digest()
        with self.assertRaisesRegex(ValueError, 'full approved'):
            check(bytes(unknown) + b'\xff' * (APP1_SIZE - len(unknown)))

    def test_corrupt_header(self):
        with self.assertRaises(ValueError):
            check(b'\x00' + self.slot[1:])

    def test_corrupt_payload(self):
        corrupt = bytearray(self.slot); corrupt[1000] ^= 1
        with self.assertRaises(ValueError):
            check(bytes(corrupt))

    def test_corrupt_appended_digest(self):
        corrupt = bytearray(self.slot); corrupt[len(self.safe) - 1] ^= 1
        with self.assertRaises(ValueError):
            check(bytes(corrupt))

    def test_partly_erased(self):
        with self.assertRaises(ValueError):
            check(b'\xff' * 4096 + self.slot[4096:])

    def test_tail_not_erased(self):
        with self.assertRaisesRegex(ValueError, 'trailing'):
            check(self.slot[:-1] + b'\x00')

    def test_unknown_without_magic(self):
        with self.assertRaises(ValueError):
            check(b'\x00' * APP1_SIZE)

    def test_short_or_oversize_read(self):
        for data in (b'', self.safe, self.slot[:-1], self.slot + b'\xff'):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                check(data)

    def test_unavailable_or_changed_reference(self):
        with tempfile.TemporaryDirectory() as tmp:
            ref = Path(tmp) / 'ref.bin'
            with self.assertRaises(ValueError):
                check(self.slot, ref)
            ref.write_bytes(b'unapproved image')
            with self.assertRaises(ValueError):
                check(self.slot, ref)
            ref.unlink(); ref.symlink_to(REFERENCE)
            with self.assertRaises(ValueError):
                check(self.slot, ref)


class BoundsTests(unittest.TestCase):
    def test_bounds_from_verified_table(self):
        self.assertEqual(bounds(TABLE), (0x340000, 0x330000))

    def test_unverified_tables_refused(self):
        ent = [TABLE[i:i + 32] for i in range(0, 6 * 32, 32)]
        moved = bytearray(ent[3]); struct.pack_into('<I', moved, 4, 0x350000)
        small = bytearray(ent[3]); struct.pack_into('<I', small, 8, 0x100000)
        sub = bytearray(ent[3]); sub[3] = 0x12
        for name, data in {'app1 moved': table(ent[:3] + [bytes(moved)] + ent[4:]),
                           'app1 shrunk': table(ent[:3] + [bytes(small)] + ent[4:]),
                           'no ota_1': table(ent[:3] + [bytes(sub)] + ent[4:]),
                           'app1 missing': table(ent[:3] + ent[4:]),
                           'short': TABLE[:2048], 'bad md5': TABLE[:6 * 32] + b'\x00' + TABLE[6 * 32 + 1:]}.items():
            with self.subTest(name), self.assertRaises(ValueError):
                bounds(data)

    def test_cli_isolated(self):
        tool = Path(__file__).resolve().parent / 'check_app1.py'
        with tempfile.TemporaryDirectory() as tmp:
            t, a = Path(tmp) / 'parts.bin', Path(tmp) / 'app1.bin'
            t.write_bytes(TABLE); a.write_bytes(b'\xff' * APP1_SIZE)
            run = lambda *x: subprocess.run([sys.executable, '-I', str(tool), *x], capture_output=True, text=True)
            self.assertEqual(run('bounds', str(t)).stdout, '0x340000 0x330000\n')
            self.assertIn('EMPTY', run('check', str(t), str(a)).stdout)
            a.write_bytes(b'\x00' * APP1_SIZE)
            bad = run('check', str(t), str(a))
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn('APP1 partition SHA256=', bad.stdout)
            self.assertNotEqual(run('check', str(t)).returncode, 0)


class KnownBoardTests(unittest.TestCase):
    """The single accepted non-bootable state, from the board's own 2026-11-10 readback (local only)."""
    FIXTURE = REFERENCE.parents[2] / 'board-app1/app1-937d903c.bin'

    @classmethod
    def setUpClass(cls):
        cls.raw = cls.FIXTURE.read_bytes()
        assert hashlib.sha256(cls.raw).hexdigest() == KNOWN_BOARD_APP1['sha256'], 'fixture changed'
        cls.a, cls.b = KNOWN_BOARD_APP1['window']

    def test_layout_of_the_observed_state(self):
        raw, a, b = self.raw, self.a, self.b
        self.assertEqual((a, b), (0x32F000, 0x32F010))
        self.assertEqual(raw[:a] + raw[b:], b'\xff' * (APP1_SIZE - 16))
        self.assertNotIn(b'\xff', raw[a:b])
        self.assertTrue(all(raw[o] != 0xe9 for o in range(0, APP1_SIZE, 4096)))   # no image header anywhere

    def test_accepted_only_on_pinned_board(self):
        self.assertIn('KNOWN NON-BOOTABLE', check_slot(self.raw, APP1_SIZE, known_board=True))
        self.assertIn('RECHECK-SECTOR 0x32f000', check_slot(self.raw, APP1_SIZE, known_board=True))
        with self.assertRaisesRegex(ValueError, 'pinned board'):
            check_slot(self.raw, APP1_SIZE)

    def test_every_deviating_byte_is_no_go(self):
        offsets = list(range(self.a - 1, self.b + 1)) + [0, 1, 0x1000, 0x200000, APP1_SIZE - 1]
        for off in offsets:
            for value in {self.raw[off] ^ 1, 0x00, 0xff, 0xe9} - {self.raw[off]}:
                bad = bytearray(self.raw); bad[off] = value
                with self.subTest(offset=hex(off), value=value), self.assertRaises(ValueError):
                    check_slot(bytes(bad), APP1_SIZE, known_board=True)

    def test_window_moved_or_wrong_size(self):
        moved = self.raw[4096:] + b'\xff' * 4096          # same 16 bytes, one sector earlier
        for data, size in ((moved, APP1_SIZE), (self.raw[4096:], APP1_SIZE - 4096), (self.raw + b'\xff' * 4096, APP1_SIZE + 4096)):
            with self.subTest(size=size), self.assertRaises(ValueError):
                check_slot(data, size, known_board=True)

    def test_cli_board_binding(self):
        tool = Path(__file__).resolve().parent / 'check_app1.py'
        ent = [TABLE[i:i + 32] for i in range(0, 6 * 32, 32)]
        moved = bytearray(ent[3]); struct.pack_into('<I', moved, 4, 0x350000)
        with tempfile.TemporaryDirectory() as tmp:
            t, a, pin = Path(tmp) / 'parts.bin', Path(tmp) / 'app1.bin', Path(tmp) / 'board.mac'
            t.write_bytes(TABLE); a.write_bytes(self.raw); pin.write_text('02:00:00:00:00:08\n')
            run = lambda *x: subprocess.run([sys.executable, '-I', str(tool), 'check', *map(str, x)], capture_output=True, text=True)
            ok = run(t, a, '02:00:00:00:00:08', pin)
            self.assertEqual(ok.returncode, 0, ok.stderr); self.assertIn('RECHECK-SECTOR 0x32f000', ok.stdout)
            self.assertEqual(run(t, a, '02:00:00:00:00:08', pin).returncode, 0)
            for args in ((t, a), (t, a, '02:00:00:00:00:09', pin), (t, a, 'not-a-mac', pin),
                         (t, a, '02:00:00:00:00:08', Path(tmp) / 'missing')):
                with self.subTest(args=args[2:]):
                    self.assertNotEqual(run(*args).returncode, 0)
            link = Path(tmp) / 'link.mac'; link.symlink_to(pin)
            self.assertNotEqual(run(t, a, '02:00:00:00:00:08', link).returncode, 0)
            t.write_bytes(table(ent[:3] + [bytes(moved)] + ent[4:]))   # different table
            self.assertNotEqual(run(t, a, '02:00:00:00:00:08', pin).returncode, 0)


if __name__ == '__main__':
    unittest.main()
