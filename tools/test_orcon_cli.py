"""Local tests for tools/orcon.py: manifest, signature (openssl, temporary key), RF gate,
log gap detection and OTA confirmation/rollback verdicts. No board, no network."""
import hashlib, json, os, re, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import orcon

ok = fail = 0
def check(cond, name):
    global ok, fail
    if cond: ok += 1; print('  [ok]', name)
    else: fail += 1; print('  [FAIL]', name)

with tempfile.TemporaryDirectory() as t:
    t = Path(t)
    fw = t / 'firmware.bin'; fw.write_bytes(b'\xe9' + os.urandom(5000))
    text = orcon.build_manifest('02:00:00:00:00:0A', 'heltec_v2_sx1276', 'a' * 40, fw, 'b' * 32)
    m = json.loads(text)
    check(m == {'v': 1, 'mac': '02:00:00:00:00:0a', 'env': 'heltec_v2_sx1276', 'variant': 'rx-only', 'allow_tx': False,
                'commit': 'a' * 40, 'size': 5001, 'sha256': hashlib.sha256(fw.read_bytes()).hexdigest(), 'nonce': 'b' * 32},
          'RX-only manifest: board, variant, no TX, size and hash of the exact file')
    rf = json.loads(orcon.build_manifest('02:00:00:00:00:0a', 'heltec_v2_sx1276_rftest', 'a' * 40, fw, 'b' * 32))
    check(rf['variant'] == 'rf-test' and rf['allow_tx'] is True, 'RF test manifest states TX explicitly')
    try: orcon.build_manifest('02:00:00:00:00:0a', 'heltec_v2_sx1276_webctl', 'a' * 40, fw, 'b' * 32); check(False, 'uncapped RF env refused')
    except ValueError: check(True, 'uncapped RF env refused')
    try: orcon.rf_gate('heltec_v2_sx1276_rftest', ''); check(False, 'RF image needs the phrase')
    except SystemExit: check(True, 'RF image needs the phrase')
    orcon.rf_gate('heltec_v2_sx1276_rftest', orcon.RF_ACK); orcon.rf_gate('heltec_v2_sx1276', '')
    check(True, 'phrase accepted; RX-only needs none')
    ctl = json.loads(orcon.build_manifest('02:00:00:00:00:0a', 'heltec_v2_sx1276_control', 'a' * 40, fw, 'b' * 32))
    check(ctl['variant'] == 'control' and ctl['allow_tx'] is True and ctl['env'] == 'heltec_v2_sx1276_control',
          'control manifest: its own variant, states TX explicitly')
    for env, phrase in (('heltec_v2_sx1276_control', orcon.CONTROL_ACK), ('heltec_v2_sx1276_rftest', orcon.RF_ACK)):
        other = orcon.RF_ACK if phrase == orcon.CONTROL_ACK else orcon.CONTROL_ACK
        try: orcon.rf_gate(env, other); check(False, f'{env}: the phrase of the other image is refused')
        except SystemExit: check(True, f'{env}: the phrase of the other image is refused')
        orcon.rf_gate(env, phrase); check(True, f'{env}: its own phrase accepted')
    check(orcon.ENV_ACK['heltec_v2_sx1276_control'] == ('ORCON_CONTROL_ACK', 'control firmware'), 'control phrase is passed to heltec_v2.sh')
    req = json.loads(orcon.reset_request('02:00:00:00:00:0A', 'c' * 32))
    check(req == {'v': 1, 'kind': 'orcon-auth-reset', 'mac': '02:00:00:00:00:0a', 'nonce': 'c' * 32},
          'recovery request: exactly v, kind, this board (lower case) and the nonce')
    check(set(req) & {'env', 'variant', 'size', 'sha256'} == set(), 'a recovery request is never an OTA manifest')
    # signature round trip with a throw-away key, verified the way the device does (SHA-256 ECDSA, DER)
    key = t / 'k.pem'
    subprocess.run(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', str(key)], check=True); os.chmod(key, 0o600)
    sig = orcon.sign(text, key)
    pub = t / 'pub.pem'; subprocess.run(['openssl', 'ec', '-in', str(key), '-pubout', '-out', str(pub)], check=True, capture_output=True)
    (t / 's.der').write_bytes(bytes.fromhex(sig)); (t / 'm.txt').write_text(text)
    v = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(pub), '-signature', str(t / 's.der'), str(t / 'm.txt')], capture_output=True, text=True)
    check('Verified OK' in v.stdout, 'signature verifies with the public key')
    (t / 'm.txt').write_text(text.replace('rx-only', 'rf-test'))
    v = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(pub), '-signature', str(t / 's.der'), str(t / 'm.txt')], capture_output=True, text=True)
    check('Verified OK' not in v.stdout, 'a changed manifest no longer verifies')
    os.chmod(key, 0o644)
    try: orcon.sign(text, key); check(False, 'world-readable key refused')
    except SystemExit: check(True, 'world-readable key refused')
    # the committed public key is a valid P-256 key
    pem = ''.join(re.findall(r'"([^"]*)\\n"', Path('src/ota_pubkey.h').read_text())).replace('-----BEGIN PUBLIC KEY-----', '-----BEGIN PUBLIC KEY-----\n').replace('-----END', '\n-----END')
    (t / 'repo_pub.pem').write_text(pem + '\n')
    r = subprocess.run(['openssl', 'ec', '-pubin', '-in', str(t / 'repo_pub.pem'), '-text', '-noout'], capture_output=True, text=True)
    out = r.stdout + r.stderr
    check(r.returncode == 0 and ('prime256v1' in out or 'P-256' in out), 'firmware public key is P-256')

