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
    check(not hasattr(orcon, 'reset_request') and not hasattr(orcon, 'cmd_auth_code'),
          'no password recovery with the firmware signing key (auth-code removed)')
    try: orcon.main(['auth-code']); check(False, 'auth-code is no longer a command')
    except SystemExit as e: check(e.code == 2, 'auth-code is no longer a command')
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

# Trial-boot watch: preflight, transport, boots and console pages, outcome and verdict, serial close, no secrets in a log
import http.client, http.server, threading
OK_OTA = {'running': 'app1', 'boot': 'app1', 'running_state': 'valid'}
OK_DIAG = {'wifi': {'up': True, 'rssi': -70}, 'rx': {'state': 0}}
DONE, FP = {'setup': {'state': 'done'}}, '1a2b-3c4d-5e6f'
check(orcon.preflight(OK_OTA, OK_DIAG, DONE, FP) == [], 'trial watch: preflight GO on a confirmed image with an owner key attested by hand')
pf = orcon.preflight(OK_OTA, OK_DIAG, DONE)
check(len(pf) == 1 and 'not provable' in pf[0] and '--key-proven' in pf[0], 'trial watch: "done" alone is NO-GO (no home-network route shows the key)')
check(all(len(orcon.preflight(OK_OTA, OK_DIAG, DONE, bad)) == 1 for bad in ('', 'yes', '1A2B-3C4D-5E6F', '1a2b-3c4d-5e6', '1a2b3c4d5e6f', FP + '\n')),
      'trial watch: anything but the board\'s fingerprint format is no attestation')
pf = orcon.preflight(OK_OTA, OK_DIAG, {'setup': {'state': 'legacy'}}, FP)
check(len(pf) == 1 and 'legacy' in pf[0], 'trial watch: a legacy board (no owner key) stays NO-GO, whatever is attested')
check(len(orcon.preflight({**OK_OTA, 'boot': 'app0'}, OK_DIAG, DONE, FP)) == 1, 'trial watch: running != boot is NO-GO (migration step 7)')
check(len(orcon.preflight(OK_OTA, {'wifi': {'up': False}, 'rx': {'state': 1}}, DONE, FP)) == 2, 'trial watch: WiFi down and RX recovering are NO-GO')
check(len(orcon.preflight({}, {}, {})) == 6, 'trial watch: an unreadable board is NO-GO on every point')
k = orcon.keep('state', {'fw': 'c' * 40, 'token': 'a' * 32, 'wifi': {'ap': False, 'ip': '192.0.2.7', 'ssid': 'Thuisnet'},
                         'setup': {'state': 'done', 'x': 1}, 'profiles': [1]})
check(k == {'fw': 'c' * 40, 'wifi': {'ap': False}, 'setup': {'state': 'done'}}, 'trial watch: a log keeps only allowlisted fields (no token, address, network name)')
check(orcon.redact('uitlezen gestart (sessie 123456, 29:****56) ' + 'b' * 32 + ' ' + 'd' * 64 + ' ' + 'c' * 40) ==
      'uitlezen gestart (sessie <redacted>, 29:****56) <redacted> <redacted> ' + 'c' * 40, 'trial watch: session numbers and 32/64-hex runs are redacted, commits stay')

# The transport, against a real local socket: GET only, Host only, no redirect, no proxy, a short body is no reply
class Board(http.server.BaseHTTPRequestHandler):
    seen = []
    def do_GET(self):
        Board.seen.append((self.command, self.path, sorted(h.lower() for h in self.headers.keys())))
        if self.path == '/moved': self.send_response(302); self.send_header('Location', 'http://192.0.2.1/x'); self.send_header('Content-Length', '0'); self.end_headers(); return
        body = b'{"ok":true}' if self.path != '/short' else b'{"ok":'
        self.send_response(200); self.send_header('Content-Length', str(len(body)) if self.path != '/short' else '64'); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
