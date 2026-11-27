#!/usr/bin/env python3
"""test_release_ci.py — the official release pipeline (tools/release_ci.py) against good and tampered inputs.

Throwaway ECDSA keys only, made per run in a temporary directory: never the production signing key. Synthetic images
carry the markers the device policy looks for; a real release build is used too when RELEASE_BUILD_DIR points at one
(the workflow sets it). Needs g++ and the pinned ArduinoJson (.pio/libdeps/*/ArduinoJson) for the device-policy harness,
and a PlatformIO build for the real bootloader/partition files; whatever is missing is reported as NOT TESTED.
"""
import hashlib, io, json, os, re, shutil, struct, subprocess, sys, tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'tools'))
import release_ci as rc  # noqa: E402

def not_tested(msg):
    """Locally a missing tool is NOT TESTED (reported, exit 0); in the release workflow (RELEASE_BUILD_DIR set) it fails."""
    print('NOT TESTED: ' + msg)
    sys.exit(1 if os.environ.get('RELEASE_BUILD_DIR') else 0)

n = 0
def ok(msg):
    global n; n += 1; print('  [ ok ] ' + msg)
def refused(fn, *a, want=''):
    try: fn(*a)
    except rc.Refused as e:
        assert want in str(e), (want, str(e)); return str(e)
    raise AssertionError(f'not refused: {fn.__name__}{a!r:.120}')

