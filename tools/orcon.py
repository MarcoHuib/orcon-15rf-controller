#!/usr/bin/env python3
"""orcon.py — talk to the Heltec V2 over WiFi: status, RF log collection, signed OTA.

  orcon.py status                       firmware, OTA slots, reset reason, crash summary
  orcon.py logs [--minutes N]           collect console/capture/diag into captures/sessions/<UTC>/
  orcon.py capture                      save the complete 1FC9/10E0 capture buffer
  orcon.py ota-install --worktree DIR --env ENV --yes [--tx-ack PHRASE]
                                        verify the local package with tools/heltec_v2.sh, sign a
                                        manifest for THIS board with ~/.orcon/ota-signing-key.pem,
                                        upload, then confirm the new image (or report the rollback).
                                        PHRASE for images that transmit: "RF test image" (rftest),
                                        "control firmware" (the daily-use control image)
       ota-install ... --test-rollback  rollback test: the trial boot is told not to confirm itself
                                        (POST /api/ota/hold, logged in); PASS only if the previous image returns.
                                        The password is checked on the running firmware before anything is
                                        written; a hold that fails is retried until the trial boot ends
  orcon.py backup-check --nvs DUMP      compare the board (RF IDs, OLED mode, WiFi name, profiles) with an
                                        NVS dump (tools/nvs_keys.py); prints only equal/different per field
  orcon.py release --worktree DIR --env ENV --version N --yes [--tx-ack PHRASE] [--out DIR]
                                        a signed release bundle (.orconfw) for every board: the image
                                        plus a v2 manifest signed with the release key. Owners install
                                        it in the web page (Systeem > Firmware); the board checks the
                                        signature, the version and its own owner's session
  orcon.py keygen                       create the signing key (once) and src/ota_pubkey.h
  orcon.py trial-watch [--minutes N] [--every S] [--serial PORT] [--anyway]
                                        read-only record of one trial boot, started BEFORE installing: a
                                        preflight (running == boot, valid, owner state done, WiFi, RX), then
                                        /api/ota, /api/diag, /api/setup/state and the RF console every S s
                                        (optionally the USB log, opened without a reset) until the new image
                                        confirms itself or rolls back, then the condition that held it back.
                                        Never logs in and never writes to the board; logs keep no secrets

Owner-model firmware (it reports "owner_model" in /api/ota) only installs with the owner's session:
ota-install then asks for the web password. Older firmware installs as before.

Host: --host, $ORCON_HOST, ~/.orcon/config.json {"host": ...}, else orcon15rf.local.
Captures hold real device IDs: they go to captures/ (git-ignored), directories 0700, files 0600.
Nothing here uses GitHub or the network beyond the board itself.
"""
import argparse, getpass, hashlib, json, os, re, subprocess, sys, tempfile, threading, time, urllib.error, urllib.request, uuid, zlib
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KEY = Path.home() / '.orcon' / 'ota-signing-key.pem'
# Every image written over WiFi ("<sha256> <label>"): tools/heltec_v2.sh recover identifies slots with it.
INSTALLED = Path.home() / '.orcon' / 'installed.txt'
RF_ACK = 'RF test image'                      # same phrases as tools/heltec_v2.sh
CONTROL_ACK = 'control firmware'
ENV_VARIANT = {'heltec_v2_sx1276': ('rx-only', False), 'heltec_v2_sx1276_rftest': ('rf-test', True),
               'heltec_v2_sx1276_control': ('control', True)}
ENV_ACK = {'heltec_v2_sx1276_rftest': ('ORCON_RF_TEST_ACK', RF_ACK), 'heltec_v2_sx1276_control': ('ORCON_CONTROL_ACK', CONTROL_ACK)}


def host(args):
    if getattr(args, 'host', None): return args.host
    if os.environ.get('ORCON_HOST'): return os.environ['ORCON_HOST']
    cfg = Path.home() / '.orcon' / 'config.json'
    if cfg.exists(): return json.loads(cfg.read_text()).get('host', 'orcon15rf.local')
    return 'orcon15rf.local'


def http(h, path, body=None, timeout=10, ctype='application/json', headers=None):
    data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
    req = urllib.request.Request(f'http://{h}{path}', data=data, method='GET' if body is None else 'POST')
    if body is not None: req.add_header('Content-Type', ctype)
    for k, v in (headers or {}).items(): req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r: return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        try: return e.code, json.loads(e.read() or b'{}')
        except ValueError: return e.code, {}