# console collector: continuity, board-side loss, restart
c = orcon.Collector()
got = c.page({'ev': [{'s': 1}, {'s': 2}], 'next': 3}, 't1'); check(len(got) == 2 and not c.gaps and c.cursor == 2, 'events in order, cursor advances')
c.page({'ev': [{'s': 5}], 'next': 6}, 't2'); check(c.gaps and c.gaps[-1]['kind'] == 'gap' and '3..4' in c.gaps[-1]['detail'], 'missing events reported')
c.gaps.clear(); c.page({'ev': [{'s': 6}], 'next': 7, 'lost': 4}, 't3'); check(c.gaps and c.gaps[-1]['kind'] == 'lost', 'board buffer overflow reported')
c.gaps.clear(); c.page({'ev': [{'s': 1}], 'next': 2}, 't4'); check(c.gaps and c.gaps[-1]['kind'] == 'restart' and c.cursor == 0, 'board restart detected, cursor reset')

# OTA verdicts with a fake clock
def run(states, **kw):
    seq, now = list(states), [0.0]
    def get():
        s = seq.pop(0) if seq else seq_last[0]; seq_last[0] = s
        if isinstance(s, Exception): raise s
        return 200, s
    seq_last = [None]
    return orcon.watch_confirmation(get, 'n' * 40, 'o' * 40, lambda: now[0], lambda d: now.__setitem__(0, now[0] + d), **kw)
import urllib.error
OLD = {'fw': 'o' * 40, 'running': 'app0', 'running_state': 'valid'}
NEW_P = {'fw': 'n' * 40, 'running': 'app1', 'running_state': 'pending-verify'}
NEW_V = dict(NEW_P, running_state='valid')
check(run([OLD, urllib.error.URLError('down'), NEW_P, NEW_V]).startswith('CONFIRMED'), 'reboot, trial boot, confirmed')
check(run([NEW_P, NEW_P, OLD]).startswith('ROLLED BACK'), 'new image ran, then the old one again: rollback reported')
check(run([OLD] * 30).startswith('ROLLED BACK'), 'new image never came up: reported as rollback after the grace time')
check(run([NEW_P] * 200).startswith('NO CONFIRMATION'), 'stuck in trial: no false success')
check(run([OLD, NEW_V]).startswith('NO TRIAL BOOT'), 'valid on first sight (core confirmed it at boot): not reported as success')
calls = []
check(run([OLD, NEW_P, NEW_P, NEW_P, OLD], on_trial=lambda: calls.append(1) or True).startswith('ROLLED BACK') and calls == [1],
      'rollback test: hold sent exactly once in the trial boot, then the old image is reported')
