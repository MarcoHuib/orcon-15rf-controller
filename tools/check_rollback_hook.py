"""check_rollback_hook.py <nm> <firmware.elf>: the app's verifyRollbackLater must be the linked one.

Arduino-ESP32 declares verifyRollbackLater() weak with C linkage and, when it returns false,
initArduino() confirms a PENDING_VERIFY image at boot. ota_signed.cpp must therefore provide a
strong C symbol (type T); a weak (W) symbol means the core default won and rollback is inactive.
"""
import subprocess
import sys

out = subprocess.run([sys.argv[1], sys.argv[2]], capture_output=True, text=True, check=True).stdout
kinds = {line.split()[1] for line in out.splitlines() if line.split()[-1:] == ['verifyRollbackLater'] and len(line.split()) == 3}
if kinds != {'T'}:
    sys.exit(f'verifyRollbackLater symbol types {sorted(kinds)}: the core default (W) confirms OTA images at boot; need T')
print('verifyRollbackLater: strong app definition linked (trial boot active)')
