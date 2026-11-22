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

# Trial-boot watch: preflight, transport, boots and console pages, the facts per boot, no secrets in a log
import http.client, http.server, io, threading
from contextlib import redirect_stdout
OK_OTA = {'fw': 'b' * 40, 'running': 'app1', 'boot': 'app1', 'next': 'app0', 'running_state': 'valid'}
OK_DIAG = {'fw': 'b' * 40, 'running': 'app1', 'wifi': {'up': True, 'rssi': -70}, 'rx': {'state': 0}}
DONE, FP = {'setup': {'state': 'done'}}, '1a2b-3c4d-5e6f'
check(orcon.preflight(OK_OTA, OK_DIAG, DONE, FP) == [], 'trial watch: preflight GO on a confirmed image with an owner key attested by hand')
pf = orcon.preflight(OK_OTA, OK_DIAG, DONE)
check(len(pf) == 1 and 'not provable' in pf[0] and '--key-proven' in pf[0], 'trial watch: "done" alone is NO-GO (no home-network route shows the key)')
check(all(len(orcon.preflight(OK_OTA, OK_DIAG, DONE, bad)) == 1 for bad in ('', 'yes', '1A2B-3C4D-5E6F', '1a2b-3c4d-5e6', '1a2b3c4d5e6f', FP + '\n')),
      'trial watch: anything but the board\'s fingerprint format is no attestation')
check(len(orcon.preflight(OK_OTA, OK_DIAG, {'setup': {'state': 'legacy'}}, FP)) == 1, 'trial watch: a legacy board (no owner key) stays NO-GO, whatever is attested')
check(len(orcon.preflight({**OK_OTA, 'boot': 'app0'}, OK_DIAG, DONE, FP)) == 1, 'trial watch: running != boot is NO-GO (migration step 7)')
check(all(len(orcon.preflight({**OK_OTA, 'next': n}, OK_DIAG, DONE, FP)) == 1 for n in (None, '?', 'app1')), 'trial watch: no real, distinct target slot is NO-GO')
check(len(orcon.preflight(OK_OTA, {**OK_DIAG, 'fw': 'e' * 40}, DONE, FP)) == 1, 'trial watch: /api/ota and /api/diag of different images are NO-GO')
check(len(orcon.preflight(OK_OTA, {**OK_DIAG, 'wifi': {'up': False}, 'rx': {'state': 1}}, DONE, FP)) == 2, 'trial watch: WiFi down and RX recovering are NO-GO')
check(len(orcon.preflight({}, {'wifi': [], 'rx': 'x'}, {'setup': []})) == 6, 'trial watch: an unreadable or ill-typed board is NO-GO, not a crash')
check(orcon.keep('diag', {'fw': 'c' * 40, 'wifi': {'up': True, 'ip': '192.0.2.7'}, 'rx': ['x'], 'coredump': {'pc': 1}, 'reset_reason': 'x' * 300, 'other': 1})
      == {'fw': 'c' * 40, 'wifi': {'up': True}, 'rx': None, 'coredump': True, 'reset_reason': None}, 'trial watch: a log keeps only allowlisted fields, typed and short')
check(orcon.keep('state', {'token': 'a' * 32, 'wifi': {'ap': False, 'ssid': 'Thuisnet'}, 'setup': {'state': 'done'}}) == {'wifi': {'ap': False}, 'setup': {'state': 'done'}},
      'trial watch: no token or network name from /api/setup/state')
check(orcon.redact('uitlezen gestart (sessie 123456) weer verbonden (adres 192.0.2.44) ' + 'b' * 32 + ' ' + 'c' * 40) ==
      'uitlezen gestart (sessie <redacted>) weer verbonden (adres <address>) <redacted> ' + 'c' * 40, 'trial watch: session numbers, addresses and 32/64-hex runs are redacted')

# The transport, against a real local socket: GET with Host only, no redirect, no proxy, the firmware's fixed-length 200 only
class Board(http.server.BaseHTTPRequestHandler):
    seen = []
    def do_GET(self):
        Board.seen.append((self.command, self.path, sorted(h.lower() for h in self.headers.keys())))
        body = b'{"ok":true}'
        code, hdr = {'/moved': (302, [('Location', 'http://192.0.2.1/x'), ('Content-Length', '0')]), '/short': (200, [('Content-Length', '64')]),
                     '/chunked': (200, [('Transfer-Encoding', 'chunked'), ('Content-Length', '11')]), '/huge': (200, [('Content-Length', '9' * 30)]),
                     '/err': (503, [('Content-Length', '11')])}.get(self.path, (200, [('Content-Length', '11')]))
        self.send_response(code)
        for k, v in hdr: self.send_header(k, v)
        self.end_headers(); self.wfile.write(body if code != 302 else b'')
    def log_message(self, *a): pass