def cookie_sid(set_cookies):
    """The orcon_sid value (32 hex) from the Set-Cookie headers of a login reply, else ''."""
    for c in set_cookies:
        v = c.split(';', 1)[0].strip()
        if v.startswith('orcon_sid=') and len(v) == 42 and all(ch in '0123456789abcdef' for ch in v[10:]): return v[10:]
    return ''


def login(h, password, timeout=15):
    """Web login (PBKDF2 on the board takes ~1.3 s). Returns the headers that authorise a mutating
    request: session cookie, its CSRF token and the same-origin header the board requires."""
    origin = f'http://{h}'
    req = urllib.request.Request(f'{origin}/api/auth/login', data=json.dumps({'password': password}).encode(), method='POST',
                                 headers={'Content-Type': 'application/json', 'Origin': origin})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            j, sid = json.loads(r.read() or b'{}'), cookie_sid(r.headers.get_all('Set-Cookie') or [])
    except urllib.error.HTTPError as e:
        try: err = json.loads(e.read() or b'{}').get('error', '')
        except ValueError: err = ''
        raise SystemExit(f'login refused (HTTP {e.code} {err})')
    if not sid or not j.get('token'): raise SystemExit('login reply without a session')
    return {'Cookie': f'orcon_sid={sid}', 'X-Orcon-Token': j['token'], 'Origin': origin}


def rollback_hold(h, password, login_fn=login, post=http):
    """Rollback test: the trial boot is a fresh boot (sessions live in RAM), so log in there,
    then ask it not to confirm itself. The board only accepts this from a logged-in owner.
    True only once the board accepted the hold; a failure never ends the watch (it is retried)."""
    def hold():
        try: st, j = post(h, '/api/ota/hold', {}, headers=login_fn(h, password))
        except (SystemExit, Exception) as e:          # refused login, network error, malformed reply
            print('rollback test: hold not sent yet:', e); return False
        print('rollback test: hold ->', (st, j)); return st == 200
    return hold


def private_dir(p):
    p.mkdir(parents=True, exist_ok=True); os.chmod(p, 0o700); return p


def write_private(p, text):
    p.write_text(text); os.chmod(p, 0o600)


# ---- manifest and signature (pure; tested by tools/test_orcon_cli.py) ----------------------
def build_manifest(mac, env, commit, firmware, nonce):
    if env not in ENV_VARIANT: raise ValueError(f'env {env} is not an approved OTA variant')
    variant, allow_tx = ENV_VARIANT[env]
    data = Path(firmware).read_bytes()
    m = {'v': 1, 'mac': mac.lower(), 'env': env, 'variant': variant, 'allow_tx': allow_tx, 'commit': commit,
         'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'nonce': nonce}
    return json.dumps(m, separators=(',', ':'))


def sign(text, key=KEY):
    if not Path(key).exists(): raise SystemExit(f'signing key {key} missing (orcon.py keygen)')
    if Path(key).stat().st_mode & 0o077: raise SystemExit(f'{key} must be readable by you only (chmod 600)')
    r = subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(key)], input=text.encode(), capture_output=True, check=True)
    return r.stdout.hex()


