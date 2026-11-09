"""Offline package integrity gate; never imports serial or accesses a device.

Trust anchors were checked against the pre-existing local hardware report and
flash logs for d5c39e1, plus ESP32 headers/digests and the historical source.
These hashes attest the retained local artifact, not a bit-reproducible CI build.
"""
import hashlib
from pathlib import Path
import re
import sys

ROLLBACK = 'd5c39e1b4d1f0ed37defe69b51e3562c36058208'
PINNED = {
    'bootloader.bin': 'ef4ea64b8c5175a23427d78ce4c635a2d8d1b6811750c2597c46c5d458d2494a',
    'partitions.bin': '1d9cca96de0fe07ad7fc0648b9878ddecd9ce565e38b589ad20fea698ed4c80c',
    'boot_app0.bin': 'f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0',
}
ROLLBACK_APP = '6939546a631e76d94b684ed3964eacd37e6b8fa3897a1863d43232e30618a7db'


def check(directory, commit):
    d = Path(directory)
    names = set(PINNED) | {'firmware.bin'}
    rows = {}
    for line in (d / 'SHA256SUMS.txt').read_text().splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9_.-]+)', line)
        if not match:
            raise ValueError('malformed checksum row')
        digest, name = match.groups()
        # Historical manifest also records the unused merged image. It is checked
        # if present, but is NEVER a flash input. No paths or unknown members.
        if name not in names | {'heltec-v2-rx-only-merged.bin'} or name in rows:
            raise ValueError('unknown/duplicate checksum member')
        rows[name] = digest
    if not names <= rows.keys():
        raise ValueError('checksum manifest does not cover all four images')
    for name, digest in rows.items():
        f = d / name
        if not f.is_file() or f.is_symlink():
            raise ValueError(f'{name}: missing or symlink')
        actual = hashlib.sha256(f.read_bytes()).hexdigest()
        if actual != digest or (name in PINNED and actual != PINNED[name]):
            raise ValueError(f'{name}: SHA-256 mismatch with manifest/trust anchor')
        if name == 'firmware.bin' and commit == ROLLBACK and actual != ROLLBACK_APP:
            raise ValueError('historical rollback firmware differs from independent local pin')


if __name__ == '__main__':
    try:
        check(*sys.argv[1:])
    except (OSError, ValueError) as exc:
        sys.exit(str(exc))
    print('all package members and local trust anchors OK')