srv = http.server.HTTPServer(('127.0.0.1', 0), Board); threading.Thread(target=srv.serve_forever, daemon=True).start()
saved = {k: os.environ.get(k) for k in ('http_proxy', 'HTTP_PROXY', 'no_proxy', 'NO_PROXY')}
os.environ.update(http_proxy='http://user:pw@192.0.2.9:3128', HTTP_PROXY='http://user:pw@192.0.2.9:3128', no_proxy='', NO_PROXY='')
try:
    g = orcon.trial_get('127.0.0.1', srv.server_port)
    check(g('/api/ota') == (200, {'ok': True}) and Board.seen[-1] == ('GET', '/api/ota', ['host']), 'trial watch: a direct GET with the Host header only, no proxy')
    check(g('/moved') == (302, None) and len(Board.seen) == 2, 'trial watch: a redirect is not followed')
    check(g('/short')[1] is None, 'trial watch: a cut-off reply is no reply')
finally:
    srv.shutdown()
    for k, v in saved.items():
        if v is None: os.environ.pop(k, None)
        else: os.environ[k] = v
def boom(path): raise http.client.IncompleteRead(b'x')
check(orcon.read_round(boom, 0) == {n: ('IncompleteRead', None) for n in ('console', 'ota', 'diag', 'state')}, 'trial watch: a reply cut by a reboot is a named error, not a crash')

# Boots and console pages (the firmware's ring: a cursor at or past "next" restarts from 1, else it is used as given)
def ring(events, now, lost=0):
    def page(after):
        n = len(events) + 1; after = 0 if after >= n else after
        return {'now': now, 'next': n, 'oldest': 1, 'lost': lost, 'more': False, 'ev': [{'s': i + 1, 't': 0, 'k': 'INFO', 'm': m} for i, m in enumerate(events)][after:]}
    return page
def rnd(w, slot, state, up, boots, con=None, hold=False, wifi=True, rx=0, nxt='invalid', status=200):
    ota = {'fw': 'x' * 40, 'running': slot, 'boot': slot, 'running_state': state, 'next_state': nxt, 'hold': hold}
    diag = {'uptime_s': up, 'boots': boots, 'wifi': {'up': wifi, 'rssi': -80}, 'rx': {'state': rx}}
    r = {'console': (status, con(w['cursor']) if con and status == 200 else None), 'ota': (200, ota), 'diag': (200, diag), 'state': (200, DONE)}
    return orcon.fold(w, r, 't')
START, KEY = {'slot': 'app1', 'target': 'app0'}, 'geen herstelsleutel geregistreerd: deze firmware wordt niet bevestigd en de vorige keert terug.'
w = orcon.new_watch()
rnd(w, 'app1', 'valid', 2000, 3, ring(['opgestart: app1', 'nieuwe firmware bevestigd (gezond na 61 s)'], 2000000))
rnd(w, 'app0', 'pending-verify', 20, 4, ring(['opgestart: app0, proefstart na OTA', 'a', 'b'], 20000))   # the old cursor (2) would skip events 1-2
check(w['epoch'] == 1 and w['cursor'] == 0 and len(w['events']) == 2, 'trial watch: a restart is seen from /api/diag; a page read with the old cursor is dropped, the cursor restarts')
rnd(w, 'app0', 'pending-verify', 70, 4, ring(['opgestart: app0, proefstart na OTA', 'a', 'b', KEY], 70000))
check([e['s'] for e in w['events'] if e['epoch'] == 1] == [1, 2, 3, 4] and not w['incomplete'], 'trial watch: the new boot is read from its first event, nothing skipped')
check(orcon.trial_outcome(START, w['rounds']) is None, 'trial watch: still open while the trial boot runs')
rnd(w, 'app1', 'valid', 5, 5, ring(['opgestart: app1, reden software'], 5000))
check(orcon.trial_outcome(START, w['rounds']) == 'rolled back' and orcon.trial_verdict(START, w)[0] == 'owner-key',
      'trial watch: rolled back, named by the trial boot\'s key note; the old boot\'s "bevestigd" note does not count')
