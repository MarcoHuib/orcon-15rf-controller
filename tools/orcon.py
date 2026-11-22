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
  orcon.py trial-watch --key-proven FINGERPRINT [--minutes N] [--every S] [--serial PORT] [--anyway]
                                        read-only record of one trial boot, started BEFORE installing: a
                                        preflight (running == boot, valid, owner state done, WiFi, RX, and the
                                        owner key proven by hand: the board's fingerprint after a recovery with
                                        the key file, because no home-network route shows the key), then
                                        /api/ota, /api/diag, /api/setup/state and the RF console every S s (2..60,
                                        default 3) for up to N min (6..120, default 12), per boot, until the new
                                        image confirms itself or rolls back, then the condition that held it back
                                        (optionally the USB log, opened and closed without a reset). GET only, on a
                                        direct connection (no proxy, no redirect, no login); logs keep no secrets

Owner-model firmware (it reports "owner_model" in /api/ota) only installs with the owner's session:
ota-install then asks for the web password. Older firmware installs as before.

Host: --host, $ORCON_HOST, ~/.orcon/config.json {"host": ...}, else orcon15rf.local.
Captures hold real device IDs: they go to captures/ (git-ignored), directories 0700, files 0600.
Nothing here uses GitHub or the network beyond the board itself.
"""
import argparse, getpass, hashlib, json, os, re, subprocess, sys, tempfile, threading, time, urllib.error, urllib.request, uuid, zlib
from datetime import datetime, timezone
from http.client import HTTPConnection, HTTPException   # trial-watch: a direct connection (no proxy, no redirect)
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


# ---- trial-boot watch (read only; tested by tools/test_orcon_cli.py) ---------------------------------------------
# The control image confirms its own trial boot (ota_signed.cpp tick): from 60 s after boot, at most once a second,
# while not held (/api/ota hold), on the home network (/api/diag wifi.up) and receiving (rx.state 0), and only with a
# usable owner key (a console note, once per boot, when it is not). At 300 s an unconfirmed image rolls back. The board
# does not report the result of esp_ota_mark_app_valid_cancel_rollback, so a healthy image that stays unconfirmed is
# "unknown" here, never a proven cause (Codex trial-watch r1).
CONFIRM_FROM_S, ROLLBACK_AT_S, MIN_EVERY_S, MIN_MINUTES, MAX_MINUTES = 60, 300, 2.0, 6.0, 120.0
FINGERPRINT = re.compile(r'[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}')   # ownerkey::fingerprint: SHA-256 of the public key, 6 bytes
LABEL = r'[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?'
TRIAL_HOST = re.compile(rf'({LABEL}(?:\.{LABEL})*)(?::([0-9]{{1,5}}))?', re.ASCII)   # a name or IPv4, an optional port, nothing else
HEX_SECRET = re.compile(r'(?<![0-9A-Fa-f])(?:[0-9A-Fa-f]{64}|[0-9A-Fa-f]{32})(?![0-9A-Fa-f])')
SESSION_NO = re.compile(r'\b(sessie|session|sid)(\W{1,3})[0-9]+', re.I)   # the RF read session number in console texts
IPV4 = re.compile(r'\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b')                    # e.g. "WiFi: weer verbonden ... (adres <IP>)"
# The USB lines a log keeps: the board's own status lines. RF frame lines ([rx ...], [sync ...]: ids, payloads) are counted only.
SERIAL_KEEP = ('[ota]', '[owner]', '[radio]', '[wifi]', '[reset]', '[FATAL]', '[rxstat]', 'Orcon 15RF emulator starting', 'Ready.', 'rst:', 'ets ')
# Of each reply only these fields reach a log (allowlist): states and counters, no address, name, id or token.
KEEP = {'ota': ('fw', 'variant', 'running', 'running_state', 'boot', 'next', 'next_state', 'session', 'last_error', 'hold', 'release'),
        'diag': ('fw', 'variant', 'uptime_s', 'reset_reason', 'boots', 'previous_uptime_s', 'heap_free', 'heap_min', 'running', 'running_state',
                 'rx', 'wifi', 'coredump'),
        'state': ('fw', 'auth', 'wifi', 'setup')}
KEEP_SUB = {'wifi': ('up', 'rssi', 'outages', 'retries', 'down_s', 'last_outage_s', 'ip_changes', 'last_reason', 'watch_restart',
                     'restart_block', 'ap'),
            'setup': ('state', 'via_ap', 'physical', 'key'), 'auth': ('enabled', 'logged_in', 'password_set', 'login_wait_s'),
            'rx': ('state', 'frames', 'recoveries', 'lost', 'uart_overflows', 'queue_drops')}
ROUTES = (('ota', '/api/ota'), ('diag', '/api/diag'), ('state', '/api/setup/state'))   # the preflight
TRIAL_SLEEP = time.sleep


def redact(v):
    """Text a log may keep: no 32/64-hex run (session id, CSRF token, Siri key), no RF read session number, no IPv4
    address. 40-hex commits stay."""
    if isinstance(v, dict): return {k: redact(x) for k, x in v.items()}
    if isinstance(v, list): return [redact(x) for x in v]
    return IPV4.sub('<address>', SESSION_NO.sub(r'\1\2<redacted>', HEX_SECRET.sub('<redacted>', v))) if isinstance(v, str) else v


def keep(route, j):
    """Only the fields the watch needs from one reply (allowlist), redacted; None for no reply."""
    if not isinstance(j, dict): return None
    out = {k: j[k] for k in KEEP[route] if k in j}
    for k, allowed in KEEP_SUB.items():
        if isinstance(out.get(k), dict): out[k] = {x: y for x, y in out[k].items() if x in allowed}
    if 'coredump' in out: out['coredump'] = out['coredump'] is not None   # present or not; its contents stay on the board
    return redact(out)


def trial_get(host, port):
    """GET on a direct connection to the board: no proxy, no redirect, no header but Host (Codex trial-watch r1).
    (status, JSON object or None); network and protocol errors raise."""
    def get(path):
        c = HTTPConnection(host, port, timeout=4)
        try:
            c.putrequest('GET', path, skip_host=True, skip_accept_encoding=True)
            c.putheader('Host', host if port == 80 else f'{host}:{port}')
            c.endheaders()
            r = c.getresponse()
            body, length = r.read(65537), r.getheader('Content-Length')
            if r.status != 200 or len(body) > 65536 or length is None or not length.isdigit() or int(length) != len(body):
                return r.status, None           # only a complete 200 reply is evidence (the board always sends a length)
            try: j = json.loads(body)
            except ValueError: j = None
            return r.status, j if isinstance(j, dict) else None
        finally:
            c.close()
    return get


def one(get, path):
    """(status, body); the error's name as status when the request failed."""
    try: return get(path)
    except (OSError, HTTPException) as e: return type(e).__name__, None


