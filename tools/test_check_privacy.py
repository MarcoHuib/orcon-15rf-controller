"""Local test for the private-key guard in tools/check_privacy.py (Codex OWNER-12): real keys in every
representation are refused, markers and fake fixtures are not. Keys are generated with openssl in a
temporary directory and deleted afterwards; none is ever written into the repository."""
import json, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ok = fail = 0
def check(cond, name):
    global ok, fail
    if cond: ok += 1; print('  [ok]', name)
    else: fail += 1; print('  [FAIL]', name)

def flagged(path):
    r = subprocess.run([sys.executable, str(ROOT / 'tools/check_privacy.py'), str(path)], capture_output=True, text=True)
    return 'private key material' in r.stdout

def openssl(*args, **kw):
    return subprocess.run(['openssl', *args], capture_output=True, text=True, check=True, **kw).stdout

# Moved from the workflow's privacy step: real-looking IDs/MAC/LAN IP and a personal home path are refused.
# Assembled at run time, so this file itself contains no such values.
with tempfile.TemporaryDirectory() as d:
    for name, text in (('real-data.txt', 'id 29:%s\nmac 3c:%s\nip 192.168.%s\n' % ('123456', 'aa:bb:cc:dd:ee', '50.7')),
                       ('home-path.txt', 'cd /Users/%s/project\n' % 'someone')):
        p = Path(d) / name; p.write_text(text)
        r = subprocess.run([sys.executable, str(ROOT / 'tools/check_privacy.py'), str(p)], capture_output=True, text=True)
        check(r.returncode != 0, f'{name}: refused')

# IPv4 addresses in every usual spot are refused; a dotted number of more than four groups (an OID such as Matter's
# vendor attribute 1.3.6.1.4.1.37244.2.1, or a version) is no address.
with tempfile.TemporaryDirectory() as d:
    lan = '192.168.' + '50.7'
    for name, text, want in (('ip-text.txt', f'host {lan} here', True), ('ip-end.txt', f'see {lan}.', True), ('ip-url.txt', f'http://{lan}/x', True),
                             ('ip-port.txt', f'{lan}:80', True), ('ip-quoted.txt', f'"{lan}"', True), ('oid.txt', 'oid 1.3.6.1.4.1.37244.2.1', False),
                             ('version.txt', 'version 1.2.3.4.5', False)):
        p = Path(d) / name; p.write_text(text + '\n')
        r = subprocess.run([sys.executable, str(ROOT / 'tools/check_privacy.py'), str(p)], capture_output=True, text=True)
        check(('IPv4 address' in r.stdout) == want, f"{name}: {'refused' if want else 'allowed'}")

# Matter (docs/MATTER.md): a pairing code (the specification's construction: chunks and Verhoeff check digit), a QR
# payload, identity bytes or values and identity, firmware, key and log files are refused; the CHIP specification's
# example code and payloads, test values and the zero placeholder are allowed. Assembled at run time, as above.
VD = [[0,1,2,3,4,5,6,7,8,9],[1,2,3,4,0,6,7,8,9,5],[2,3,4,0,1,7,8,9,5,6],[3,4,0,1,2,8,9,5,6,7],[4,0,1,2,3,9,5,6,7,8],
      [5,9,8,7,6,0,4,3,2,1],[6,5,9,8,7,1,0,4,3,2],[7,6,5,9,8,2,1,0,4,3],[8,7,6,5,9,3,2,1,0,4],[9,8,7,6,5,4,3,2,1,0]]
VP = [[0,1,2,3,4,5,6,7,8,9],[1,5,7,6,2,8,3,0,9,4],[5,8,0,3,7,9,6,1,4,2],[8,9,1,6,0,4,3,5,2,7],[9,4,5,3,1,2,6,8,7,0],
      [4,2,8,6,5,7,3,9,0,1],[2,7,9,3,8,0,6,4,1,5],[7,0,4,6,9,1,3,2,5,8]]