w = orcon.new_watch()
rnd(w, 'app0', 'pending-verify', 290, 4, ring(['opgestart: app0'], 290000))
rnd(w, 'app1', 'valid', 1, 5, ring(['opgestart: app0', KEY], 299000))   # the page was read just before the restart, /api/diag just after
check([e['epoch'] for e in w['events']] == [0, 0] and w['cursor'] == 0, 'trial watch: the old boot\'s last page, read before a restart, stays with that boot')
w = orcon.new_watch()
for up in (20, 70, 140): rnd(w, 'app0', 'pending-verify', up, 4, ring(['opgestart: app0'] * 3, up * 1000, lost=5 if up == 70 else 0))
code, why = orcon.trial_verdict(START, w)
check(code == 'unknown' and 'incomplete' in why and 'ESP-IDF' in why, 'trial watch: healthy readings without a key note stay "unknown"; a ring overflow is said')
w = orcon.new_watch()
rnd(w, 'app0', 'pending-verify', 20, 4, status=403); rnd(w, 'app0', 'pending-verify', 100, 4, status=403)
check([g['status'] for g in w['gaps']] == [403, 403] and 'incomplete' in orcon.trial_verdict(START, w)[1], 'trial watch: a refused console page is a gap, and the verdict says so')
check(orcon.fold(orcon.new_watch(), {'console': (200, {'now': 1}), 'ota': (200, {}), 'diag': ('timeout', None), 'state': (200, {})}, 't') is None,
      'trial watch: without /api/diag a round has no boot, so its page is not used')
def verdict(*rs):
    w = orcon.new_watch()
    for r in rs: rnd(w, *r)
    return orcon.trial_verdict(START, w)[0], orcon.trial_outcome(START, w['rounds'])
check(verdict(('app0', 'pending-verify', 20, 4), ('app0', 'valid', 61, 4)) == ('confirmed', 'confirmed'), 'trial watch: valid in the target slot = confirmed')
check(verdict(*[('app0', 'pending-verify', t, 4, None, False, False) for t in (60, 150, 290)]) == ('wifi', None), 'trial watch: never on WiFi from 60 s')
check(verdict(('app0', 'pending-verify', 100, 4, None, False, True, 1), ('app0', 'pending-verify', 200, 4, None, False, False))[0] == 'wifi+rx', 'trial watch: both causes named')
check(verdict(('app0', 'pending-verify', 100, 4, None, True))[0] == 'hold', 'trial watch: a held trial')
check(verdict(('app0', 'pending-verify', 59, 4), ('app1', 'valid', 5, 5))[0] == 'no-data', 'trial watch: no reading from 60 s on is no evidence')
check(verdict(('app1', 'valid', 3000, 3), ('app1', 'valid', 5, 5), ('app1', 'valid', 9, 6)) == ('no-data', None),
      'trial watch: a trial never read stays open, even after restarts (the target may be invalid from before)')
w = orcon.new_watch()
rnd(w, 'app0', 'pending-verify', 20, 4, ring(['nieuwe firmware bevestigd (gezond na 61 s)'], 62000))
check(orcon.trial_verdict(START, w)[0] == 'confirmed', 'trial watch: the trial boot\'s own confirmation note counts when the valid reading was missed')

# The USB log: redacted lines, and RTS released before DTR, then closed, on every path
class Port:
    def __init__(self, broken): self.ops, self.n, self.broken = [], 0, broken
    def read(self, k):
        self.n += 1
        if self.n > 3: raise OSError('device gone') if self.broken else KeyboardInterrupt
        return b'[ota] uitlezen gestart (sessie 987654)\n'
    def __setattr__(self, k, v):
        if k in ('rts', 'dtr'): self.ops.append((k, v))
        object.__setattr__(self, k, v)
    def close(self): self.ops.append(('close',))
with tempfile.TemporaryDirectory() as d:
    for broken in (True, False):
        p = Port(broken)
        try: orcon.serial_tee(p, Path(d) / 'serial.log', threading.Event())
        except (OSError, KeyboardInterrupt): pass
        check(p.ops == [('rts', False), ('dtr', False), ('close',)], f'trial watch: serial closed RTS first, then DTR ({"a read error" if broken else "an interrupt"})')
    log = (Path(d) / 'serial.log').read_text()
    check('sessie <redacted>' in log and '987654' not in log and (Path(d) / 'serial.log').stat().st_mode & 0o077 == 0, 'trial watch: the USB log is redacted and private')

