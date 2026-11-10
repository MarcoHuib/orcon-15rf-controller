"""Synthetic full-slot app1 tests, using the already-local independent trust anchor."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_app1 import REFERENCE, bounds, check as check_slot  # noqa: E402

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


if __name__ == '__main__':
    unittest.main()