srv = http.server.HTTPServer(('127.0.0.1', 0), Board); threading.Thread(target=srv.serve_forever, daemon=True).start()
saved = {k: os.environ.get(k) for k in ('http_proxy', 'HTTP_PROXY', 'no_proxy', 'NO_PROXY')}
os.environ.update(http_proxy='http://user:pw@192.0.2.9:3128', HTTP_PROXY='http://user:pw@192.0.2.9:3128', no_proxy='', NO_PROXY='')
try:
    g = orcon.trial_get('127.0.0.1', srv.server_port, orcon.time.monotonic() + 30)
    check(g('/api/ota') == (200, {'ok': True}) and Board.seen[-1] == ('GET', '/api/ota', ['host']), 'trial watch: a direct GET with the Host header only, no proxy')
    check(g('/moved') == (302, None) and len(Board.seen) == 2, 'trial watch: a redirect is not followed')
    check(g('/short') == ('short-reply', None) and g('/chunked') == ('bad-framing', None) and g('/huge') == ('bad-framing', None),
          'trial watch: a short, chunked or oversized reply is no reply, even when it parses')
    check(g('/err') == (503, None), 'trial watch: a non-200 status is kept and its body never used')
    late = orcon.trial_get('127.0.0.1', srv.server_port, orcon.time.monotonic() - 1)
    n = len(Board.seen); check(orcon.one(late, '/api/ota') == ('TimeoutError', None) and len(Board.seen) == n, 'trial watch: no request once the watch time is over')
finally:
    srv.shutdown()
    for k, v in saved.items():
        if v is None: os.environ.pop(k, None)
        else: os.environ[k] = v

# A firmware-shaped board, request by request: its console ring (a cursor at or past "next" restarts from 1), and a
# switch to the next state at the given request numbers (a restart between any two GETs).
def dg(slot, state, up, boots, fw, wifi=True, rx=0):
    return {'fw': fw * 40, 'running': slot, 'running_state': state, 'uptime_s': up, 'boots': boots, 'wifi': {'up': wifi, 'rssi': -80, 'ip': '192.0.2.7'}, 'rx': {'state': rx}}
def st(diag, ring=(), per=16, nxt='app0', now=None, **ota):
    o = {'fw': diag['fw'], 'running': diag['running'], 'boot': diag['running'], 'next': nxt if diag['running'] != nxt else 'app1',
         'running_state': diag['running_state'], 'next_state': 'invalid', 'hold': False, **ota}
    return {'diag': diag, 'ota': o, 'ring': list(ring), 'per': per, 'now': now}
class FakeBoard:
    def __init__(self, states, switch_at=()): self.states, self.i, self.n, self.at, self.calls = states, 0, 0, set(switch_at), []
    def get(self, path):
        self.n += 1
        if self.n in self.at: self.i = min(self.i + 1, len(self.states) - 1)
        self.calls.append(path); s = self.states[self.i]
        if path == '/api/diag': return 200, s['diag']
        if path == '/api/ota': return 200, s['ota']
        if path == '/api/setup/state': return 200, {**DONE, 'token': 'f' * 32, 'wifi': {'ap': False, 'ssid': 'Thuisnet'}}
        ev, after = s['ring'], int(path.split('=')[1]); n = len(ev) + 1; after = 0 if after >= n else after; take = ev[after:after + s['per']]
        return 200, {'now': s['now'] if s['now'] is not None else s['diag']['uptime_s'] * 1000 + 10, 'next': after + len(take) + 1, 'oldest': 1, 'lost': 0,
                     'more': after + len(take) < len(ev), 'ev': [{'s': after + i + 1, 't': 0, 'k': 'INFO', 'm': m} for i, m in enumerate(take)]}
def rounds(board, k, w=None):
    w = w or orcon.new_watch()
    for _ in range(k): orcon.fold(w, orcon.read_round(board.get, w), 't')
    return w
KEY = 'geen herstelsleutel geregistreerd: deze firmware wordt niet bevestigd en de vorige keert terug.'
OLD, T20, T70, BACK = (dg('app1', 'valid', 2000, 3, 'b'), dg('app0', 'pending-verify', 20, 4, 'e'), dg('app0', 'pending-verify', 70, 4, 'e'), dg('app1', 'valid', 5, 5, 'b'))
START = {'slot': 'app1', 'target': 'app0'}
b = FakeBoard([st(OLD, ['opgestart: app1', 'nieuwe firmware bevestigd (gezond na 61 s)']), st(T20, ['opgestart: app0, proefstart na OTA', 'a', 'b']),
               st(T70, ['opgestart: app0, proefstart na OTA', 'a', 'b'] + ['x'] * 20 + [KEY]), st(BACK, ['opgestart: app1, reden software'])], switch_at=(5, 9, 14))