# The whole command against a fake board: GET routes only, an old note never decides, private files, no secret kept
def watch_args(**kw): return argparse.Namespace(**{'host': 'b', 'minutes': 6, 'every': 2, 'serial': None, 'anyway': False, 'key_proven': FP, **kw})
NEW = lambda st, up: {'fw': 'e' * 40, 'running': 'app0', 'boot': 'app0', 'running_state': st, 'next': 'app1', 'next_state': 'valid', 'hold': False}
OLDB = {'fw': 'b' * 40, 'running': 'app1', 'boot': 'app1', 'running_state': 'valid', 'next': 'app0', 'next_state': 'invalid', 'hold': False}
snaps = [(OLDB, 2000, 3, None), (OLDB, 2003, 3, ring(['opgestart: app1', 'nieuwe firmware bevestigd (gezond na 61 s)', 'uitlezen gestart (sessie 4242, 29:****56)'], 2003000)),
         (NEW('pending-verify', 20), 20, 4, ring(['opgestart: app0, proefstart na OTA', 'a', 'b'], 20000)),
         (NEW('pending-verify', 70), 70, 4, ring(['opgestart: app0, proefstart na OTA', 'a', 'b', KEY], 70000)),
         (OLDB, 5, 5, ring(['opgestart: app1, reden software'], 5000))]
def fake_board():
    calls, pos = [], [0]
    def get(path):
        calls.append(path)
        if path.startswith('/api/setup/console?after='): pos[0] = min(pos[0] + 1, len(snaps) - 1)
        o, up, boots, con = snaps[pos[0]]
        if path == '/api/ota': return 200, o
        if path == '/api/diag': return 200, {'uptime_s': up, 'boots': boots, **OK_DIAG, 'wifi': {'up': True, 'rssi': -80, 'ip': '192.0.2.7'}}
        if path == '/api/setup/state': return 200, {**DONE, 'token': 'f' * 32, 'wifi': {'ap': False, 'ssid': 'Thuisnet'}}
        return 200, con(int(path.split('=')[1]))
    return calls, get
real = (orcon.trial_get, orcon.REPO, orcon.TRIAL_SLEEP)
with tempfile.TemporaryDirectory() as d:
    try:
        orcon.REPO, orcon.TRIAL_SLEEP = Path(d), lambda s: None
        calls, get = fake_board(); orcon.trial_get = lambda h, p: get
        for bad in (dict(every=0), dict(minutes=0), dict(every=float('nan')), dict(host='http://b'), dict(host='a@b'), dict(host='b/x'), dict(host='b:0'), dict(host='b:70000')):
            try: orcon.cmd_trial_watch(watch_args(**bad)); check(False, f'trial watch: refused {bad}')
            except SystemExit: pass
        check(calls == [], 'trial watch: bad timing or host is refused before any request')
        try: orcon.cmd_trial_watch(watch_args(key_proven='0123456789abcdef0123456789abcdef'))
        except SystemExit as e: stopped = 'do not install' in str(e)
        pre = json.loads(next((Path(d) / 'captures' / 'trial').iterdir()).joinpath('preflight.json').read_text())
        check(stopped and calls == ['/api/ota', '/api/diag', '/api/setup/state'] and pre['key_proof'] == {'rejected': 'not a fingerprint, not stored'},
              'trial watch: without a valid attestation it stops after the preflight reads, and a pasted secret is never stored')
        calls, get = fake_board(); orcon.trial_get = lambda h, p: get
        orcon.cmd_trial_watch(watch_args())
    finally: orcon.trial_get, orcon.REPO, orcon.TRIAL_SLEEP = real
    run_dir = sorted((Path(d) / 'captures' / 'trial').iterdir())[-1]
    v = json.loads((run_dir / 'verdict.json').read_text())
    check(v['verdict'] == 'owner-key' and v['end'] == 'rolled back' and v['restarts_seen'] == 2, 'trial watch: a whole rollback, named by the trial boot\'s own key note')
    check(all(p in ('/api/ota', '/api/diag', '/api/setup/state') or re.fullmatch(r'/api/setup/console\?after=\d+', p) for p in calls),
          'trial watch: only the four read routes')
    logs = ''.join(p.read_text() for p in run_dir.iterdir())
    check(KEY in logs and not any(s in logs for s in ('f' * 32, 'token', '192.0.2.7', 'Thuisnet', '4242')), 'trial watch: the logs hold the notes, no token, address, network name or session number')
    check(all(p.stat().st_mode & 0o077 == 0 for p in run_dir.iterdir()) and run_dir.stat().st_mode & 0o077 == 0, 'trial watch: private files (0600 in 0700)')
print(f'orcon cli: {ok} checks, {fail} failures'); sys.exit(1 if fail else 0)
