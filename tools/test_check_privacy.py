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

if subprocess.run(['openssl', 'version'], capture_output=True).returncode:
    print('check privacy: NOT TESTED (no openssl)'); sys.exit(0)

with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    pem = openssl('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256')
    lines = pem.strip().splitlines()
    body = ''.join(lines[1:-1])
    enc8 = openssl('pkcs8', '-topk8', '-v2', 'aes-256-cbc', '-passout', 'pass:testzin123', input=pem)
    trad = openssl('ec', '-aes256', '-passout', 'pass:testzin123', input=pem)   # Proc-Type/DEK-Info headers
    pub = openssl('pkey', '-pubout', input=pem)
    cases = {
        'plain.pem': (pem, True),
        'compact.pem': (f'{lines[0]}\n{body}\n{lines[-1]}\n', True),
        'cstring.h': ('static const char K[] =\n' + ''.join(f'    "{l}\\n"\n' for l in lines) + ';\n', True),
        'escaped.js': (f'const K = {json.dumps(pem)};\n', True),
        'concat.js': ('const K = ' + ' +\n  '.join(json.dumps(l + '\n') for l in lines) + ';\n', True),
        'pkcs8enc.pem': (enc8, True),
        'traditional.pem': (trad, True),
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