w = rounds(b, 4)
f = orcon.summary(w)
check(w['epoch'] == 2 and [x['boot'] for x in f] == [0, 1, 2] and orcon.trial_end(START, w['rounds']) == 'back on the start slot',
      'trial watch: three boots read through the actual round path; the end is what the board showed (back on the start slot)')
check(f[0]['notes']['confirmed'] and not f[1]['notes']['confirmed'] and f[1]['notes']['owner_key_failed'] and f[1]['console_complete'],
      'trial watch: notes stay with their boot (the old boot\'s "bevestigd", the trial boot\'s key note), over several pages, complete')
check(f[1]['window_60_300_s'] == {'readings': 1, 'unknown_input': 0, 'held': 0, 'wifi_down': 0, 'rx_not_receiving': 0, 'healthy': 1}, 'trial watch: the 60-300 s inputs counted per reading')
b = FakeBoard([st(T70, ['opgestart: app0']), st(BACK, ['opgestart: app1, reden software'])], switch_at=(6,))   # a restart between the console and /api/ota
w = rounds(b, 2)
check(w['epoch'] == 1 and [r['epoch'] for r in w['rounds']] == [0, 0, 1] and 0 in w['incomplete'] and any(g['status'] == 'restart-inside' for g in w['gaps']),
      'trial watch: a restart inside a round keeps its first reading for the old boot and uses none of its pages')
b = FakeBoard([st(dg('app1', 'valid', 62, 1, 'b'), ['nieuwe firmware bevestigd (gezond na 61 s)']), st(dg('app0', 'pending-verify', 70, 1, 'e'), [KEY])], switch_at=(5,))
w = rounds(b, 2)
check(w['epoch'] == 1 and orcon.summary(w)[1]['notes'] == {'owner_key_failed': True, 'confirmed': False, 'not_confirmed': False},
      'trial watch: a power cut that resets the boot counter is still a new boot when the slot or image changes')
w = rounds(FakeBoard([st(T70, ['x'] * 80, per=5)]), 1)
check(not orcon.summary(w)[0]['console_complete'] and w['cursor'] == 20, 'trial watch: short pages with more waiting leave that boot\'s console marked unread')
w = rounds(FakeBoard([st(T70, ['a'], now=5000)]), 1)
check(not w['events'] and 0 in w['incomplete'] and w['gaps'][-1]['status'] == 'unowned-page', 'trial watch: a page whose own clock is outside the round is not used')
w = rounds(FakeBoard([st(OLD), st(dg('app0', 'valid', 4, 4, 'e'))], switch_at=(5,)), 3)
check(orcon.trial_end(START, w['rounds']) is None, 'trial watch: the target seen valid without a trial before it is no end (no claim of a confirmation)')
w = rounds(FakeBoard([st(dg('app0', 'pending-verify', 61, 4, 'e')), st(dg('app0', 'valid', 62, 4, 'e'))], switch_at=(5,)), 2)
check(orcon.trial_end(START, w['rounds']) == 'target valid after its trial', 'trial watch: pending-verify, then valid, in the same boot: the end the board showed')
w = rounds(FakeBoard([st(dg('app0', 'pending-verify', 100, 4, 'e', wifi=False), hold=True)]), 1)
check(orcon.summary(w)[0]['window_60_300_s'] == {'readings': 1, 'unknown_input': 0, 'held': 1, 'wifi_down': 1, 'rx_not_receiving': 0, 'healthy': 0},
      'trial watch: a held and offline reading is counted as such, nothing is concluded')
w = rounds(FakeBoard([st(dg('app0', 'pending-verify', 100, 4, 'e', rx=None))]), 1)
check(orcon.summary(w)[0]['window_60_300_s']['unknown_input'] == 1, 'trial watch: a missing input is counted as unknown, never as a cause')
check(orcon.take_page(orcon.new_watch(), {'next': 2, 'ev': [{'s': 1, 'm': '#12 -80 dBm I 22F1 29:****56>--:------ pl=000304'}]}, 0, 't') is None, 'trial watch: take_page returns nothing')
w = orcon.new_watch(); orcon.take_page(w, {'next': 2, 'ev': [{'s': 1, 'm': '#12 -80 dBm I 1FC9 29:****56>32:****21 pl=0022F1123456'}]}, 0, 't')
check(w['events'][0]['m'] == '<rf frame>', 'trial watch: an RF frame in the console (ids, payload) is kept as a marker only')

