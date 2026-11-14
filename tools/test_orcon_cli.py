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
check(run([OLD, NEW_P, NEW_P, NEW_P, OLD], on_trial=lambda: calls.append(1)).startswith('ROLLED BACK') and calls == [1],
      'rollback test: hold sent exactly once in the trial boot, then the old image is reported')
calls.clear(); run([OLD, NEW_V], on_trial=lambda: calls.append(1))
check(calls == [], 'rollback test: no hold for an image that is already confirmed')
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
hold()
check(seq == [('login', 'orcon15rf.local', 'pw-123456'), ('post', '/api/ota/hold', {}, hdr)],
      'rollback test: log in inside the trial boot, then hold with cookie, CSRF token and origin')
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