COMMIT = 'c0ffee' + '1' * 34
VERSION = 1002003
TMP = Path(tempfile.mkdtemp(prefix='orcon-release-test-'))
try:
    # ---- tools ------------------------------------------------------------------------------------------------------
    aj = next(iter(sorted(REPO.glob('.pio/libdeps/*/ArduinoJson/src'))), None)
    if not aj or not shutil.which('g++'): not_tested('g++ or the pinned ArduinoJson (.pio/libdeps) missing')
    harness = TMP / 'release_accept'
    subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'src'), '-I', str(aj), str(REPO / 'test/release_accept.cpp'),
                    str(REPO / 'src/fw_update.cpp'), str(REPO / 'src/ota_policy.cpp'), '-o', str(harness)], check=True)
    real = next((d for d in [Path(os.environ.get('RELEASE_BUILD_DIR', '/nonexistent')), REPO / '.pio/build/heltec_v2_sx1276_control']
                 if (d / 'bootloader.bin').is_file() and (d / 'firmware.elf').is_file()), None)
    if not real: not_tested('no PlatformIO build of heltec_v2_sx1276_control (bootloader/partitions/ELF) for the build checks')
    nm = next(iter(sorted(Path.home().glob('.platformio/packages/toolchain-xtensa-esp32*/bin/xtensa-esp32-elf-nm'))), None)
    if not nm: not_tested('xtensa-esp32-elf-nm missing (PlatformIO toolchain)')

    def keypair(name):
        k = TMP / f'{name}.pem'
        subprocess.run(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', str(k)], check=True, capture_output=True)
        pub = subprocess.run(['openssl', 'ec', '-in', str(k), '-pubout'], capture_output=True, text=True, check=True).stdout
        return k.read_text(), pub
    KEY, PUB = keypair('throwaway')
    esptool = next(iter(sorted(Path.home().glob('.platformio/packages/tool-esptoolpy*/esptool.py'))), None)
    pys = [sys.executable] + [str(p) for p in Path.home().glob('Library/Application Support/pipx/venvs/platformio/bin/python')] + [str(p) for p in Path.home().glob('.platformio/penv/bin/python')]
    py = next((x for x in pys if subprocess.run([x, '-c', 'import serial'], capture_output=True).returncode == 0), None)
    OTHER, _ = keypair('other')
    rc.firmware_pubkey = lambda: PUB        # in this test process only: the throwaway key plays the firmware's key

    # ---- versions: SemVer <-> release number, same as the device -----------------------------------------------------
    for t, want in [('1.0.0', 1000000), ('v1.2.3', 1002003), ('0.0.1', 1), ('999.999.999', 999999999), ('2.0.0', 2000000)]:
        assert rc.semver_number(t) == want and subprocess.run([harness, 'semver', t], capture_output=True, text=True).stdout.strip() == str(want), t
    for bad in ['', 'v', '1.2', '192.0.2.4', '01.2.3', '1.02.3', '1000.0.0', '1.2.3-rc1', '1.2.3+x', ' 1.2.3', 'V1.2.3', '0.0.0', '1.2.x', 'v1.2.3\n']:
        refused(rc.semver_number, bad, want='')
        assert subprocess.run([harness, 'semver', bad], capture_output=True, text=True).stdout.strip() == 'invalid', bad
    assert rc.semver_text(1002003) == '1.2.3'
    ok('versions: valid and invalid SemVer judged identically by the pipeline and the device (fw_update.cpp)')
    assert rc.check_version('1.2.3', []) == VERSION and rc.check_version('1.2.3', ['v1.2.2', 'v1.1.9', 'notes', '']) == VERSION
    refused(rc.check_version, '1.2.3', ['v1.2.3'], want='already exists')
    refused(rc.check_version, 'v1.2.3', ['v1.0.0', 'v1.2.3'], want='already exists')
    refused(rc.check_version, '1.2.3', ['1.2.3'], want='already exists')
    refused(rc.check_version, '1.2.3', ['v1.3.0'], want='not newer')
    refused(rc.check_version, '1.2.3', ['v2.0.0', 'v1.0.0'], want='not newer')
    refused(rc.check_version, '1.2.3', ['1.3.0'], want='not newer')                     # a bare tag counts too (Codex r3)
    assert rc.check_version('1.2.3', ['v0.0.0', '0.0.0']) == VERSION                    # the reserved 0.0.0 never blocks
    for bad in ('1.2.1\u0662', '\u0661.2.3', 'v1.\u0662.3'): refused(rc.semver_number, bad, want='invalid')   # non-ASCII digits, as the device
    assert rc.check_version('1.2.3', ['v1.\u0663.0']) == VERSION                        # nor do they count as a published version
    ok('versions: an existing tag or release, or one not newer than every published version, is refused')

    # ---- the next version: one patch/minor/major step above the newest tag or release ----------------------------------
    nv = rc.next_version
    assert nv('patch', 1000000) == 1000001 and nv('patch', 1000001) == 1000002 and nv('patch', 1000002) == 1000003
    assert nv('minor', 1000005) == 1001000 and nv('patch', 1001000) == 1001001
    assert nv('major', 1004008) == 2000000 and nv('patch', 2000000) == 2000001 and nv('minor', 2000001) == 2001000
    ok('next version: patch 1.0.0 -> 1.0.1 -> 1.0.2; minor 1.0.5 -> 1.1.0; major 1.4.8 -> 2.0.0 (SemVer); patches count on after each step')
    for kind, newest, why in [('patch', 0, 'no earlier release'), ('patch', 1000999, 'range'), ('minor', 1999005, 'range'), ('major', 999000001, 'range'),
                              ('patch', -1, 'not a release number'), ('patch', 1000000000, 'not a release number'), ('patch', '1000000', 'not a release number'),
                              ('PATCH', 1000000, 'release type'), ('1.0.1', 1000000, 'release type'), ('', 1000000, 'release type'), ('build', 1000000, 'release type')]:
        refused(nv, kind, newest, want=why)
    ok('next version: no history, a step outside 0..999, a bad number or another release type is refused')
    # The history job's newest version and the publish floor are one awk program over the same kind of list.
    wf = (REPO / '.github/workflows/release-control.yml').read_text()
    progs = re.findall(r"(newest|floor)=\"\$\(awk '(.*)' \"\$RUNNER_TEMP/tags.txt\"\)\"", wf)
    assert [k for k, _ in progs] == ['newest', 'floor'] and progs[0][1] == progs[1][1], progs
    def newest(names):
        f = TMP / 'tags-list.txt'; f.write_bytes(('\n'.join(names) + '\n').encode())
        return int(subprocess.run(['awk', progs[0][1], str(f)], capture_output=True, text=True, check=True).stdout)
    junk = ['rx-only', 'v1.0', 'v1.0.9-rc1', 'v01.0.4', 'v0.0.0', '', 'v1.٣.0', 'refs/tags/v9.9.9']
    assert newest(['v1.0.0', 'v1.0.3', '1.0.2'] + junk) == 1000003 and newest(['v1.0.0', 'v1.0.0', '1.0.0']) == 1000000 and newest(junk) == 0
    assert nv('patch', newest(['v1.0.0', 'v1.0.1'])) == 1000002 and nv('minor', newest(['v1.0.0', 'v1.1.1'])) == 1002000   # a draft or an orphan tag keeps its number
    ok('history: one awk rule for the newest version (history job) and the floor (publish); drafts, orphan tags, duplicates count; other names do not')
    odd = ['maintenance v8.0.0', 'v1.0.1 ', 'v1.0.2 ', 'v1.0.3\r', ' v1.0.4', 'v1.0.5\t', 'v1 .0.6', 'v1.0.7\x85']
    assert newest(['v1.0.0'] + odd) == 1000000 and all(rc.version_of(t) == 0 for t in odd)
    ok('history: odd names (a Unicode line separator, a no-break or extra space, CR, tab) are no version, in the awk rule and in Python alike')
    chosen = nv('patch', newest(['v1.0.0']))
    assert all(newest(['v1.0.0', t]) >= chosen for t in ('v1.0.1', '1.0.1', 'v1.1.1', 'v2.0.1')) and newest(['v1.0.0']) < chosen
    ok('publish: the floor over every tag and release (drafts included) refuses a number that a draft, a tag or a newer release took meanwhile')
    def awk_of(var):   # the workflow's own programs for the beta channel
        m = re.search(var + r"=\"\$\(awk (?:-v n=\"\$NUMBER\" )?'(.*?)' \"\$RUNNER_TEMP/tags.txt\"\)\"", wf); assert m, var; return m.group(1)
    def run_awk(prog, names, *v):
        f = TMP / 'tags-beta.txt'; f.write_bytes(('\n'.join(names) + '\n').encode())
        return subprocess.run(['awk', *v, prog, str(f)], capture_output=True, text=True, check=True).stdout
    tags = ['v1.0.1', 'v1.1.0-beta.1', '1.1.0-beta.3', 'v1.1.0-beta.03', 'v1.1.0-rc.1', 'v1.2.0-beta.2', 'junk', 'channel-beta', 'v1.0.0', 'v1.1.0-beta.100']
    assert run_awk(awk_of('betas'), tags).split() == ['v1.1.0-beta.1', 'v1.1.0-beta.3', 'v1.2.0-beta.2']
    assert int(run_awk(awk_of('pfloor'), tags, '-v', 'n=1001000')) == 3 and int(run_awk(awk_of('pfloor'), tags, '-v', 'n=1003000')) == 0
    parsed = [t for t in (rc.parse_release_tag(x if x.startswith('v') else 'v' + x) for x in tags) if t]
    assert int(run_awk(awk_of('top'), tags)) == max(rc.release_rank(*t) for t in parsed) == rc.release_rank(1002000, 2)
    assert rc.next_release('minor', int(run_awk(progs[0][1], tags)), 'beta', run_awk(awk_of('betas'), tags).split()) == (1001000, 4)
    assert rc.next_release('minor', int(run_awk(progs[0][1], ['v1.0.0', 'v1.0.1'])), 'beta') == (1001000, 1)   # v1.0.1 -> v1.1.0-beta.1
    ok('beta channel: the workflow\'s awk rules (beta tags, a number\'s highest beta, the highest rank of every tag) equal the Python and device rules')
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err): code = rc.main(['next', '--newest', '1000000', '--notes=Kort', '--', 'patch'])   # the workflow's form
    assert code == 0 and out.getvalue() == 'number=1000001\npre=0\ntag=v1.0.1\nname=1.0.1\nrank=100000200\nis_beta=false\nlatest=true\n' \
        and 'v1.0.0' in err.getvalue() and 'v1.0.1' in err.getvalue()
    for argv in (['--newest', '1000000', '--notes=' + 'x' * 401, '--', 'patch'], ['--newest', '1000000', '--', 'custom'], ['--newest', '1000000', '--', '--help'],
                 ['--newest', '1000000', '--', '-h'], ['--newest', '1000000', '--', ''], ['--newest', '', '--', 'patch'], ['--newest', '0', '--', 'patch'],
                 ['--newest', '01000000', '--', 'patch'], ['--newest', ' 1000000', '--', 'patch'], ['--newest', '1000000000', '--', 'patch'], ['--newest', '-1', '--', 'patch']):
        o = io.StringIO()
        with redirect_stdout(o), redirect_stderr(io.StringIO()): assert rc.main(['next'] + argv) == 1 and o.getvalue() == '', argv
    ok('next (CLI): number, tag and name of one version for $GITHUB_OUTPUT, the step in the log; a bad number, notes or type (even "--help") is refused, nothing written')
    # ---- betas: X.Y.Z-beta.N, the same rule as the device (fw_update.cpp parseSemverPre), each number used once --------
    pt = rc.parse_release_tag
    assert pt('v1.2.3') == (VERSION, 0) and pt('v1.2.3-beta.1') == (VERSION, 1) and pt('v1.2.3-beta.99') == (VERSION, 99)
    for bad in ('v1.2.3-beta.0', 'v1.2.3-beta.100', 'v1.2.3-beta.01', 'v1.2.3-rc.1', 'v1.2.3-beta', 'v0.0.0', '1.2.3-beta.1 ', 'v1.2.3-beta.\u0661'):
        assert pt(bad) is None, bad
    assert rc.release_tag(VERSION, 4) == 'v1.2.3-beta.4' and rc.release_rank(VERSION, 0) > rc.release_rank(VERSION, 99) > rc.release_rank(VERSION, 1)
    nr = rc.next_release
    assert nr('patch', 1000000) == (1000001, 0) and nr('patch', 1000000, 'beta') == (1000001, 1)
    assert nr('patch', 1000000, 'beta', ['v1.0.1-beta.1', 'v1.0.1-beta.3', 'v1.0.2-beta.9', 'junk', 'v1.0.1']) == (1000001, 4)
    refused(nr, 'patch', 1000000, 'beta', [f'v1.0.1-beta.{i}' for i in range(1, 100)], want='99 betas')
    refused(nr, 'patch', 1000000, 'rc', want='stable or beta')
    out = io.StringIO()
    with redirect_stdout(out), redirect_stderr(io.StringIO()):
        code = rc.main(['next', '--newest', '1000000', '--channel', 'beta', '--betas', 'v1.0.1-beta.2', '--', 'patch'])
    assert code == 0 and out.getvalue() == 'number=1000001\npre=3\ntag=v1.0.1-beta.3\nname=1.0.1-beta.3\nrank=100000103\nis_beta=true\nlatest=false\n', out.getvalue()
    assert rc.build_flags(VERSION, COMMIT, 4).endswith(' -DORCON_RELEASE_PRE=4') and rc.stamp(VERSION, 4) == '1002003-beta.4'
    ok('betas: tags, ranks (a stable above its betas), the next beta of a number once, never past 99; build flag and stamp')

    # ---- release source ----------------------------------------------------------------------------------------------
    good = dict(repo='MarcoHuib/orcon-15rf-controller', ref='refs/heads/main', event='workflow_dispatch', default_branch='main', sha=COMMIT, head=COMMIT)
    rc.check_source(**good)
    for k, v, why in [('event', 'push', 'workflow_dispatch'), ('event', 'pull_request_target', 'workflow_dispatch'), ('event', 'release', 'workflow_dispatch'),
                      ('ref', 'refs/heads/feature/x', 'default branch'), ('ref', 'refs/tags/v1.2.3', 'default branch'), ('ref', 'refs/pull/1/merge', 'default branch'),
                      ('repo', 'someone/orcon-15rf-controller', 'repository'), ('repo', 'me-processware/orcon-15rf-controller', 'repository'),
                      ('sha', 'abc', 'full SHA'), ('head', '2' * 40, 'no longer the head'), ('default_branch', '', 'default branch')]:
        refused(rc.check_source, *{**good, k: v}.values(), want=why)
    ok('source: only a manual run on the default branch of this repository, at its current head (no PR, push, tag, fork or other branch)')
    listing = ('Platform espressif32 @ 6.7.0 (required: espressif32 @ 6.7.0)\n'
               + '\n'.join(f'├── {l.split()[0]} @ {l.split()[1]} (required: x)' for l in (REPO / 'tools/release/platformio-packages.lock').read_text().splitlines()
                           if l and not l.startswith('#') and not l.startswith('espressif32 ')))
    listing = '\n'.join(l for l in listing.split('\n') if 'tool-scons' not in l)
    core = TMP / 'pio-core'; (core / 'packages/tool-scons').mkdir(parents=True)
    scons = lambda v: (core / 'packages/tool-scons/package.json').write_text(json.dumps({'name': 'tool-scons', 'version': v}))
    scons('4.41101.0')
    rc.check_packages(listing, core)
    rc.check_packages(listing + '\n├── tool-openocd-esp32 @ 2.1100.20220706 (required: x)\n├── toolchain-riscv32-esp @ 8.4.0 (required: x)', core)
    rc.check_packages((REPO / 'test/fixtures/pio-pkg-list-clean.txt').read_text(), core)
    refused(rc.check_packages, listing.replace('3.20016.0', '3.20017.0'), core, want='differ from the lock')
    refused(rc.check_packages, listing + '\n├── evil-lib @ 1.0.0 (required: x)', core, want='extra')
    refused(rc.check_packages, listing.replace('toolchain-xtensa-esp32', 'toolchain-xtensa-esp-elf'), core, want='missing')
    scons('4.41102.0'); refused(rc.check_packages, listing, core, want='tool-scons')            # Core's SCons from its range (Codex r3)
    (core / 'packages/tool-scons/package.json').unlink(); refused(rc.check_packages, listing, core, want='missing')
    ok('toolchain: the resolved PlatformIO packages and Core\'s SCons must equal the lock (changed framework/SCons or extra library refused)')
    assert rc.build_flags(VERSION, COMMIT) == f'-DFW_VERSION=\\"{COMMIT}\\" -DORCON_RELEASE_VERSION={VERSION}'
    refused(rc.build_flags, VERSION, 'main', want='SHA')
    ok('build flags: exactly the commit and the release number')

    # ---- the built image -----------------------------------------------------------------------------------------------
    CTL = b'txpoc packet TX (CONTROL: max 10/min, 60/h)'
    ELF_SHA = hashlib.sha256((real / 'firmware.elf').read_bytes()).digest()
    def esp(data, header=b'\xe9\x01\x02\x30', elf_sha=ELF_SHA, segments=None):
        """A structurally valid ESP32 app image (one segment holding data, the ELF's SHA-256 at file offset 0xB0 as
        esptool writes it, checksum, appended SHA-256), so the build checks reach the release rules."""
        seg = bytearray(b'\x00' * 0x90 + elf_sha + data); seg += b'\x00' * (-len(seg) % 4)
        b = bytearray(header + b'\x00' * 4 + b'\xee' + b'\x00' * 14 + b'\x01')
        if segments is not None: b[1] = segments
        b += struct.pack('<II', 0x3F400020, len(seg)) + seg
        cs = 0xEF
        for v in seg: cs ^= v
        b += b'\x00' * (15 - len(b) % 16) + bytes([cs])
        return bytes(b + hashlib.sha256(b).digest())
    def image(*parts, version=VERSION, commit=COMMIT, header=b'\xe9\x01\x02\x30', owner=True, owner_key=True, signed=True, pre=0, **kw):
        body = []
        for p in parts + ((b'ORCON-SIGNED-OTA v1',) if signed else ()) + ((b'ORCON-OWNER-MODEL v1',) if owner else ()) + ((b'ORCON-OWNER-KEY v1',) if owner_key else ()):
            body += [p, b'\x00' * 97]
        if version is not None: body += [f'ORCON-RELEASE {rc.stamp(version, pre)};'.encode(), b'\x00' * 10]
        if commit: body += [commit.encode(), b'\x00' * 10]
        return esp(b''.join(body) + bytes(range(256)) * 20, header, **kw)
    def build_dir(name, fw, matter=False, **swap):
        d = TMP / 'build' / name; d.mkdir(parents=True)
        (d / ('orcon_15rf.bin' if matter else 'firmware.bin')).write_bytes(fw)
        shutil.copy(real / 'firmware.elf', d / ('orcon_15rf.elf' if matter else 'firmware.elf'))
        if not matter:
            for f in ('bootloader.bin', 'partitions.bin'): shutil.copy(real / f, d / f)
        for f, data in swap.items(): (d / f).write_bytes(data)
        return d
    def vb(name, fw, version=VERSION, commit=COMMIT, variant='control', pre=0, out=None, **swap):
        m = variant == 'control-matter'
        return rc.verify_build(build_dir(name, fw, m, **swap), version, commit, harness, nm, out or TMP / 'dist' / name,
                               variant=variant, pre=pre, boot_files=real if m else None)
    info = vb('good', image(CTL))
    assert info['version'] == VERSION and info['name'] == '1.2.3' and info['env'] == 'heltec_v2_sx1276_control' and info['commit'] == COMMIT
    assert {f.name for f in (TMP / 'dist/good').iterdir()} == {'firmware.bin', 'bootloader.bin', 'partitions.bin', 'boot_app0.bin', 'BUILD_INFO.json'}
    ok('build: a control image with the release stamp, commit and pinned boot files is accepted (device policy: control)')
    cases = [
        ('no-esp-image', image(CTL, header=b'\x00\x01\x02\x30'), {}, 'not an ESP32'),
        ('wrong-flash-header', image(CTL, header=b'\xe9\x01\x00\x20'), {}, 'flash header'),
        ('too-large', image(CTL, b'\x01' * rc.SLOT), {}, 'does not fit'),
        ('255-segments', image(CTL, segments=255), {}, 'segments'),
        ('bad-checksum', (lambda b: b[:200] + bytes([b[200] ^ 1]) + b[201:])(image(CTL)), {}, 'mismatch'),
        ('trailing-bytes', image(CTL) + b'\x00', {}, 'trailing'),
        ('other-elf', image(CTL, elf_sha=b'\x11' * 32), {}, 'checked firmware.elf'),
        ('dev-build', image(CTL, version=0), {}, 'release stamp'),
        ('unstamped', image(CTL, version=None), {}, 'release stamp'),
        ('other-version', image(CTL, version=1002004), {}, 'release stamp'),
        ('two-stamps', image(CTL, b'ORCON-RELEASE 7;'), {}, 'release stamp'),
        ('wrong-commit', image(CTL, commit='d' * 40), {}, 'commit is not stamped'),
        ('rx-only', image(b'SX1276 direct RX (TX disabled)', b'RX-ONLY TEST FIRMWARE: receive only'), {}, 'image-not-control'),
        ('rf-test', image(b'SX1276 direct RX + txpoc packet TX (RF TEST: max 50 per boot)', b'txpoc packet TX'), {}, 'image-not-control'),
        ('dry-run', image(CTL, b'tx-poc-dryrun'), {}, 'image-not-control'),
        ('control-plus-rx', image(CTL, b'SX1276 direct RX (TX disabled)'), {}, 'image-not-control'),
        ('legacy-ota', image(CTL, b'ArduinoOTA'), {}, 'image-has-unauthenticated-ota'),
        ('no-owner-model', image(CTL, owner=False), {}, 'image-predates-owner-model'),
        ('no-owner-key', image(CTL, owner_key=False), {}, 'image-without-owner-key'),
        ('no-signed-ota', image(CTL, signed=False), {}, 'image-without-signed-ota'),
        ('bootloader', image(CTL), {'bootloader.bin': (real / 'bootloader.bin').read_bytes()[:-1] + b'\x00'}, 'bootloader.bin differs'),
        ('partitions', image(CTL), {'partitions.bin': (real / 'partitions.bin').read_bytes()[:-1] + b'\x00'}, 'partitions.bin differs'),
    ]
    for name, fw, swap, why in cases:
        refused(lambda: vb(name, fw, **swap), want=why)
    refused(lambda: vb('version-arg', image(CTL), version=1002004), want='release stamp')
    if esptool and py and (real / 'firmware.bin').is_file():
        rc.check_elf_image(real, esptool, py)
        refused(lambda: rc.check_elf_image(build_dir('not-from-elf', image(CTL)), esptool, py), want='not the image of the checked firmware.elf')
        ok('build: firmware.bin must equal the pinned esptool\'s image of the checked ELF; a self-consistent image with the ELF hash is refused (Codex r4)')
    elif os.environ.get('RELEASE_BUILD_DIR'): raise AssertionError('elf2image check: esptool or pyserial missing in the release workflow')
    else: print('  NOT TESTED: ELF-to-image regeneration (no esptool/pyserial)')
    ok(f'build: {len(cases) + 1} wrong images refused (no ESP image, flash layout, size, version stamp, commit, RX-only/RF-test/dry-run, legacy OTA, boot files)')
    # ---- the Matter variant and betas ------------------------------------------------------------------------------------
    MTR = b'ORCON-MATTER-FIRMWARE v1'
    mi = vb('matter', image(CTL, MTR), variant='control-matter')
    assert mi['env'] == 'heltec_v2_matter' and mi['variant'] == 'control-matter' and mi['pre'] == 0
    for name, fw, kw, why in [('matter-without-marker', image(CTL), {}, 'Matter marker'),
                              ('control-with-matter', image(CTL, MTR), {'variant': 'control'}, 'Matter marker'),
                              ('matter-with-identity', image(CTL, MTR, b'ORCON-MATTER-EMBEDDED-IDENTITY v1'), {}, 'holds a Matter identity'),
                              ('matter-as-rx', image(MTR, b'SX1276 direct RX (TX disabled)', b'RX-ONLY TEST FIRMWARE: receive only'), {}, 'image-not-control-matter')]:
        refused(lambda: vb(name, fw, **{'variant': 'control-matter', **kw}), want=why)
    refused(lambda: rc.verify_build(build_dir('matter-no-boot', image(CTL, MTR), True), VERSION, COMMIT, harness, nm, TMP / 'dist/x',
                                    variant='control-matter'), want='--boot-files')
    bi = vb('beta', image(CTL, pre=2), pre=2)
    assert bi['pre'] == 2 and bi['name'] == '1.2.3-beta.2'
    refused(lambda: vb('beta-as-stable', image(CTL, pre=2)), want='release stamp')
    refused(lambda: vb('stable-as-beta', image(CTL), pre=2), want='release stamp')
    ok('build: the Matter variant needs its marker and never holds an identity; a beta image carries its own stamp, never passes as stable (or back)')

    # ---- manifest, signature, package ---------------------------------------------------------------------------------------
    D = TMP / 'dist/good'
    text = rc.make_manifest(D); m = json.loads(text)
    assert m == {'v': 2, 'kind': 'orcon-release', 'env': 'heltec_v2_sx1276_control', 'variant': 'control', 'allow_tx': True, 'commit': COMMIT,
                 'version': VERSION, 'size': info['firmware']['size'], 'sha256': info['firmware']['sha256']}
    ok('manifest: v2 release, control, exact size/hash/commit/version of the verified image')
    refused(rc.sign, D, '', want='signing key missing')
    refused(rc.sign, D, 'not a key', want='signing key missing')
    refused(rc.sign, D, OTHER, want='does not match src/ota_pubkey.h')
    ok('sign: a missing secret, garbage or another key than the firmware trusts is refused (nothing signed)')
    before = set(Path(tempfile.gettempdir()).glob('orcon-sign-*'))
    sig = rc.sign(D, KEY)
    assert set(Path(tempfile.gettempdir()).glob('orcon-sign-*')) == before, 'temporary key directory left behind'
    assert rc.verify_signature(text, sig) and not rc.verify_signature(text.replace('"size"', '"size" '), sig)
    ok('sign: valid signature; the temporary key file is removed')
    out = io.StringIO()
    with redirect_stdout(out), redirect_stderr(out):
        os.environ['ORCON_OTA_SIGNING_KEY'] = KEY
        code = rc.main(['sign', '--dist', str(D)])
    assert code == 0 and 'ORCON_OTA_SIGNING_KEY' not in os.environ, 'the secret must leave the environment after use'
    keyb64 = [l for l in KEY.splitlines() if l and 'PRIVATE' not in l]
    assert not any(l in out.getvalue() for l in keyb64) and 'PRIVATE' not in out.getvalue()
    ok('sign (CLI): the secret is taken out of the environment and never printed')
    refused(rc.package, D, 'regel\x01twee', want='control characters')
    refused(rc.package, D, 'x' * 401, want='400 bytes')
    bundle = rc.package(D, 'Betere WiFi-herverbinding.')
    assert bundle.name == 'orcon-control-v1.2.3-c0ffee1.orconfw'
    assert rc.verify_package(D, harness) == bundle and rc.verify_package(D, harness, notes='Betere   WiFi-herverbinding.') == bundle
    refused(rc.verify_package, D, harness, False, 'Andere notes', want='notes differ')
    refused(rc.verify_package, D, harness, True, want='factory image')                   # official: the factory image is required
    assert rc.verify_package(D, harness, release='v1.2.3') == bundle
    for other in ('v1.2.4', '1.2.3', 'v1.2.3 ', 'v1.02.3', 'v1.2.2'): refused(rc.verify_package, D, harness, False, None, other, want='not the release')
    for name, extra in (('two-stamps', b'ORCON-RELEASE 1002002;'), ('no-commit', None)):   # a build changed after its check, with a fresh record
        dd = TMP / 'tamper' / name; shutil.copytree(D, dd)
        fw = (dd / 'firmware.bin').read_bytes()
        fw = fw + extra if extra else fw.replace(COMMIT.encode(), b'0' * 40)
        (dd / 'firmware.bin').write_bytes(fw)
        i = json.loads((dd / 'BUILD_INFO.json').read_text()); i['firmware'] = {'size': len(fw), 'sha256': rc.sha256(fw)}
        (dd / 'BUILD_INFO.json').write_text(json.dumps(i)); rc.write_sums(dd)
        refused(rc.verify_package, dd, harness, False, None, 'v1.2.3', want='release stamp or commit stamp')
    ok('verify-package: the package carries exactly the version prepare chose (tag = build = manifest = bundle = update index)')
    ok('package: bundle + update index + SHA256SUMS verify (signature; device policy; a board on 1.2.3 sees "up to date"; a newer one refuses)')
    idx = json.loads((D / 'orcon-update.json').read_text())
    assert idx['notes'] == 'Betere WiFi-herverbinding.' and idx['manifest'] == text and len((D / 'orcon-update.json').read_bytes()) <= 2048
    ok('index: exactly the signed manifest and its signature, short notes, within the device limit')

    def tampered(name, mutate, why):
        d = TMP / 'tamper' / name; shutil.copytree(D, d); mutate(d)
        refused(rc.verify_package, d, harness, want=why)
    b = bundle.name
    tampered('image', lambda d: (d / b).write_bytes((d / b).read_bytes()[:-1] + b'\x00'), 'not the verified firmware.bin')
    def swap_manifest(d):
        raw = (d / b).read_bytes(); nl = raw.find(b'\n'); h = json.loads(raw[:nl])
        h['manifest'] = h['manifest'].replace(f'"version":{VERSION}', f'"version":{VERSION + 1}')
        (d / b).write_bytes(json.dumps(h, separators=(',', ':')).encode() + raw[nl:])
    tampered('manifest', swap_manifest, 'does not match the build')
    def other_sig(d):
        raw = (d / b).read_bytes(); nl = raw.find(b'\n'); h = json.loads(raw[:nl])
        k = TMP / 'o.pem'; k.write_text(OTHER)
        h['sig'] = subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(k)], input=h['manifest'].encode(), capture_output=True, check=True).stdout.hex()
        (d / b).write_bytes(json.dumps(h, separators=(',', ':')).encode() + raw[nl:])
    tampered('signature', other_sig, 'signature does not verify')
    tampered('index-notes', lambda d: (d / 'orcon-update.json').write_text((d / 'orcon-update.json').read_text().replace('Betere', 'Slechtere')), 'SHA256SUMS')
    def index_other(d):
        i = json.loads((d / 'orcon-update.json').read_text()); i['manifest'] = i['manifest'].replace('"size"', '"size" ')
        (d / 'orcon-update.json').write_text(json.dumps(i, separators=(',', ':'))); rc.write_sums(d)
    tampered('index-manifest', index_other, 'device update check refuses the index')
    def lie(d, **kw):
        i = json.loads((d / 'BUILD_INFO.json').read_text()); i.update(kw); (d / 'BUILD_INFO.json').write_text(json.dumps(i)); rc.write_sums(d)
    tampered('build-info-version', lambda d: lie(d, version=9009009, name='9.9.9'), 'does not match the build')
    tampered('build-info-name', lambda d: lie(d, name='9.9.9'), 'malformed')
    tampered('build-info-bootloader', lambda d: lie(d, bootloader='0' * 64), 'not the pinned board file')
    tampered('firmware-artifact', lambda d: (d / 'firmware.bin').write_bytes(b'\xe9' + (d / 'firmware.bin').read_bytes()[1:-1] + b'\x01'), 'differs from BUILD_INFO')
    tampered('two-bundles', lambda d: shutil.copy(d / b, d / ('x' + b)), 'exactly one')
    tampered('sums', lambda d: (d / 'SHA256SUMS').write_text('0' * 64 + '  ' + b + '\n'), 'SHA256SUMS')
    def renamed(d): (d / b).rename(d / b.replace('c0ffee1', 'c0ffee2')); rc.write_sums(d)
    tampered('bundle-name', renamed, 'bundle-name')
    def head_variant(d, make):
        raw = (d / b).read_bytes(); nl = raw.find(b'\n'); h = json.loads(raw[:nl])
        (d / b).write_bytes(make(h).encode() + raw[nl:]); rc.write_sums(d)
    tampered('extra-sig-key', lambda d: head_variant(d, lambda h: '{"manifest":%s,"sig":"00","sig":"%s"}' % (json.dumps(h['manifest']), h['sig'])), 'bad-bundle')
    tampered('escaped-key', lambda d: head_variant(d, lambda h: '{"\\u006danifest":%s,"sig":"%s"}' % (json.dumps(h['manifest']), h['sig'])), 'bad-bundle')
    ok('verify-package: tampered image, manifest, signature (other key), index, artifact, extra bundle, checksums or name are refused')
    refused(rc.make_manifest, TMP / 'tamper/firmware-artifact', want='differs from BUILD_INFO')
    ok('manifest: an artifact changed after the build check is refused (signing job input)')

    # ---- a release: both variants, one version; stable and beta --------------------------------------------------------------
    for tag, pre in (('v1.2.3', 0), ('v1.2.3-beta.2', 2)):
        R = TMP / f'release-{pre}'
        if not pre:   # stable: the control firmware only, as before
            vb('rel-stable', image(CTL), out=TMP / 'release-stable/control')
            rc.make_manifest(TMP / 'release-stable'); rc.sign(TMP / 'release-stable', KEY); rc.package(TMP / 'release-stable')
        vb(f'rel{pre}-control', image(CTL, pre=pre), pre=pre, out=R / 'control')
        vb(f'rel{pre}-matter', image(CTL, MTR, pre=pre), variant='control-matter', pre=pre, out=R / 'control-matter')
        if not pre: continue   # both variants as one stable release: refused below
        rc.make_manifest(R); rc.sign(R, KEY); rc.package(R, 'Twee varianten.')
        if esptool and py: rc.factory(R, esptool, py)
        rc.verify_package(R, harness, bool(esptool and py), 'Twee varianten.', tag)
        names = rc.assemble(R, TMP / f'publish-{pre}')
        want = {f'orcon-control-{tag}-c0ffee1.orconfw', f'orcon-control-matter-{tag}-c0ffee1.orconfw', 'orcon-update.json', 'orcon-update-matter.json',
                'BUILD_INFO-control.json', 'BUILD_INFO-control-matter.json'} | ({f'orcon-control-{tag}-c0ffee1-factory-usb.bin',
                f'orcon-control-matter-{tag}-c0ffee1-factory-usb.bin'} if esptool and py else set())
        assert set(names) == want, names
        assert subprocess.run(['sha256sum' if shutil.which('sha256sum') else 'shasum', *([] if shutil.which('sha256sum') else ['-a', '256']), '-c', 'SHA256SUMS'],
                              cwd=TMP / f'publish-{pre}', capture_output=True).returncode == 0
        md = rc.notes_md(R, '', '', TMP / f'publish-{pre}' / 'SHA256SUMS')
        assert f'orcon-control-matter-{tag}-c0ffee1.orconfw' in md and 'niet gecertificeerd' in md and ('Bèta' in md) == bool(pre)
    refused(rc.verify_package, TMP / 'release-2', harness, False, None, 'v1.2.3', want='not the release')
    refused(rc.verify_package, TMP / 'release-stable', harness, False, None, 'v1.2.3-beta.2', want='not the release')
    mixed = TMP / 'release-mixed'; shutil.copytree(TMP / 'release-0/control', mixed / 'control'); shutil.copytree(TMP / 'release-2/control-matter', mixed / 'control-matter')
    refused(rc.verify_package, mixed, harness, want='not one release')
    only = TMP / 'release-only'; shutil.copytree(TMP / 'release-2/control', only / 'control')
    refused(rc.assemble, only, TMP / 'publish-only', want='exactly the variants')          # a beta without its Matter firmware
    assert rc.assemble(TMP / 'release-stable', TMP / 'publish-stable') and not any('matter' in f.name for f in (TMP / 'publish-stable').iterdir())
    refused(rc.verify_package, TMP / 'release-0', harness, want='exactly the variants')    # a stable release never carries Matter (yet)
    ok('release: both variants signed and verified as one version (each index judged by its own boards, the other variant refused), '
       'one SHA256SUMS; betas refused by boards without the channel; mixed or partial releases refused')

    # ---- factory image and notes ----------------------------------------------------------------------------------------------
    if esptool and py:
        f = rc.factory(D, esptool, py)
        assert f.name == 'orcon-control-v1.2.3-c0ffee1-factory-usb.bin' and rc.verify_package(D, harness, official=True)
        ok('factory: one USB image from 0x0 with the verified bootloader, table, boot_app0 and control image; listed in SHA256SUMS')
        for name, mutate in (('corrupt', lambda b: b[:0x10064] + bytes([b[0x10064] ^ 1]) + b[0x10065:]), ('truncated', lambda b: b[:-1]),
                             ('appended', lambda b: b + b'\x00'), ('nvs-not-erased', lambda b: b[:0x9000] + b'\x00' + b[0x9001:])):
            d = TMP / 'factory' / name; shutil.copytree(D, d)
            g = d / f.name; g.write_bytes(mutate(g.read_bytes())); rc.write_sums(d)
            refused(rc.verify_package, d, harness, want='factory image')
        ok('verify-package: a factory image that is not exactly the verified parts is refused, even with fresh checksums (Codex r2)')
    else:
        if os.environ.get('RELEASE_BUILD_DIR'): raise AssertionError('factory image: esptool or pyserial missing in the release workflow')
        print('  NOT TESTED: factory image (no PlatformIO tool-esptoolpy, or no Python with pyserial)')
    md2 = rc.notes_md(D, '<!-- [x](y) *z*', '')
    assert 'Update installeren' in md2 and 'wist alle instellingen' in md2 and '<!--' not in md2 and '&#60;&#33;--' in md2 and '*z*' not in md2
    ok('notes: markup in the owner\'s notes is shown literally, after the instructions; it can hide nothing (Codex r4)')
    md = rc.notes_md(D, 'Betere WiFi-herverbinding.', 'https://github.com/x/y/actions/runs/1')
    assert 'v1.2.3' in md and COMMIT in md and info['firmware']['sha256'] in md and 'Update installeren' in md and 'PRIVATE' not in md
    ok('notes: version, commit, image hash, checksums and the install route; no secrets')

    # ---- the real release build, when the workflow (or a developer) made one ----------------------------------------------------
    if os.environ.get('RELEASE_BUILD_DIR'):
        rv, rcommit, rpre = int(os.environ['RELEASE_VERSION']), os.environ['RELEASE_COMMIT'], int(os.environ.get('RELEASE_PRE', '0'))
        i = rc.verify_build(real, rv, rcommit, harness, nm, TMP / 'dist/real', pre=rpre)
        ok(f"real build: {i['firmware']['size']} B control image v{i['name']} accepted by the build checks")
    else:
        print('  NOT TESTED here: a real release-stamped build (RELEASE_BUILD_DIR unset; the workflow runs it)')
    print(f'release pipeline: {n} checks, 0 failures')
finally:
    shutil.rmtree(TMP, ignore_errors=True)