calls.clear(); run([OLD, NEW_V], on_trial=lambda: calls.append(1) or True)
check(calls == [], 'rollback test: no hold for an image that is already confirmed')
calls.clear()
check(run([OLD, NEW_P, NEW_P, NEW_P, OLD], on_trial=lambda: calls.append(1) or len(calls) == 2).startswith('ROLLED BACK') and calls == [1, 1],
      'rollback test: a failed hold is retried at the next poll of the trial boot, and not after it was accepted')
calls.clear()
check(run([OLD, NEW_P, NEW_V], on_trial=lambda: calls.append(1) or False).startswith('CONFIRMED') and calls == [1],
      'rollback test: a hold that never lands ends as CONFIRMED (the CLI then warns), no retry after the trial')
# Two images of ONE commit (RX-only rollback image in app0, control image written to app1): only the
# slot tells them apart. Found on hardware 2026-11-13: a real rollback was reported as CONFIRMED.
SAME_OLD = {'fw': 'n' * 40, 'variant': 'rx-only', 'running': 'app0', 'running_state': 'valid'}
SAME_P = {'fw': 'n' * 40, 'variant': 'control', 'running': 'app1', 'running_state': 'pending-verify'}
SAME_V = dict(SAME_P, running_state='valid')
slot = dict(target='app1', old_slot='app0')
check(run([SAME_OLD, SAME_P, SAME_P, SAME_OLD], **slot).startswith('ROLLED BACK'), 'same commit: back in the old slot is a rollback, not a success')
check(run([SAME_OLD, SAME_P, SAME_V], **slot).startswith('CONFIRMED'), 'same commit: confirmed only from the target slot')
check(run([SAME_OLD] * 30, **slot).startswith('ROLLED BACK'), 'same commit: never left the old slot -> rollback after the grace time')
check(not run([SAME_OLD, SAME_OLD, SAME_OLD], limit=10, **slot).startswith('CONFIRMED'), 'same commit: the old slot is never taken for the new image')
# Rollback test: the board accepts the hold only from a logged-in owner (Codex SEC-01). The trial boot is a
# fresh boot, so the CLI logs in there first and sends the session cookie, its CSRF token and the origin.
SID = '0123456789abcdef' * 2
check(orcon.cookie_sid([f'orcon_sid={SID}; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000']) == SID, 'login: session id from Set-Cookie')
check(orcon.cookie_sid(['orcon_sid=; Max-Age=0', 'other=1']) == '' and orcon.cookie_sid([f'orcon_sid={SID}0']) == '', 'login: no or malformed cookie -> none')
seq = []
hdr = {'Cookie': f'orcon_sid={SID}', 'X-Orcon-Token': 'f' * 32, 'Origin': 'http://orcon15rf.local'}
hold = orcon.rollback_hold('orcon15rf.local', 'pw-123456', login_fn=lambda h, pw: seq.append(('login', h, pw)) or hdr,
                           post=lambda h, path, body, headers=None: seq.append(('post', path, body, headers)) or (200, {'ok': True}))
check(seq == [], 'rollback test: nothing is sent before the trial boot')
check(hold() is True and seq == [('login', 'orcon15rf.local', 'pw-123456'), ('post', '/api/ota/hold', {}, hdr)],
      'rollback test: log in inside the trial boot, then hold with cookie, CSRF token and origin')
def refused(h, pw): raise SystemExit('login refused (HTTP 401 bad-password)')
def down(h, path, body, headers=None): raise urllib.error.URLError('timed out')
check(orcon.rollback_hold('b', 'pw', login_fn=refused)() is False, 'rollback test: a refused login is "not held yet", never an abort')
check(orcon.rollback_hold('b', 'pw', login_fn=lambda h, pw: hdr, post=down)() is False, 'rollback test: a network error is "not held yet"')
check(orcon.rollback_hold('b', 'pw', login_fn=lambda h, pw: hdr, post=lambda *a, **k: (409, {'error': 'not-a-trial-boot'}))() is False,
      'rollback test: a hold the board refused is not counted as held')
