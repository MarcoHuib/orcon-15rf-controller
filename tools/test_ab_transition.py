#!/usr/bin/env python3
"""test_ab_transition.py — the A/B slots through the move to control-only releases, simulated with real otadata bytes.

The board today (docs/RELEASES.md, ~/.orcon/installed.txt of 2026-11-19; not re-read from the device): app1 runs the control
firmware b75fad3 (valid), app0 holds the older RX-only image 2edb423 (valid). The target: the newest control release runs,
the previous control release is the fallback, no RX-only image as the normal rollback.

The model writes otadata the way ESP-IDF 4.4 does (esp_ota_set_boot_partition: the other sector gets the next sequence for
the chosen slot, state NEW with rollback enabled; esp_ota_mark_app_valid_cancel_rollback: VALID;
esp_ota_mark_app_invalid_rollback_and_reboot: INVALID) and lets the bootloader step run as Arduino-ESP32 2.0.16's bootloader
does with rollback enabled (NEW -> PENDING_VERIFY and boot it; a PENDING_VERIFY entry at the next boot -> ABORTED, boot the
other). WHICH slot boots is then decided by tools/check_slots.otadata(), the reader the USB recovery already uses. The app
side follows ota_signed.cpp: an update goes to esp_ota_get_next_update_partition() (the slot that is not running), is
refused during a trial boot, confirms after 60 s when WiFi and RX are healthy and rolls back at 300 s otherwise.
Simulation only: no hardware is touched. Real-board validation is listed in docs/RELEASES.md.
"""
import struct, sys, zlib
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_slots import otadata  # noqa: E402

NEW, PENDING, VALID, INVALID, ABORTED, UNDEF = 0, 1, 2, 3, 4, 0xFFFFFFFF


class Board:
    def __init__(self, app0, app1, running, seq):
        self.slot = [app0, app1]                 # image label per slot ('' = erased or incomplete)
        self.ota = bytearray(b'\xff' * 0x2000)
        self.write_entry(0, seq, VALID)          # the current boot entry; the other sector keeps the previous one
        self.write_entry(1, seq - 1, VALID)
        assert otadata(bytes(self.ota))[1] == running, 'baseline otadata does not select the running slot'
        self.running = running
        self.boot()

    def write_entry(self, sector, seq, state):
        off = sector * 0x1000
        self.ota[off:off + 32] = struct.pack('<I20sII', seq, b'\xff' * 20, state, zlib.crc32(struct.pack('<I', seq), 0xFFFFFFFF))

    def entries(self):
        return [(struct.unpack_from('<I', self.ota, o)[0], struct.unpack_from('<I', self.ota, o + 24)[0]) for o in (0, 0x1000)]

    def active_sector(self):
        e, _ = otadata(bytes(self.ota))
        usable = [i for i, x in enumerate(e) if x[2]]
        return max(usable, key=lambda i: e[i][0]) if usable else None

    # ---- app side (ESP-IDF 4.4 esp_ota_ops.c, as ota_signed.cpp calls it) ----
    def next_update(self):
        return 1 - self.running

    def set_boot(self, slot):
        a = self.active_sector()
        if a is None: self.write_entry(0, slot + 1, NEW); return
        seq, i = self.entries()[a][0], 0
        while seq > slot + 1: i += 1; seq -= 2
        self.write_entry(1 - a, 2 * i + slot + 1, NEW)

    def state_of_running(self):
        a = self.active_sector()
        return self.entries()[a][1] if a is not None and otadata(bytes(self.ota))[1] == self.running else UNDEF

    def mark_valid(self):
        a = self.active_sector(); seq, _ = self.entries()[a]; self.write_entry(a, seq, VALID)

    def mark_invalid(self):
        a = self.active_sector(); seq, _ = self.entries()[a]; self.write_entry(a, seq, INVALID)

    # ---- bootloader step (rollback enabled) ----
    def boot(self):
        a = self.active_sector()
        if a is not None:
            seq, st = self.entries()[a]
            if st == PENDING: self.write_entry(a, seq, ABORTED)       # an unconfirmed trial: abandon it
            elif st == NEW: self.write_entry(a, seq, PENDING)         # first boot of a new image: a trial
        e, slot = otadata(bytes(self.ota))
        if not self.slot[slot]:                                        # nothing bootable there: the other slot
            slot = 1 - slot
        self.running = slot
        self.pending = self.state_of_running() == PENDING

    # ---- one update as ota_signed.cpp + fw_update_esp.cpp run it ----
    def update(self, image, *, power_cut=None, healthy=True, reset_in_trial=False, owner_key=True):
        if self.pending: return 'refused: confirm-running-image-first'
        if not owner_key: return 'refused: owner-key-first'          # nothing erased (ota_signed.cpp ownerKeyUsable)
        target = self.next_update()
        assert target != self.running
        self.slot[target] = ''                                         # sequential erase/write has started
        if power_cut == 'during-write': self.boot(); return 'power cut while writing'
        self.slot[target] = image                                      # complete; hash, markers and image check passed
        self.set_boot(target)
        if power_cut == 'before-restart': self.boot(); return self.trial(healthy, reset_in_trial)
        self.boot()                                                    # restartSoon()
        return self.trial(healthy, reset_in_trial)

    def trial(self, healthy, reset_in_trial):
        if not self.pending: return 'no trial boot'
        if reset_in_trial: self.boot(); return 'reset in trial boot'  # power cut, RST, watchdog, panic: unconfirmed
        if healthy: self.mark_valid(); self.pending = False; return 'confirmed'
        self.mark_invalid(); self.boot(); return 'rolled back (unhealthy at 300 s)'

    def view(self):
        st = {VALID: 'valid', INVALID: 'invalid', ABORTED: 'aborted', NEW: 'new', PENDING: 'pending-verify', UNDEF: '-'}
        e = otadata(bytes(self.ota))[0]
        labels = {}
        for i, (seq, state, _) in enumerate(e):
            if seq != 0xFFFFFFFF: labels[(seq - 1) % 2] = st.get(state, hex(state))
        return f"runs app{self.running} ({self.slot[self.running]}) | app0={self.slot[0] or 'leeg'} [{labels.get(0, '-')}] app1={self.slot[1] or 'leeg'} [{labels.get(1, '-')}]"