# The whole command against the fake board: read routes only, GO only on fresh facts, private files, no secret kept
def watch_args(**kw): return argparse.Namespace(**{'host': 'b', 'minutes': 6, 'every': 2, 'anyway': False, 'key_proven': FP, **kw})
OLD2 = dg('app1', 'valid', 2003, 3, 'b')
FULL = [st(OLD), st(OLD2, ['opgestart: app1', 'nieuwe firmware bevestigd (gezond na 61 s)', 'uitlezen gestart (sessie 4242)', '#9 -70 dBm I 1FC9 pl=0022F1123456']),
        st(T20, ['opgestart: app0, proefstart na OTA', 'a', 'b']), st(T70, ['opgestart: app0, proefstart na OTA', 'a', 'b', KEY + ' (adres 192.0.2.7)']),
        st(BACK, ['opgestart: app1, reden software'])]
real = (orcon.trial_get, orcon.REPO, orcon.TRIAL_SLEEP)
with tempfile.TemporaryDirectory() as d:
    def run(board, **kw):
        orcon.trial_get = lambda h, p, dl: board.get
        out = io.StringIO()
        with redirect_stdout(out): orcon.cmd_trial_watch(watch_args(**kw))
        return out.getvalue(), sorted((Path(d) / 'captures' / 'trial').iterdir())[-1]
    try:
        orcon.REPO, orcon.TRIAL_SLEEP = Path(d), lambda s: None
        b = FakeBoard(FULL)
        for bad in (dict(every=0), dict(minutes=0), dict(every=float('nan')), dict(host='http://b'), dict(host='a@b'), dict(host='b/x'), dict(host='b:0'), dict(host='b:70000')):
            try: run(b, **bad); check(False, f'trial watch: refused {bad}')
            except SystemExit: pass
        check(b.calls == [], 'trial watch: bad timing or host is refused before any request')
        try: run(FakeBoard(FULL), key_proven='0123456789abcdef0123456789abcdef'); stopped = False
        except SystemExit as e: stopped = 'do not install' in str(e)
        pre = json.loads(sorted((Path(d) / 'captures' / 'trial').iterdir())[-1].joinpath('preflight.json').read_text())
        check(stopped and pre['key_proof'] == {'rejected': 'not a fingerprint, not stored'}, 'trial watch: without a valid attestation no watch, and a pasted secret is never stored')
        try: run(FakeBoard([st(OLD), st(T20)], switch_at=(3,))); mixed = False
        except SystemExit as e: mixed = 'one boot' in str(e)
        check(mixed, 'trial watch: a preflight whose replies span a restart is refused')
        txt, _ = run(FakeBoard([st(OLD), st(T20)], switch_at=(6,)))
        check('changed since the preflight' in txt and 'preflight GO' not in txt, 'trial watch: no GO when the board restarted after the preflight')
        b = FakeBoard(FULL, switch_at=(6, 10, 14, 18)); txt, run_dir = run(b)
        check('preflight GO' in txt and 'END: back on the start slot' in txt, 'trial watch: GO on fresh facts, then recorded to the end the board showed')
    finally: orcon.trial_get, orcon.REPO, orcon.TRIAL_SLEEP = real
    s = json.loads((run_dir / 'summary.json').read_text())
    check(s['end'] == 'back on the start slot' and [x['notes']['owner_key_failed'] for x in s['boots']] == [False, True, False] and s['boots'][0]['notes']['confirmed'],
          'trial watch: the summary gives the facts per boot (the key note in the trial boot only)')
    check(all(p in ('/api/ota', '/api/diag', '/api/setup/state') or re.fullmatch(r'/api/setup/console\?after=\d+', p) for p in b.calls), 'trial watch: only the read routes')
    logs = ''.join(p.read_text() for p in run_dir.iterdir())
    check(KEY in logs and '<rf frame>' in logs and not any(x in logs for x in ('f' * 32, 'token', '192.0.2.7', 'Thuisnet', '4242', '123456')),
          'trial watch: the logs hold the notes, no token, address, network name, session number or RF payload')
    runs = sorted((Path(d) / 'captures' / 'trial').iterdir())
    check(all(p.stat().st_mode & 0o077 == 0 for r in runs for p in r.iterdir()) and all(r.stat().st_mode & 0o077 == 0 for r in runs), 'trial watch: private files (0600 in 0700)')
print(f'orcon cli: {ok} checks, {fail} failures'); sys.exit(1 if fail else 0)
