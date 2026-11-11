"""test_elf_tx.py <dry-run firmware.elf> <toolchain-bin> — the reachability check must SEE a path.

SX127x::setMode is called by our RX start-up (receiveDirect), so treating it as a "transmit"
method must fail the check and name a non-RadioLib caller; the real check must pass.
"""
import contextlib
import io
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_elf_tx  # noqa: E402

elf, bindir = sys.argv[1:3]
env = 'heltec_v2_sx1276_tx_poc_dryrun'
assert check_elf_tx.analyse(elf, env, bindir), 'real check must pass'
check_elf_tx.TX = re.compile(r'^SX127x::setMode\(')
out = io.StringIO()
with contextlib.redirect_stdout(out):
    found = check_elf_tx.analyse(elf, env, bindir)
assert not found and 'non-RadioLib function reaches transmit' in out.getvalue(), out.getvalue()
print('reachability self-test OK: a known RX-path call into RadioLib is detected')