def release_manifest(env, commit, firmware, version):
    """v2: one signature for every board. The board checks it against the release key, refuses an
    older release than its own and installs only with its owner's logged-in session."""
    if env not in ('heltec_v2_sx1276', 'heltec_v2_sx1276_control'): raise ValueError(f'{env} is not a released variant')
    if not isinstance(version, int) or not 0 <= version < 2 ** 32: raise ValueError('version must be a non-negative integer')
    variant, allow_tx = ENV_VARIANT[env]
    data = Path(firmware).read_bytes()
    if f'ORCON-RELEASE {version};'.encode() not in data:
        raise ValueError(f'the image is not built as release {version} (-DORCON_RELEASE_VERSION={version})')
    m = {'v': 2, 'kind': 'orcon-release', 'env': env, 'variant': variant, 'allow_tx': allow_tx, 'commit': commit,
         'version': version, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    return json.dumps(m, separators=(',', ':'))


def write_bundle(path, manifest, sig, firmware):
    """.orconfw: one ASCII JSON line {"manifest","sig"}, a newline, then the image (the web page splits it)."""
    head = json.dumps({'manifest': manifest, 'sig': sig}, separators=(',', ':'))
    assert head.isascii() and '\n' not in head
    Path(path).write_bytes(head.encode() + b'\n' + Path(firmware).read_bytes())


def rf_gate(env, ack):
    """An image that can transmit needs its own explicit phrase, the same as the USB flash path."""
    if ENV_VARIANT.get(env, (None, False))[1]:
        need = ENV_ACK[env][1]
        if ack != need: raise SystemExit(f'{env} can transmit: pass --tx-ack "{need}" to install it')


# ---- log collection with gap detection ---------------------------------------------------
class Collector:
    """Console events carry a per-boot sequence ('s'); a lower 'next' means the board restarted."""
    def __init__(self): self.cursor, self.expect, self.gaps = 0, None, []

    def page(self, c, now):
        out = []
        if self.expect is not None and c.get('next', 0) and c['next'] - 1 < self.cursor:
            self.gaps.append({'t': now, 'kind': 'restart', 'detail': f"sequence restarted at {c['next']}"})
            self.cursor, self.expect = 0, None
            return out
        if c.get('lost'):
            self.gaps.append({'t': now, 'kind': 'lost', 'detail': f"board buffer overflowed: {c['lost']} events"})
        for e in c.get('ev', []):
            if self.expect is not None and e.get('s', self.expect) > self.expect:
                self.gaps.append({'t': now, 'kind': 'gap', 'detail': f"events {self.expect}..{e['s'] - 1} missing"})
            self.expect = e.get('s', 0) + 1
            out.append(e)
        if c.get('next'): self.cursor = c['next'] - 1
        return out


def cmd_logs(args):
    h = host(args)
    d = private_dir(REPO / 'captures' / 'sessions' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    con, gaps, diag = ((d / n).open('a') for n in ('console.jsonl', 'gaps.jsonl', 'diag.jsonl'))
    for f in (con, gaps, diag): os.chmod(f.name, 0o600)
    col, end, last_cap, last_diag, down = Collector(), time.time() + args.minutes * 60, -1, 0, False
    print(f'collecting from {h} into {d} for {args.minutes} min (Ctrl-C stops)')
    try:
        while time.time() < end:
            now = datetime.now(timezone.utc).isoformat()
            try:
                st, c = http(h, f'/api/setup/console?after={col.cursor}', timeout=5)
                if down: gaps.write(json.dumps({'t': now, 'kind': 'link', 'detail': 'board reachable again'}) + '\n'); down = False
                for e in col.page(c, now): con.write(json.dumps({'host_t': now, **e}) + '\n')
                for g in col.gaps: gaps.write(json.dumps(g) + '\n'); print('GAP', g['kind'], g['detail'])
                col.gaps.clear()
                if time.time() - last_diag > 30:
                    last_diag = time.time()
                    _, cap = http(h, '/api/setup/capture', timeout=5)
                    if cap.get('total', -1) != last_cap:
                        last_cap = cap.get('total', -1); write_private(d / f'capture-{last_cap}.json', json.dumps(cap, indent=1))
                    _, dg = http(h, '/api/diag', timeout=5)
                    diag.write(json.dumps({'host_t': now, **dg}) + '\n')
                con.flush(); gaps.flush(); diag.flush()
                time.sleep(0.15 if c.get('more') else 1.0)
            except (urllib.error.URLError, OSError, ValueError) as e:
                if not down: gaps.write(json.dumps({'t': now, 'kind': 'link', 'detail': f'board unreachable: {e}'}) + '\n'); print('board unreachable; retrying')
                down = True; time.sleep(3)
    except KeyboardInterrupt:
        pass
    print(f'saved: {d}')


def cmd_capture(args):
    _, cap = http(host(args), '/api/setup/capture')
    d = private_dir(REPO / 'captures')
    p = d / f"capture-{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}.json"
    write_private(p, json.dumps(cap, indent=1)); print(f"{cap.get('total', 0)} frames total; saved {p}")


# ---- trial-boot watch (read only; the pure parts are tested by tools/test_orcon_cli.py) -------------------------
# The control image confirms its own trial boot (ota_signed.cpp tick): from 60 s after boot, at most once a second,
# while not held (/api/ota hold), on the home network (/api/diag wifi.up) and receiving (rx.state 0), and only with a
# usable owner key (a console note when it is not). At 300 s an unconfirmed image rolls back. The one input the board
# does not report is the result of esp_ota_mark_app_valid_cancel_rollback itself.
CONFIRM_FROM_S = 60
SECRET_KEYS = {'token', 'csrf', 'sid', 'nonce', 'password', 'pass', 'psk', 'secret'}
HEX_SECRET = re.compile(r'(?<![0-9A-Fa-f])(?:[0-9A-Fa-f]{64}|[0-9A-Fa-f]{32})(?![0-9A-Fa-f])')


def redact(v):
    """What a log may keep: no field named like a secret, no 32/64-hex run (session id, CSRF token, Siri key).
    40-hex commits stay. The watch never logs in, so the board sends none of these to it anyway."""
    if isinstance(v, dict): return {k: redact(x) for k, x in v.items() if k.lower() not in SECRET_KEYS}
    if isinstance(v, list): return [redact(x) for x in v]
    return HEX_SECRET.sub('<redacted>', v) if isinstance(v, str) else v


def preflight(ota, diag, state):
    """What must hold before an install, so the new image can confirm itself (docs/HERSTELSLEUTEL_MIGRATIE.md step 7)."""
    p, rx = [], (diag.get('rx') or {}).get('state')
    if not ota.get('running') or ota.get('running') != ota.get('boot'):
        p.append(f"running {ota.get('running')} != boot {ota.get('boot')}: the old updater would write the selected slot")
    if ota.get('running_state') != 'valid': p.append(f"running_state {ota.get('running_state')}: not a confirmed image")
    owner = (state.get('setup') or {}).get('state')
    if owner != 'done':
        p.append(f"owner state {owner}: register the recovery key in the PRG recovery mode first (legacy = no key)")
    if not (diag.get('wifi') or {}).get('up'): p.append('WiFi not joined')
    if rx != 0: p.append(f'RX state {rx}: not receiving')
    return p


def trial_view(ota, diag, state):
    """One poll, reduced to the firmware's own confirmation inputs."""
    w = diag.get('wifi') or {}
    return {'fw': (ota.get('fw') or '')[:7], 'slot': ota.get('running'), 'state': ota.get('running_state'),
            'up_s': diag.get('uptime_s'), 'hold': ota.get('hold'), 'wifi': w.get('up'), 'rssi': w.get('rssi'),
            'rx': (diag.get('rx') or {}).get('state'), 'owner': (state.get('setup') or {}).get('state'),
            'ap': (state.get('wifi') or {}).get('ap')}


def settled(views):
    """After a trial boot: 'confirmed' (valid in the trial slot) or 'rolled back' (valid in another slot); else None."""
    t = next((v for v in views if v['state'] == 'pending-verify'), None)
    if not t or views[-1]['state'] != 'valid': return None
    return 'confirmed' if views[-1]['slot'] == t['slot'] else 'rolled back'


def trial_verdict(views, notes, gaps=False):
    """The condition that held a trial boot back. views: trial_view() of every poll; notes: console texts;
    gaps: the console capture lost events, so a missing key note proves nothing."""
    if settled(views) == 'confirmed' or any('nieuwe firmware bevestigd' in n for n in notes):
        return 'confirmed', 'the new image confirmed itself'
    if any('geen herstelsleutel' in n for n in notes):
        return 'owner-key', 'no usable owner key (state, key or crypto self-test): it never confirms on this board'
    # +3 s: a poll right after 60 s may precede the first attempt (once a second, in the main loop)
    due = [v for v in views if v['state'] == 'pending-verify' and (v['up_s'] or 0) >= CONFIRM_FROM_S + 3]
    if not due: return 'no-data', f'no poll of the trial boot after {CONFIRM_FROM_S} s'
    why = ['hold' if v['hold'] else 'wifi' if not v['wifi'] else 'rx' if v['rx'] != 0 else 'healthy' for v in due]
    if 'healthy' in why:
        if gaps: return 'unknown', 'healthy in a poll, but the console capture has gaps: a key note may be missing'
        return 'confirm-call', 'healthy, no key note, still unconfirmed: esp_ota_mark_app_valid_cancel_rollback failed'
    n = {k: why.count(k) for k in ('hold', 'wifi', 'rx') if k in why}
    return '+'.join(n), 'never healthy after 60 s: ' + ', '.join(f'{k} in {c} polls' for k, c in n.items())


def serial_tee(port, path, stop):
    """The USB log, line by line with UTC time, until the watch ends."""
    with path.open('a') as f:
        os.chmod(path, 0o600)
        buf = b''
        while not stop.is_set():
            buf += port.read(512)
            while b'\n' in buf:
                raw, buf = buf.split(b'\n', 1)
                line = redact(raw.decode('utf-8', 'replace').rstrip('\r'))
                f.write(f'{datetime.now(timezone.utc).isoformat()} {line}\n'); f.flush()
                if line.startswith(('[ota]', '[owner]', '[radio]', '[wifi]', '[FATAL]')): print('  usb:', line)
    port.rts = False; port.dtr = False   # RTS first: EN is pulled low only while RTS is held and DTR is not (rxmon.py)
    port.close()


def cmd_trial_watch(args):
    h = host(args)
    def get(path):
        st, j = http(h, path, timeout=4)
        return redact(j) if st == 200 else {}
    try: ota, diag, state = get('/api/ota'), get('/api/diag'), get('/api/setup/state')
    except (urllib.error.URLError, OSError, ValueError) as e: raise SystemExit(f'board {h} not readable (try again): {e}')
    d = private_dir(REPO / 'captures' / 'trial' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    problems = preflight(ota, diag, state)
    write_private(d / 'preflight.json', json.dumps({'ota': ota, 'diag': diag, 'state': state, 'problems': problems}, indent=1))
    for p in problems: print('PREFLIGHT NO-GO:', p)
    if problems and not args.anyway: raise SystemExit(f'do not install now (saved {d}); --anyway watches all the same')
    if not problems: print('preflight GO: install now in the web page (Systeem > Firmware); this records the trial boot')
    stop, tee = threading.Event(), None
    if args.serial:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from rxmon import open_serial
        try: port = open_serial(args.serial)   # now, before the install: DTR and RTS held, so opening never resets the board
        except ImportError: raise SystemExit('the USB log needs pyserial: run with .flashtools/venv/bin/python')
        tee = threading.Thread(target=serial_tee, args=(port, d / 'serial.log', stop), daemon=True)
        tee.start()
    col, views, notes, gaps, end = Collector(), [], [], False, time.time() + args.minutes * 60
    out = {n: (d / f'{n}.jsonl').open('a') for n in ('polls', 'console', 'gaps')}
    for f in out.values(): os.chmod(f.name, 0o600)
    try:
        while time.time() < end and not settled(views):
            now = datetime.now(timezone.utc).isoformat()
            try:
                ota, diag, state = get('/api/ota'), get('/api/diag'), get('/api/setup/state')
                _, c = http(h, f'/api/setup/console?after={col.cursor}', timeout=4)
                for e in col.page(redact(c), now):
                    out['console'].write(json.dumps({'host_t': now, **e}) + '\n'); notes.append(e.get('m', ''))
                    print('  console:', e.get('k'), e.get('m'))
                for g in col.gaps: out['gaps'].write(json.dumps(g) + '\n'); gaps = gaps or g['kind'] != 'restart'
                col.gaps.clear()
                v = trial_view(ota, diag, state); views.append(v)
                out['polls'].write(json.dumps({'host_t': now, 'view': v, 'ota': ota, 'diag': diag, 'state': state}) + '\n')
                print(f"{now[11:19]} {v['slot']} {v['fw']} {v['state']} up {v['up_s']} s | hold {v['hold']} | wifi {v['wifi']} "
                      f"({v['rssi']} dBm) | rx {v['rx']} | owner {v['owner']} | ap {v['ap']}")
            except (urllib.error.URLError, OSError, ValueError) as e:
                out['gaps'].write(json.dumps({'t': now, 'kind': 'link', 'detail': str(e)}) + '\n'); print(now[11:19], 'unreachable:', e)
            for f in out.values(): f.flush()
            time.sleep(args.every)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        if tee: tee.join(5)
        for f in out.values(): f.close()
    code, why = trial_verdict(views, notes, gaps)
    write_private(d / 'verdict.json', json.dumps({'verdict': code, 'why': why, 'end': settled(views)}, indent=1))
    print(f'VERDICT {code}: {why}\nsaved: {d}')


def cmd_status(args):
    h = host(args)
    _, o = http(h, '/api/ota'); _, d = http(h, '/api/diag')
    print(json.dumps({'ota': o, 'diag': d}, indent=1))


def cmd_ota_install(args):
    h, wt, env = host(args), Path(args.worktree).resolve(), args.env
    rf_gate(env, args.tx_ack)
    if not args.yes: raise SystemExit('refusing without --yes (explicit approval for this update)')
    commit = subprocess.run(['git', '-C', str(wt), 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    pkg = wt / '.flashtools' / 'local' / f'{commit}-{env}'
    # Same package verification as the USB path: evidence, hashes, markers, ELF reachability.
    venv = dict(os.environ, LOCAL_ENV=env, **({ENV_ACK[env][0]: ENV_ACK[env][1]} if env in ENV_ACK else {}))
    v = subprocess.run(['bash', 'tools/heltec_v2.sh', 'verify', commit], cwd=wt, env=venv, capture_output=True, text=True)
    if v.returncode or 'VERIFIED' not in v.stdout: raise SystemExit('package verification failed:\n' + v.stdout[-800:] + v.stderr[-800:])
    mac = (wt / '.flashtools' / 'board.mac').read_text().strip()
    st, before = http(h, '/api/ota')
    if st != 200 or before.get('signed_ota') != 'ORCON-SIGNED-OTA v1': raise SystemExit(f'board has no signed OTA (HTTP {st}); USB bootstrap first')
    if before.get('running_state') == 'pending-verify': raise SystemExit('board is still confirming its running image; wait')
    # Owner-model firmware installs only with its owner's session; the rollback hold is sent minutes
    # later inside the trial boot (a fresh boot, so it logs in again). Asked before anything is written.
    owner = bool(before.get('owner_model'))
    password = getpass.getpass('Webwachtwoord van de controller: ') if owner or args.test_rollback else ''
    hdr = login(h, password) if owner else {}
    # A hold that cannot log in lets the test image confirm itself: a wrong password must fail HERE, before
    # anything is written. The running firmware checks the same stored password the new image will.
    if args.test_rollback and not owner: login(h, password)
    st, n = http(h, '/api/ota/nonce', {}, headers=hdr)
    if st != 200: raise SystemExit(f'nonce refused: {n}')
    text = build_manifest(mac, env, commit, pkg / 'firmware.bin', n['nonce'])
    st, b = http(h, '/api/ota/begin', {'manifest': text, 'sig': sign(text)}, headers=hdr)
    if st != 200: raise SystemExit(f'begin refused: {b}')
    print(f"upload {commit[:7]} {env} -> {b['partition']} ({b['size']} B)")
    boundary = uuid.uuid4().hex
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\n'
            f'Content-Type: application/octet-stream\r\n\r\n').encode() + (pkg / 'firmware.bin').read_bytes() + f'\r\n--{boundary}--\r\n'.encode()
    st, u = http(h, '/api/ota/upload', body, timeout=180, ctype=f'multipart/form-data; boundary={boundary}', headers=hdr)
    if st != 200: raise SystemExit(f'upload refused, boot partition unchanged: {u}')
    record_install(INSTALLED, pkg / 'firmware.bin', f'{commit[:7]}-{env} ota {datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}')
    print('written and verified on the board; restarting into a trial boot')
    hold = rollback_hold(h, password) if args.test_rollback else None
    verdict = watch_confirmation(lambda: http(h, '/api/ota', timeout=4), commit, before['fw'], time.time, time.sleep,
                                 limit=480 if hold else 420, on_trial=hold, target=b['partition'], old_slot=before.get('running'))
    print(verdict)
    if args.test_rollback and verdict.startswith('CONFIRMED'):
        print('WARNING: the hold did not take effect in time; the test image confirmed itself and stays installed '
              '(the same result as a normal install of this package). Check with orcon.py status.')
    want = 'ROLLED BACK' if args.test_rollback else 'CONFIRMED'
    if not verdict.startswith(want): raise SystemExit(1)
    if args.test_rollback:
        print('ROLLBACK TEST PASS: the unconfirmed image was abandoned and the previous one runs')
        print(f"Note: {b['partition']} now holds the abandoned image, so the board has ONE valid image until the next "
              'install. Install the next image soon, or rewrite the previous image there.')


def record_install(path, firmware, label):
    private_dir(path.parent)
    with open(path, 'a') as f: f.write(f'{hashlib.sha256(Path(firmware).read_bytes()).hexdigest()} {label}\n')
    os.chmod(path, 0o600)


def watch_confirmation(get, commit, old_fw, clock, sleep, limit=420, boot_grace=90, on_trial=None, target=None, old_slot=None):
    """The new image must boot (fw == commit, from the target slot the upload wrote) and confirm
    itself (running_state valid). The old slot again after the new image ran, or never leaving it,
    means a rollback. Slots, not only commits: two images of one commit (e.g. the RX-only rollback
    image and the control image) differ only by slot and variant.
    on_trial is called as soon as the new image is seen in its trial boot, and again at every poll of
    that trial boot until it returns True (the image confirms itself 60 s after boot: no single shot)."""
    start, seen_new, seen_trial, last = clock(), False, False, None
    while clock() - start < limit:
        sleep(5)
        try: st, o = get()
        except (urllib.error.URLError, OSError): continue
        if st != 200: continue
        last = o
        new = o.get('fw') == commit and (target is None or o.get('running') == target)
        old = o.get('running') == old_slot if old_slot else o.get('fw') == old_fw
        if new:
            seen_new = True
            if o.get('running_state') == 'pending-verify':
                seen_trial = True
                if on_trial and on_trial(): on_trial = None
            if o.get('running_state') == 'valid':
                # The image confirms itself only after >= 60 s of trial boot; valid without one means
                # the bootloader rollback was bypassed (e.g. the core confirmed it at boot).
                if not seen_trial: return f'NO TRIAL BOOT: {commit[:7]} was already confirmed when first seen; rollback protection is not active'
                return f"CONFIRMED: {commit[:7]} runs from {o['running']} and passed its health check"
        elif old and (seen_new or clock() - start > boot_grace):
            return f"ROLLED BACK: the board runs {str(o.get('fw'))[:7]} ({o.get('variant')}) from {o.get('running')} again; see orcon.py status"
    return f'NO CONFIRMATION within {limit} s; last state: {last}'


def cmd_release(args):
    """A release bundle for every board, from a verified local package (same verification as USB/OTA)."""
    wt, env = Path(args.worktree).resolve(), args.env
    rf_gate(env, args.tx_ack)
    if not args.yes: raise SystemExit('refusing without --yes (explicit approval to sign a public release)')
    commit = subprocess.run(['git', '-C', str(wt), 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    pkg = wt / '.flashtools' / 'local' / f'{commit}-{env}'
    venv = dict(os.environ, LOCAL_ENV=env, **({ENV_ACK[env][0]: ENV_ACK[env][1]} if env in ENV_ACK else {}))
    v = subprocess.run(['bash', 'tools/heltec_v2.sh', 'verify', commit], cwd=wt, env=venv, capture_output=True, text=True)
    if v.returncode or 'VERIFIED' not in v.stdout: raise SystemExit('package verification failed:\n' + v.stdout[-800:] + v.stderr[-800:])
    try: text = release_manifest(env, commit, pkg / 'firmware.bin', args.version)
    except ValueError as e: raise SystemExit(str(e))
    out = Path(args.out or wt / 'release').resolve(); out.mkdir(parents=True, exist_ok=True)
    name = out / f'orcon-{ENV_VARIANT[env][0]}-{args.version}-{commit[:7]}.orconfw'
    write_bundle(name, text, sign(text), pkg / 'firmware.bin')
    print(f'{name}  ({name.stat().st_size} B, sha256 {hashlib.sha256(name.read_bytes()).hexdigest()})')


def cmd_keygen(args):
    private_dir(KEY.parent)
    if not KEY.exists():
        subprocess.run(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', str(KEY)], check=True)
        os.chmod(KEY, 0o600)
    pub = subprocess.run(['openssl', 'ec', '-in', str(KEY), '-pubout'], capture_output=True, text=True, check=True).stdout.strip()
    lines = '\n'.join(f'    "{l}\\n"' for l in pub.splitlines())
    print(lines)


# ---- backup-check: the board against an NVS dump, field by field, never a value ------------------------------
def backup_view(vals):
    """What the board's API shows, rebuilt from NVS values {(namespace, key): raw bytes} (tools/nvs_keys.py):
    RF IDs, OLED mode, WiFi name and the profiles (format: src/remote_profiles.cpp encode())."""
    u32 = lambda k: int.from_bytes(vals.get(('orcon', k), b'\0'), 'little')
    text = lambda k, dflt=None: vals[('orcon', k)].split(b'\0', 1)[0].decode() if ('orcon', k) in vals else dflt
    rid = lambda raw: f'{raw >> 18:02d}:{raw & 0x3FFFF:06d}'
    b = vals.get(('orconprofiles', 'data'), b'')
    if b and (len(b) < 18 or b[:5] != b'OCRP\x01' or len(b) != 18 + 127 * b[5] or zlib.crc32(b[:-4]) != int.from_bytes(b[-4:], 'little')):
        raise ValueError('profile blob is not readable')
    items = [b[14 + 127 * i:141 + 127 * i] for i in range(b[5])] if b else []
    return {'own': f"{u32('uc'):02d}:{u32('us'):06d}", 'unit': f"{u32('fc'):02d}:{u32('fs'):06d}",
            'oled': text('oled_mode', 'auto'), 'ssid': text('ssid'), 'active': int.from_bytes(b[10:14], 'little') if b else 0,
            'profiles': [{'id': int.from_bytes(p[:4], 'little'), 'name': p[4:53].split(b'\0', 1)[0].decode(),
                          'remote': rid(int.from_bytes(p[53:57], 'little')), 'unit': rid(int.from_bytes(p[57:61], 'little'))}
                         for p in items]}


def board_view(h, hdr, get=http):
    """The same fields from the board: /api/state (open) plus /api/netcfg and /api/profiles (logged in)."""
    (s1, st), (s2, net), (s3, prof) = get(h, '/api/state'), get(h, '/api/netcfg', headers=hdr), get(h, '/api/profiles', headers=hdr)
    if (s1, s2, s3) != (200, 200, 200): raise SystemExit(f'the board refused a read (HTTP {s1}/{s2}/{s3})')
    return {'own': st.get('us_id'), 'unit': st.get('fan_id'), 'oled': st.get('oled_mode'), 'ssid': net.get('ssid'),
            'active': prof.get('active'), 'profiles': [{k: p.get(k) for k in ('id', 'name', 'remote', 'unit')} for p in prof.get('items', [])]}


BACKUP_FIELDS = (('own', 'eigen RF-ID'), ('unit', 'unit-ID'), ('profiles', 'profielen: aantal, namen, afstandsbediening en unit'),
                 ('active', 'actief profiel'), ('ssid', 'WiFi-netwerk'), ('oled', 'OLED-stand'))


def backup_rows(backup, board):
    """One line per field, 'gelijk' or 'ANDERS': never a value (IDs, names and the network name stay private)."""
    return [f"{'gelijk' if backup[k] == board.get(k) else 'ANDERS':7} {label}" for k, label in BACKUP_FIELDS]


def cmd_backup_check(args):
    import nvs_keys
    try: backup = backup_view({k: v for k, (_, v) in nvs_keys.values(Path(args.nvs).read_bytes()).items()})
    except (OSError, ValueError) as e: raise SystemExit(f'NVS dump not usable: {e}')
    h = host(args)
    rows = backup_rows(backup, board_view(h, login(h, getpass.getpass('Webwachtwoord van de controller: '))))
    print('\n'.join(rows))
    if any(r.startswith('ANDERS') for r in rows): raise SystemExit('the board does NOT match this backup')
    print('the board matches this backup')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host')
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('status'); sub.add_parser('capture'); sub.add_parser('keygen')
    lg = sub.add_parser('logs'); lg.add_argument('--minutes', type=float, default=60)
    oi = sub.add_parser('ota-install'); oi.add_argument('--worktree', required=True); oi.add_argument('--env', required=True)
    oi.add_argument('--yes', action='store_true'); oi.add_argument('--tx-ack', '--rf-ack', dest='tx_ack', default='')
    oi.add_argument('--test-rollback', action='store_true')
    rl = sub.add_parser('release'); rl.add_argument('--worktree', required=True); rl.add_argument('--env', required=True)
    rl.add_argument('--version', type=int, required=True); rl.add_argument('--yes', action='store_true')
    rl.add_argument('--tx-ack', dest='tx_ack', default=''); rl.add_argument('--out')
    sub.add_parser('backup-check').add_argument('--nvs', required=True)
    tw = sub.add_parser('trial-watch'); tw.add_argument('--minutes', type=float, default=12)
    tw.add_argument('--every', type=float, default=3); tw.add_argument('--serial'); tw.add_argument('--anyway', action='store_true')
    a = p.parse_args(argv)
    {'status': cmd_status, 'logs': cmd_logs, 'capture': cmd_capture, 'ota-install': cmd_ota_install, 'keygen': cmd_keygen,
     'release': cmd_release, 'backup-check': cmd_backup_check, 'trial-watch': cmd_trial_watch}[a.cmd](a)


if __name__ == '__main__':
    main()