n = 0
def ok(msg):
    global n; n += 1; print('  [ ok ] ' + msg)


def board(seq=6):
    return Board('2edb423 RX-only', 'b75fad3 control', running=1, seq=seq)


for seq in (2, 4, 6, 10):   # the sequence numbers on the real board are unknown: every parity with app1 active
    b = board(seq)
    assert b.running == 1 and not b.pending, b.view()
    r = b.update('v1.0.0 control')
    assert r == 'confirmed' and b.running == 0 and b.slot == ['v1.0.0 control', 'b75fad3 control'], (seq, r, b.view())
    assert 'app1=b75fad3 control [valid]' in b.view(), b.view()
    r = b.update('v1.1.0 control')
    assert r == 'confirmed' and b.running == 1 and b.slot == ['v1.0.0 control', 'v1.1.0 control'], (seq, r, b.view())
    r = b.update('v1.2.0 control')
    assert r == 'confirmed' and b.running == 0 and b.slot == ['v1.2.0 control', 'v1.1.0 control'], (seq, r, b.view())
ok('first control release goes to app0 (replacing RX-only 2edb423); b75fad3 stays the valid fallback; later releases alternate')

b = board(); r = b.update('v1.0.0 control', healthy=False)
assert r.startswith('rolled back') and b.running == 1 and b.slot == ['v1.0.0 control', 'b75fad3 control'] and not b.pending, b.view()
assert 'app0=v1.0.0 control [invalid]' in b.view(), b.view()
r = b.update('v1.0.1 control')
assert r == 'confirmed' and b.running == 0 and b.slot[1] == 'b75fad3 control', b.view()
ok('unhealthy trial (no WiFi or RX within 300 s): back to b75fad3; the next update goes to app0 again')

for why in ('reset in trial boot',):
    b = board(); r = b.update('v1.0.0 control', reset_in_trial=True)
    assert r == why and b.running == 1 and not b.pending and 'app0=v1.0.0 control [aborted]' in b.view(), b.view()
ok('reset, power cut, panic or watchdog during the trial boot: the bootloader aborts the trial and starts b75fad3')

b = board(); r = b.update('v1.0.0 control', power_cut='during-write')
assert b.running == 1 and b.slot == ['', 'b75fad3 control'] and not b.pending, b.view()
r = b.update('v1.0.0 control')
assert r == 'confirmed' and b.running == 0 and b.slot == ['v1.0.0 control', 'b75fad3 control'], b.view()
ok('power cut while downloading/writing: boot slot unchanged (b75fad3); app0 holds no image (RX-only already erased); retry works')

b = board(); r = b.update('v1.0.0 control', power_cut='before-restart')
assert r == 'confirmed' and b.running == 0, b.view()
ok('power cut after the boot slot was set: the next start is the normal trial boot of the complete, verified image')

b = board(); b.set_boot(0); b.boot()
assert b.pending and b.update('v1.0.1 control') == 'refused: confirm-running-image-first'
ok('no update during a trial boot (ota_signed.cpp g_pending; ESP-IDF refuses esp_ota_begin too)')

b = Board('v1.0.0 RX-only', 'b75fad3 control', running=0, seq=5)   # USB recovery: this branch in app0, keyless NVS
assert b.update('v1.0.1 control', owner_key=False) == 'refused: owner-key-first' and b.slot == ['v1.0.0 RX-only', 'b75fad3 control'], b.view()
ok('no owner key: the update is refused before anything is erased, so b75fad3 (old recovery) stays in app1 (Codex legacy r7)')

b = board(); b.update('v1.0.0 control'); b.update('v1.1.0 control', healthy=False)
assert b.running == 0 and b.slot == ['v1.0.0 control', 'v1.1.0 control'] and 'app1=v1.1.0 control [invalid]' in b.view(), b.view()
ok('a failed later release falls back to the previous control release, never to an RX-only image')

print('\nOvergangen (simulatie; niet op hardware uitgevoerd):')
for title, steps in [('uitgangssituatie', []), ('v1.0.0 geslaagd', [dict()]), ('v1.0.0 ongezond', [dict(healthy=False)]),
                     ('stroom weg tijdens schrijven', [dict(power_cut='during-write')]), ('daarna v1.1.0 geslaagd', [dict(), dict()])]:
    b = board()
    for i, kw in enumerate(steps): b.update(f'v1.{i}.0 control', **kw)
    print(f'  {title:30s} {b.view()}')
print(f'A/B transition: {n} checks, 0 failures')