def read_preflight(get):
    return {name: one(get, path) for name, path in ROUTES}


def read_round(get, cursor):
    """One round in a fixed order: /api/diag, the console from `cursor` (up to 4 pages of 16), /api/ota, /api/diag
    again. The two diag readings bracket the rest: when both show one boot, the pages and the OTA reply are of it."""
    r = {'diag1': one(get, '/api/diag'), 'pages': []}
    for _ in range(4):
        st, page = one(get, f'/api/setup/console?after={cursor}')
        r['pages'].append((st, page))
        if st != 200 or page is None or not page.get('more') or not isinstance(page.get('next'), int) or page['next'] < 1: break
        cursor = page['next'] - 1
    r['ota'], r['diag2'] = one(get, '/api/ota'), one(get, '/api/diag')
    return r


def preflight(ota, diag, state, key_proof=None):
    """What must hold before an install, so the new image can confirm itself (docs/HERSTELSLEUTEL_MIGRATIE.md step 7).
    No home-network route shows the owner key: /api/setup/state says 'done' also for an unreadable key, its logged-in
    'key' is no proof either (b75fad3 checks length and prefix only), and the fingerprint and crypto_ok are served on the
    setup AP only (/api/provision/state). So GO needs the owner's attestation by hand (key_proof)."""
    p, rx = [], (diag.get('rx') or {}).get('state')
    if not FINGERPRINT.fullmatch(key_proof or ''):
        p.append("owner key not provable from the home network ('done' also covers an unreadable key; the fingerprint is "
                 "on the setup AP only): attest it on this board by hand (migration steps 1-5: a password recovery with the "
                 "key file in the PRG recovery mode, and the board's fingerprint equal to the file's 'Vingerafdruk'), then "
                 "--key-proven xxxx-xxxx-xxxx")
    if not ota.get('next') or ota.get('next') == ota.get('running'): p.append(f"no other slot to install into (next {ota.get('next')})")
    if not ota.get('running') or ota.get('running') != ota.get('boot'):
        p.append(f"running {ota.get('running')} != boot {ota.get('boot')}: the old updater would write the selected slot")
    if ota.get('running_state') != 'valid': p.append(f"running_state {ota.get('running_state')}: not a confirmed image")
    owner = (state.get('setup') or {}).get('state')
    if owner != 'done':
        p.append(f"owner state {owner}: register the recovery key in the PRG recovery mode first (legacy = no key)")
    if not (diag.get('wifi') or {}).get('up'): p.append('WiFi not joined')
    if rx != 0: p.append(f'RX state {rx}: not receiving')
    return p


