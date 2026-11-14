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
                                        (POST /api/ota/hold, logged in); PASS only if the previous image returns
  orcon.py auth-code                    web login: a one-time code (10 min) to set or reset the
                                        password, issued only for a request signed with the OTA key
  orcon.py keygen                       create the signing key (once) and src/ota_pubkey.h

Host: --host, $ORCON_HOST, ~/.orcon/config.json {"host": ...}, else orcon15rf.local.
Captures hold real device IDs: they go to captures/ (git-ignored), directories 0700, files 0600.
Nothing here uses GitHub or the network beyond the board itself.
"""
import argparse, getpass, hashlib, json, os, subprocess, sys, tempfile, time, urllib.error, urllib.request, uuid
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
    then ask it not to confirm itself. The board only accepts this from a logged-in owner."""
    def hold():
        print('rollback test: hold ->', post(h, '/api/ota/hold', {}, headers=login_fn(h, password)))
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


def rf_gate(env, ack):
    """An image that can transmit needs its own explicit phrase, the same as the USB flash path."""
    if ENV_VARIANT.get(env, (None, False))[1]:
        need = ENV_ACK[env][1]
        if ack != need: raise SystemExit(f'{env} can transmit: pass --tx-ack "{need}" to install it')


def reset_request(mac, nonce):
    """The signed web-login recovery request: exactly these fields, for this board and nonce."""
    return json.dumps({'v': 1, 'kind': 'orcon-auth-reset', 'mac': mac.lower(), 'nonce': nonce}, separators=(',', ':'))


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
    # Asked before anything is written: the hold is sent minutes later, inside the trial boot.
    password = getpass.getpass('Webwachtwoord van de controller (rollbacktest): ') if args.test_rollback else ''
    st, before = http(h, '/api/ota')
    if st != 200 or before.get('signed_ota') != 'ORCON-SIGNED-OTA v1': raise SystemExit(f'board has no signed OTA (HTTP {st}); USB bootstrap first')
    if before.get('running_state') == 'pending-verify': raise SystemExit('board is still confirming its running image; wait')
    st, n = http(h, '/api/ota/nonce', {})
    if st != 200: raise SystemExit(f'nonce refused: {n}')
    text = build_manifest(mac, env, commit, pkg / 'firmware.bin', n['nonce'])
    st, b = http(h, '/api/ota/begin', {'manifest': text, 'sig': sign(text)})
    if st != 200: raise SystemExit(f'begin refused: {b}')
    print(f"upload {commit[:7]} {env} -> {b['partition']} ({b['size']} B)")
    boundary = uuid.uuid4().hex
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\n'
            f'Content-Type: application/octet-stream\r\n\r\n').encode() + (pkg / 'firmware.bin').read_bytes() + f'\r\n--{boundary}--\r\n'.encode()
    st, u = http(h, '/api/ota/upload', body, timeout=180, ctype=f'multipart/form-data; boundary={boundary}')
    if st != 200: raise SystemExit(f'upload refused, boot partition unchanged: {u}')
    record_install(INSTALLED, pkg / 'firmware.bin', f'{commit[:7]}-{env} ota {datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}')
    print('written and verified on the board; restarting into a trial boot')
    hold = rollback_hold(h, password) if args.test_rollback else None
    verdict = watch_confirmation(lambda: http(h, '/api/ota', timeout=4), commit, before['fw'], time.time, time.sleep,
                                 limit=480 if hold else 420, on_trial=hold, target=b['partition'], old_slot=before.get('running'))
    print(verdict)
    want = 'ROLLED BACK' if args.test_rollback else 'CONFIRMED'
    if not verdict.startswith(want): raise SystemExit(1)
    if args.test_rollback: print('ROLLBACK TEST PASS: the unconfirmed image was abandoned and the previous one runs')


def record_install(path, firmware, label):
    private_dir(path.parent)
    with open(path, 'a') as f: f.write(f'{hashlib.sha256(Path(firmware).read_bytes()).hexdigest()} {label}\n')
    os.chmod(path, 0o600)


def watch_confirmation(get, commit, old_fw, clock, sleep, limit=420, boot_grace=90, on_trial=None, target=None, old_slot=None):
    """The new image must boot (fw == commit, from the target slot the upload wrote) and confirm
    itself (running_state valid). The old slot again after the new image ran, or never leaving it,
    means a rollback. Slots, not only commits: two images of one commit (e.g. the RX-only rollback
    image and the control image) differ only by slot and variant.
    on_trial is called once, as soon as the new image is seen in its trial boot."""
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
                if on_trial: on_trial(); on_trial = None
            if o.get('running_state') == 'valid':
                # The image confirms itself only after >= 60 s of trial boot; valid without one means
                # the bootloader rollback was bypassed (e.g. the core confirmed it at boot).
                if not seen_trial: return f'NO TRIAL BOOT: {commit[:7]} was already confirmed when first seen; rollback protection is not active'
                return f"CONFIRMED: {commit[:7]} runs from {o['running']} and passed its health check"
        elif old and (seen_new or clock() - start > boot_grace):
            return f"ROLLED BACK: the board runs {str(o.get('fw'))[:7]} ({o.get('variant')}) from {o.get('running')} again; see orcon.py status"
    return f'NO CONFIRMATION within {limit} s; last state: {last}'


def cmd_auth_code(args):
    """Web login recovery: the board issues a one-time code only for a request signed with the
    OTA key on this Mac. Enter it in the web page ("Wachtwoord vergeten") with a new password."""
    h = host(args)
    st, n = http(h, '/api/auth/nonce', {})
    if st == 404: raise SystemExit('this firmware has no web login (no /api/auth)')
    if st != 200 or 'mac' not in n: raise SystemExit(f'nonce refused (HTTP {st}): {n}')
    text = reset_request(n['mac'], n['nonce'])
    st, r = http(h, '/api/auth/reset', {'manifest': text, 'sig': sign(text)})
    if st != 200: raise SystemExit(f'recovery request refused (HTTP {st}): {r.get("error", r)}')
    print(f"Herstelcode: {r['code']}   (eenmalig, {r['ttl_s'] // 60} minuten geldig)")
    print('Open de webinterface, kies "Wachtwoord vergeten" (of het instelscherm) en stel een nieuw wachtwoord in.')


def cmd_keygen(args):
    private_dir(KEY.parent)
    if not KEY.exists():
        subprocess.run(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', str(KEY)], check=True)
        os.chmod(KEY, 0o600)
    pub = subprocess.run(['openssl', 'ec', '-in', str(KEY), '-pubout'], capture_output=True, text=True, check=True).stdout.strip()
    lines = '\n'.join(f'    "{l}\\n"' for l in pub.splitlines())
    print(lines)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host')
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('status'); sub.add_parser('capture'); sub.add_parser('keygen'); sub.add_parser('auth-code')
    lg = sub.add_parser('logs'); lg.add_argument('--minutes', type=float, default=60)
    oi = sub.add_parser('ota-install'); oi.add_argument('--worktree', required=True); oi.add_argument('--env', required=True)
    oi.add_argument('--yes', action='store_true'); oi.add_argument('--tx-ack', '--rf-ack', dest='tx_ack', default='')
    oi.add_argument('--test-rollback', action='store_true')
    a = p.parse_args(argv)
    {'status': cmd_status, 'logs': cmd_logs, 'capture': cmd_capture, 'ota-install': cmd_ota_install, 'keygen': cmd_keygen,
     'auth-code': cmd_auth_code}[a.cmd](a)


if __name__ == '__main__':
    main()