# ota-install --test-rollback on firmware without the owner model: the password is checked BEFORE anything is
# written, because a hold that cannot log in would let the test image confirm itself.
import argparse, getpass, types
def ota_args(wt): return argparse.Namespace(host='b', worktree=str(wt), env='heltec_v2_sx1276', yes=True, tx_ack='', test_rollback=True)
def fake_run(cmd, **kw): return types.SimpleNamespace(stdout='c' * 40 + '\n' if cmd[0] == 'git' else 'VERIFIED', stderr='', returncode=0)
saved = orcon.subprocess.run, orcon.http, orcon.login, getpass.getpass
try:
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / '.flashtools').mkdir(); (Path(d) / '.flashtools' / 'board.mac').write_text('02:00:00:00:00:01\n')
        order = []
        before = {'signed_ota': 'ORCON-SIGNED-OTA v1', 'running_state': 'valid', 'fw': 'o' * 40, 'running': 'app1'}
        orcon.subprocess.run, getpass.getpass = fake_run, lambda prompt='': 'typo'
        orcon.http = lambda h, path, body=None, **k: order.append(path) or ((200, before) if path == '/api/ota' else (403, {}))
        orcon.login = refused
        try: orcon.cmd_ota_install(ota_args(d)); check(False, 'test-rollback: wrong password stops before writing')
        except SystemExit as e: check('login refused' in str(e) and order == ['/api/ota'], 'test-rollback: wrong password stops before writing')
        order.clear(); orcon.login = lambda h, pw: order.append(('login', pw)) or {}
        try: orcon.cmd_ota_install(ota_args(d))
        except SystemExit: pass
        check(order == ['/api/ota', ('login', 'typo'), '/api/ota/nonce'], 'test-rollback: the password is verified before the nonce request')
finally:
    orcon.subprocess.run, orcon.http, orcon.login, getpass.getpass = saved
# backup-check: the board against an NVS dump; the output names fields, never values
import zlib
def profile_blob(items, active):
    b = b'OCRP\x01' + bytes([len(items)]) + (9).to_bytes(4, 'little') + active.to_bytes(4, 'little')
    for pid, name, remote, unit in items:
        b += pid.to_bytes(4, 'little') + name.encode().ljust(49, b'\0') + remote.to_bytes(4, 'little') + unit.to_bytes(4, 'little') + bytes(66)
    return b + zlib.crc32(b).to_bytes(4, 'little')
vals = {('orcon', 'uc'): (37).to_bytes(4, 'little'), ('orcon', 'us'): (123).to_bytes(4, 'little'),
        ('orcon', 'fc'): (29).to_bytes(4, 'little'), ('orcon', 'fs'): (456).to_bytes(4, 'little'),
        ('orcon', 'ssid'): b'Thuisnet\0', ('orcon', 'oled_mode'): b'auto\0',
        ('orconprofiles', 'data'): profile_blob([(2, 'Huis', 32 << 18 | 7, 29 << 18 | 456)], 2)}
bv = orcon.backup_view(vals)
check(bv == {'own': '37:000123', 'unit': '29:000456', 'oled': 'auto', 'ssid': 'Thuisnet', 'active': 2,
             'profiles': [{'id': 2, 'name': 'Huis', 'remote': '32:000007', 'unit': '29:000456'}]},
      'backup-check: IDs, WiFi name, OLED mode and profiles decoded from the dump as the API shows them')
api = {'/api/state': {'us_id': '37:000123', 'fan_id': '29:000456', 'oled_mode': 'auto'}, '/api/netcfg': {'ssid': 'Thuisnet'},
       '/api/profiles': {'active': 2, 'items': [{'id': 2, 'name': 'Huis', 'remote': '32:000007', 'unit': '29:000456', 'control_mapping': True}]}}