def same_boot(a, b):
    """Two /api/diag readings of one boot: the same boot counter, slot and firmware, and an uptime that did not go back.
    (A power cut resets the counter to 1; the slot and firmware still tell a trial boot apart. A restart of the same
    image with the same counter, unseen for longer than its previous uptime, cannot be told apart.)"""
    return (isinstance(a, dict) and isinstance(b, dict) and isinstance(a.get('uptime_s'), int) and isinstance(b.get('uptime_s'), int)
            and (a.get('boots'), a.get('running'), a.get('fw')) == (b.get('boots'), b.get('running'), b.get('fw')) and b['uptime_s'] >= a['uptime_s'])


def trial_view(diag, ota):
    """One reading: slot, state, uptime and health from /api/diag; hold and the other slot from /api/ota only when that
    reply is of the same image (otherwise unknown)."""
    w, rx = diag.get('wifi') or {}, diag.get('rx') or {}
    o = ota if isinstance(ota, dict) and (ota.get('running'), ota.get('fw')) == (diag.get('running'), diag.get('fw')) else {}
    return {'fw': (diag.get('fw') or '')[:7], 'slot': diag.get('running'), 'state': diag.get('running_state'), 'up_s': diag['uptime_s'],
            'boots': diag.get('boots'), 'hold': o.get('hold'), 'next_state': o.get('next_state'),
            'wifi': w.get('up') if isinstance(w.get('up'), bool) else None, 'rssi': w.get('rssi'),
            'rx': rx.get('state') if isinstance(rx.get('state'), int) else None}


def new_watch():
    return {'epoch': 0, 'last': None, 'cursor': 0, 'rounds': [], 'events': [], 'gaps': [], 'incomplete': set()}


def take_page(w, con, epoch, now):
    """One console page into boot `epoch`; a page that lost or skipped events marks that boot's console incomplete."""
    ev = [e for e in con.get('ev') or [] if isinstance(e, dict)]
    if con.get('lost') or (ev and ev[0].get('s') != w['cursor'] + 1): w['incomplete'].add(epoch)
    for e in ev: w['events'].append({'host_t': now, 'epoch': epoch, 's': e.get('s'), 't': e.get('t'), 'k': e.get('k'), 'm': redact(str(e.get('m', '')))})
    if isinstance(con.get('next'), int) and con['next'] > 0: w['cursor'] = con['next'] - 1