def pairing_code(pin, disc):   # an independent implementation of the manual code (11 digits) the specification describes
    s = disc >> 8
    ten = f'{s >> 2}{((s & 3) << 14) | (pin & 0x3FFF):05d}{pin >> 14:04d}'
    c = 0
    for i, ch in enumerate(reversed(ten)): c = VD[c][VP[(i + 1) % 8][int(ch)]]
    return ten + str([0, 4, 3, 2, 1, 5, 6, 7, 8, 9][c])
def privacy(*args, cwd=None):
    return subprocess.run([sys.executable, str(ROOT / 'tools/check_privacy.py'), *args], capture_output=True, text=True, cwd=cwd)
check(pairing_code(20202021, 3840) == '34970112332', 'the CHIP example code follows from the construction')
code, q = pairing_code(13572468, 0xF00), '"'
placeholder = (ROOT / 'idf/components/orcon_matter/placeholder/orcon_matter_creds.h').read_text()
with tempfile.TemporaryDirectory() as d:
    for name, (text, want) in {
        'code.txt': (f'code {code}\n', 'Matter pairing code'),
        'code-grouped.txt': (f'code {code[:4]}-{code[4:7]}-{code[7:]}\n', 'Matter pairing code'),
        'code-chip-example.txt': ('code 34970112332 3497-011-2332\n', None),
        'code-test-values.txt': (f'12345678901 99999999999 {code[:-1]}{(int(code[-1]) + 1) % 10}\n', None),
        'qr.txt': ('qr MT:' + 'ABCDEFGHIJ' + 'KLMNOPQRS\n', 'Matter QR payload'),
        'qr-chip-examples.txt': ('qr MT:Y.K9042C00KA0648G00 MT:-24J0AFN00KA0648G00\n', None),
        'salt.h': ('static const uint8_t k' + 'Salt[16] = {0x00, 0x' + '3a};\n', 'Matter identity bytes'),
        'dac.h': ('static const uint8_t k' + 'DacPrivate[32] = {\n    0, 0, ' + '17,\n};\n', 'Matter identity bytes'),
        'passcode.h': ('constexpr uint32_t k' + 'Passcode = ' + '13572468;\n', 'Matter identity value'),
        'manual.h': ('static const char k' + 'ManualCode[] = ' + q + '1' + q + ';\n', 'Matter identity value'),
        'placeholder-copy.h': (placeholder, None),
        'orcon_matter_creds.h': (placeholder, 'a Matter identity header'),
        'identity.defaults': ('CONFIG_X=y\n', 'never committed'),
        'KOPPELCODE.txt': ('-\n', 'never committed'),
        'image.bin': ('-\n', 'never committed'),
        'night.log': ('-\n', 'raw logs stay local'),
    }.items():
        p = Path(d) / name; p.write_text(text)
        out = privacy(str(p)).stdout
        check((want in out) if want else 'PRIVACY:' not in out, f"{name}: {'refused' if want else 'allowed'}")

# --history: every commit counts, a secret that a later commit removes again included; a clean range passes.
with tempfile.TemporaryDirectory() as d:
    def g(*a):
        return subprocess.run(['git', '-c', 'user.name=test', '-c', 'user.email=test@example.invalid', '-c', 'commit.gpgsign=false',
                               *a], cwd=d, capture_output=True, text=True, check=True).stdout.strip()
    f = Path(d) / 'a.txt'
    g('init', '-q'); f.write_text('clean\n'); g('add', '.'); g('commit', '-qm', 'base'); base = g('rev-parse', 'HEAD')
    f.write_text('clean\nqr MT:' + 'ABCDEFGHIJ' + 'KLMNOPQRS\n'); g('commit', '-qam', 'add'); f.write_text('clean\n'); g('commit', '-qam', 'remove')
    r = privacy('--history', base, cwd=d)
    check(r.returncode != 0 and 'Matter QR payload' in r.stdout, 'history: a secret removed again is still refused')
    check(privacy(str(f), cwd=d).returncode == 0, 'history: the final file alone looks clean')
    tip = g('rev-parse', 'HEAD')
    g('commit', '-q', '--allow-empty', '-m', f'pairing code {code}')
    check('message:1: Matter pairing code' in privacy('--history', tip, cwd=d).stdout, 'history: a commit message is checked')
    tip = g('rev-parse', 'HEAD')
    (Path(d) / 'identity.defaults').write_text('CONFIG_X=y\n'); g('add', '.'); g('commit', '-qm', 'file')
    check('identity.defaults: never committed' in privacy('--history', tip, cwd=d).stdout, 'history: an added file name is checked')
    tip = g('rev-parse', 'HEAD'); f.write_text('clean\nsome more\n'); g('commit', '-qam', 'clean')
    check(privacy('--history', f'{tip}..HEAD', cwd=d).returncode == 0, 'history: a clean range passes')