board = orcon.board_view('b', hdr, get=lambda h, path, headers=None: (200, api[path]))
rows = orcon.backup_rows(bv, board)
check(len(rows) == 6 and all(r.startswith('gelijk') for r in rows), 'backup-check: an identical board gives "gelijk" on every field')
board['profiles'][0]['unit'] = '29:000457'
rows = orcon.backup_rows(bv, board)
check([r.split()[0] for r in rows] == ['gelijk', 'gelijk', 'ANDERS', 'gelijk', 'gelijk', 'gelijk'], 'backup-check: a changed profile is reported')
check(not any(v in ''.join(rows) for v in ('37:000123', '29:000456', '29:000457', 'Thuisnet', 'Huis', '32:000007')),
      'backup-check: no ID, name or network name in the output')
blob = vals[('orconprofiles', 'data')]; bad = dict(vals); bad[('orconprofiles', 'data')] = blob[:-1] + bytes([blob[-1] ^ 1])
try: orcon.backup_view(bad); check(False, 'backup-check: a damaged profile blob is refused')
except ValueError: check(True, 'backup-check: a damaged profile blob is refused')
# Release bundles (v2): one signature for every board; the image must be built as that release.
with tempfile.TemporaryDirectory() as d:
    fw = Path(d) / 'firmware.bin'; fw.write_bytes(b'\xe9' + os.urandom(3000) + b'ORCON-RELEASE 7;' + os.urandom(100))
    m = json.loads(orcon.release_manifest('heltec_v2_sx1276_control', 'c' * 40, fw, 7))
    check(m == {'v': 2, 'kind': 'orcon-release', 'env': 'heltec_v2_sx1276_control', 'variant': 'control', 'allow_tx': True, 'commit': 'c' * 40,
                'version': 7, 'size': fw.stat().st_size, 'sha256': hashlib.sha256(fw.read_bytes()).hexdigest()},
          'release manifest: no board, no nonce; variant, TX statement, version, size and hash of the exact image')
    for bad, why in ((lambda: orcon.release_manifest('heltec_v2_sx1276_rftest', 'c' * 40, fw, 7), 'the RF test image is never released'),
                     (lambda: orcon.release_manifest('heltec_v2_sx1276_control', 'c' * 40, fw, 8), 'an image built as another release is refused'),
                     (lambda: orcon.release_manifest('heltec_v2_sx1276_control', 'c' * 40, fw, -1), 'a negative version is refused')):
        try: bad(); check(False, why)
        except ValueError: check(True, why)
    b = Path(d) / 'x.orconfw'; text = json.dumps(m, separators=(',', ':'))
    orcon.write_bundle(b, text, 'ab' * 40, fw)
    raw = b.read_bytes(); nl = raw.index(b'\n'); head = json.loads(raw[:nl])
    check(head == {'manifest': text, 'sig': 'ab' * 40} and raw[nl + 1:] == fw.read_bytes() and raw[:nl].isascii(),
          'bundle: one ASCII JSON line {manifest, sig}, a newline, then exactly the image (what the web page splits)')
# Install record for heltec_v2.sh recover: "<sha256 of the image> <label>", private file, appended
with tempfile.TemporaryDirectory() as d:
    fw = Path(d) / 'fw.bin'; fw.write_bytes(b'\xe9image'); rec = Path(d) / 'o' / 'installed.txt'
    orcon.record_install(rec, fw, 'abc1234-heltec_v2_sx1276 ota t1'); orcon.record_install(rec, fw, 'again')
    lines = rec.read_text().splitlines()
    check(len(lines) == 2 and lines[0] == hashlib.sha256(b'\xe9image').hexdigest() + ' abc1234-heltec_v2_sx1276 ota t1',
          'install record: one line per install, hash of the exact image')
    check(rec.stat().st_mode & 0o077 == 0 and rec.parent.stat().st_mode & 0o077 == 0, 'install record: private (0600 in 0700)')
print(f'orcon cli: {ok} checks, {fail} failures'); sys.exit(1 if fail else 0)