def fold(w, r, now):
    """One round into the watch. A new boot: another boot counter, slot or firmware than the last reading, or its uptime
    went back. The pages and the OTA reply count only when the round's two /api/diag readings show one boot, and pages
    only when that boot is the one the cursor belongs to. Returns the reading, or None."""
    for name in ('diag1', 'ota', 'diag2'):
        if r[name][0] != 200 or r[name][1] is None: w['gaps'].append({'t': now, 'epoch': w['epoch'], 'route': name, 'status': r[name][0]})
    d1, d2 = r['diag1'][1], r['diag2'][1]
    first = d1 if same_boot(d1, d2) else d2 if isinstance(d2, dict) and isinstance(d2.get('uptime_s'), int) else None
    if first is None: w['incomplete'].add(w['epoch']); return None
    restarted = w['last'] is not None and not same_boot(w['last'], first)
    if restarted: w['incomplete'].add(w['epoch'])   # its last events may not have been read
    w['epoch'] += restarted
    w['last'] = d2
    if not same_boot(d1, d2):                       # a restart inside the round: none of it is used, the new boot is read from 0
        w['gaps'].append({'t': now, 'epoch': w['epoch'], 'route': 'round', 'status': 'restart-inside'}); w['cursor'] = 0
        return None
    if restarted: w['cursor'] = 0                   # pages read with the old boot's cursor may skip the new boot's first events
    else:
        for st, page in r['pages']:
            if st != 200 or page is None:
                w['incomplete'].add(w['epoch']); w['gaps'].append({'t': now, 'epoch': w['epoch'], 'route': 'console', 'status': st}); break
            take_page(w, page, w['epoch'], now)
    v = trial_view(d2, r['ota'][1]); v['epoch'] = w['epoch']
    w['rounds'].append(v)
    return v


def trial_outcome(start, rounds):
    """'confirmed': the target slot runs, valid. 'rolled back': after the trial boot the start slot runs again, valid.
    None while open, and when the trial boot itself was never read: slots and restarts alone cannot tell a rollback from
    an install that never started (the target may be invalid from an earlier attempt)."""
    seen = next((r['epoch'] for r in rounds if r['slot'] == start.get('target')), None)
    for r in rounds if seen is not None else []:
        if r['epoch'] < seen: continue
        if r['slot'] == start['target'] and r['state'] == 'valid': return 'confirmed'
        if r['slot'] == start.get('slot') and r['state'] == 'valid' and r['epoch'] > seen: return 'rolled back'
    return None


def blockers(r):
    """The confirmation inputs that were false in one reading, in the firmware's order; None when one was not read."""
    if r['hold'] is None or r['wifi'] is None or r['rx'] is None: return None
    return [k for k, bad in (('hold', r['hold']), ('wifi', not r['wifi']), ('rx', r['rx'] != 0)) if bad]