if subprocess.run(['openssl', 'version'], capture_output=True).returncode:
    print(f'check privacy: {ok} checks, {fail} failures (private keys NOT TESTED: no openssl)'); sys.exit(1 if fail else 0)

with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    pem = openssl('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256')
    lines = pem.strip().splitlines()
    body = ''.join(lines[1:-1])
    enc8 = openssl('pkcs8', '-topk8', '-v2', 'aes-256-cbc', '-passout', 'pass:testzin123', input=pem)
    trad = openssl('ec', '-aes256', '-passout', 'pass:testzin123', input=pem)   # Proc-Type/DEK-Info headers
    pub = openssl('pkey', '-pubout', input=pem)
    # The shortest PKCS#8 keys (Ed25519/X25519: exactly 64 base64 characters), with a '+' in the payload.
    short = {}
    for alg in ('ED25519', 'X25519'):
        for _ in range(200):
            k = openssl('genpkey', '-algorithm', alg)
            if '+' in ''.join(k.strip().splitlines()[1:-1]): short[alg] = k; break
    assert len(short) == 2, 'no short key with a + in 200 tries'
    cases = {
        'plain.pem': (pem, True),
        'compact.pem': (f'{lines[0]}\n{body}\n{lines[-1]}\n', True),
        'cstring.h': ('static const char K[] =\n' + ''.join(f'    "{l}\\n"\n' for l in lines) + ';\n', True),
        'escaped.js': (f'const K = {json.dumps(pem)};\n', True),
        'concat.js': ('const K = ' + ' +\n  '.join(json.dumps(l + '\n') for l in lines) + ';\n', True),
        'pkcs8enc.pem': (enc8, True),
        'traditional.pem': (trad, True),
        **{f'{alg.lower()}{form}': (text, True) for alg, k in short.items() for form, text in (
            ('.pem', k),
            ('.h', 'static const char K[] =\n' + ''.join(f'    "{l}\\n"\n' for l in k.strip().splitlines()) + ';\n'),
            ('.js', 'const K = ' + ' +\n  '.join(json.dumps(l + '\n') for l in k.strip().splitlines()) + ';\n'))},
        'markers.cpp': ('static const char BEGIN[] = "-----BEGIN ENCRYPTED PRIVATE KEY-----", END[] = "-----END ENCRYPTED PRIVATE KEY-----";\n', False),
        'fake.js': ("const PEM = '-----BEGIN ENCRYPTED PRIVATE KEY-----\\nAAAA\\n-----END ENCRYPTED PRIVATE KEY-----\\n';\n", False),
        'public.pem': (pub, False),
        'code.cpp': ('CHECK(strstr(pem, "-----BEGIN ENCRYPTED PRIVATE KEY-----\\n") && strstr(pem, "-----END ENCRYPTED PRIVATE KEY-----\\n"));\n'
                     'const size_t from = body.find("-----BEGIN ENCRYPTED PRIVATE KEY-----\\n") + 38, to = body.find("-----END");\n', False),
    }
    for name, (text, want) in cases.items():
        p = d / name; p.write_text(text)
        check(flagged(p) == want, f"{name}: {'refused' if want else 'allowed'}")
print(f'check privacy: {ok} checks, {fail} failures'); sys.exit(1 if fail else 0)