def trial_verdict(start, w):
    """(code, why) from the trial boot's own readings and console events only: an earlier boot never counts."""
    if trial_outcome(start, w['rounds']) == 'confirmed': return 'confirmed', 'the new image confirmed itself'
    seen = next((r['epoch'] for r in w['rounds'] if r['slot'] == start.get('target')), None)
    if seen is None: return 'no-data', f"the trial boot was never read ({w['epoch']} restarts seen): the outcome is open"
    notes = [e['m'] for e in w['events'] if e['epoch'] == seen]
    if any('nieuwe firmware bevestigd' in n for n in notes): return 'confirmed', 'the trial boot noted its own confirmation'
    if any('geen herstelsleutel' in n for n in notes):
        return 'owner-key', ('the owner-key check failed at least once (the firmware notes it once per boot) and the image was not '
                             'confirmed: most likely no usable owner key on this board')
    due = [r for r in w['rounds'] if r['epoch'] == seen and r['state'] == 'pending-verify' and CONFIRM_FROM_S <= r['up_s'] < ROLLBACK_AT_S]
    known = [(r, b) for r, b in ((r, blockers(r)) for r in due) if b is not None]
    if not known: return 'no-data', f'no complete reading of the trial boot between {CONFIRM_FROM_S} and {ROLLBACK_AT_S} s'
    due, why = [r for r, _ in known], [b for _, b in known]
    if any('hold' in b for b in why): return 'hold', 'the trial boot was held (a rollback test)'
    if all(why):
        n = {k: sum(k in b for b in why) for k in ('wifi', 'rx') if any(k in b for b in why)}
        return '+'.join(n), f'not healthy in any of {len(due)} readings after {CONFIRM_FROM_S} s: ' + ', '.join(f'{k} {c}x' for k, c in n.items())
    ok_at = [r['up_s'] for r, b in zip(due, why) if not b]
    lost = seen in w['incomplete'] or any(g['epoch'] == seen and g['route'] == 'console' for g in w['gaps'])
    return 'unknown', (f'healthy in {len(ok_at)} of {len(due)} readings (first at {ok_at[0]} s) and no owner-key note'
                       + (', but the console of that boot is incomplete' if lost else '')
                       + ': the firmware does not say why it stayed unconfirmed (the ESP-IDF call, or a condition that changed between readings)')


def serial_close(port):
    """RTS first, then DTR, then close: EN is pulled low only while RTS is held and DTR is not (rxmon.py), and a reset
    in a trial boot is a rollback."""
    try:
        port.rts = False
        port.dtr = False
    finally:
        port.close()


def serial_tee(port, path, stop, seen):
    """The USB log until `stop`: the board's own status lines only (SERIAL_KEEP), redacted, with UTC times; RF frame
    lines are counted, not kept. It never closes the port: the caller owns it (serial_close)."""
    try:
        with path.open('a') as f:
            os.chmod(path, 0o600)
            buf = b''
            while not stop.is_set():
                buf += port.read(512)
                while b'\n' in buf:
                    raw, buf = buf.split(b'\n', 1)
                    line = raw.decode('utf-8', 'replace').rstrip('\r')
                    if not line.startswith(SERIAL_KEEP): seen['dropped'] += 1; continue
                    line = redact(line)
                    f.write(f'{datetime.now(timezone.utc).isoformat()} {line}\n'); f.flush()
                    if line.startswith(('[ota]', '[owner]', '[radio]', '[wifi]', '[FATAL]')): print('  usb:', line)
    except Exception as e:   # a lost port ends the USB log, never the watch
        seen['error'] = type(e).__name__


def cmd_trial_watch(args):
    if not (MIN_EVERY_S <= args.every <= 60 and MIN_MINUTES <= args.minutes <= MAX_MINUTES):   # NaN fails too
        raise SystemExit(f'--every {MIN_EVERY_S:g}..60 s and --minutes {MIN_MINUTES:g}..{MAX_MINUTES:g}: the trial takes 300 s, the board needs a calm loop')
    hp = TRIAL_HOST.fullmatch(host(args) or '')
    port = int(hp.group(2) or 80) if hp else 0
    if not 0 < port < 65536: raise SystemExit('trial-watch needs a host name or IPv4 address, optionally with :port, and nothing else')
    get = trial_get(hp.group(1), port)
    pre = read_preflight(get)
    if any(st != 200 or body is None for st, body in pre.values()):
        raise SystemExit('board not readable (try again): ' + ', '.join(f'{k} {st}' for k, (st, _) in pre.items()))
    ota, diag, state = (pre[k][1] for k in ('ota', 'diag', 'state'))
    problems = preflight(ota, diag, state, args.key_proven)
    proof = ({'fingerprint': args.key_proven, 'by': 'owner, by hand (migration steps 1-5)', 'utc': datetime.now(timezone.utc).isoformat()}
             if FINGERPRINT.fullmatch(args.key_proven or '') else {'rejected': 'not a fingerprint, not stored'} if args.key_proven else None)
    d = private_dir(REPO / 'captures' / 'trial' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ'))
    write_private(d / 'preflight.json', json.dumps({**{k: keep(k, pre[k][1]) for k, _ in ROUTES}, 'key_proof': proof, 'problems': problems}, indent=1))
    for p in problems: print('PREFLIGHT NO-GO:', p)
    if problems and not args.anyway: raise SystemExit(f'do not install now (saved {d}); --anyway watches all the same')
    start, w, stop, serial = {'slot': ota.get('running'), 'target': ota.get('next')}, new_watch(), threading.Event(), {'dropped': 0, 'error': None}
    out = {n: (d / f'{n}.jsonl').open('a') for n in ('polls', 'console', 'gaps')}
    for f in out.values(): os.chmod(f.name, 0o600)
    usb = tee = None
    try:
        if args.serial:
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from rxmon import open_serial
            try: usb = open_serial(args.serial)   # now, before the install: DTR and RTS held, so opening never resets the board
            except ImportError: raise SystemExit('the USB log needs pyserial: run with .flashtools/venv/bin/python')
            tee = threading.Thread(target=serial_tee, args=(usb, d / 'serial.log', stop, serial), daemon=True)
            tee.start()
        deadline, told, done_ev, done_gap = time.monotonic() + args.minutes * 60, False, 0, 0
        while time.monotonic() < deadline:
            now = datetime.now(timezone.utc).isoformat()
            r = read_round(get, w['cursor'])
            v = fold(w, r, now)
            for e in w['events'][done_ev:]: out['console'].write(json.dumps(e) + '\n'); print('  console:', e['k'], e['m'])
            for g in w['gaps'][done_gap:]: out['gaps'].write(json.dumps(g) + '\n')
            done_ev, done_gap = len(w['events']), len(w['gaps'])
            if v is not None:
                out['polls'].write(json.dumps({'host_t': now, 'view': v, 'ota': keep('ota', r['ota'][1]), 'diag': keep('diag', r['diag2'][1])}) + '\n')
                print(f"{now[11:19]} boot {v['epoch']} {v['slot']} {v['fw']} {v['state']} up {v['up_s']} s | hold {v['hold']} | "
                      f"wifi {v['wifi']} ({v['rssi']} dBm) | rx {v['rx']}")
                if not told:
                    told = True
                    if not same_boot(diag, r['diag1'][1]):   # the board restarted or changed since the preflight: no GO on old facts
                        print('the board changed since the preflight: no GO, run trial-watch again'); break
                    if not problems: print(f'preflight GO (owner key attested by hand: {args.key_proven}); recording now: install in the web page (Systeem > Firmware)')
            else: print(now[11:19], 'no consistent reading this round (a restart, or the board is away)')
            for f in out.values(): f.flush()
            if trial_outcome(start, w['rounds']): break
            TRIAL_SLEEP(max(0.0, min(args.every if v is not None else max(args.every, 5.0), deadline - time.monotonic())))   # back off while away
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        try:
            if tee is not None and tee.ident is not None: tee.join(5)
        finally:
            if usb is not None: serial_close(usb)   # the one owner of the port closes it, on every path
            for f in out.values(): f.close()
            code, why = trial_verdict(start, w)
            write_private(d / 'verdict.json', json.dumps({'verdict': code, 'why': why, 'end': trial_outcome(start, w['rounds']), 'restarts_seen': w['epoch'],
                                                          'incomplete_console_boots': sorted(w['incomplete']), 'usb': serial if args.serial else None}, indent=1))
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
    tw.add_argument('--key-proven', metavar='FINGERPRINT')
    a = p.parse_args(argv)
    {'status': cmd_status, 'logs': cmd_logs, 'capture': cmd_capture, 'ota-install': cmd_ota_install, 'keygen': cmd_keygen,
     'release': cmd_release, 'backup-check': cmd_backup_check, 'trial-watch': cmd_trial_watch}[a.cmd](a)


if __name__ == '__main__':
    main()
