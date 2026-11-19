#!/usr/bin/env python3
"""release_ci.py — the checks and steps of an official release (.github/workflows/release-control.yml).

Only the Heltec V2 control image (heltec_v2_sx1276_control) is ever published. Every step runs the same way on a
developer machine; tools/test_release_ci.py runs them with throwaway keys and tampered inputs.

  version SEMVER [--tags FILE]           the release number for SEMVER (MAJOR.MINOR.PATCH, each 0..999); refuses an
                                         existing tag/release and anything not newer than every published version
  source --repo R --ref REF --event E --default-branch B --sha SHA --head SHA
                                         only a workflow_dispatch on the default branch of this repository, at its head
  packages --list FILE                   `pio pkg list -e heltec_v2_sx1276_control` must equal tools/release/platformio-packages.lock
  flags --version N --commit SHA         PLATFORMIO_BUILD_FLAGS of the release build
  verify-build --build DIR --version N --commit SHA --harness BIN --nm NM --out DIR
                                         the built image: ESP image header, size, release stamp, commit stamp, control
                                         markers (the device's own policy, test/release_accept.cpp), pinned bootloader,
                                         partition table and boot_app0, the rollback hook; copies the inputs + BUILD_INFO.json
  manifest --dist DIR                    the signed-to-be v2 release manifest (orcon.py release_manifest)
  sign --dist DIR                        signs manifest.txt with the PEM in $ORCON_OTA_SIGNING_KEY (never printed or kept);
                                         refuses an empty secret or a key that is not the one in src/ota_pubkey.h
  package --dist DIR [--notes TEXT]      the .orconfw bundle, orcon-update.json (the device's update index), SHA256SUMS
  verify-package --dist DIR --harness BIN
                                         signature (openssl, src/ota_pubkey.h) and the device's own rules on the package
  factory --dist DIR --esptool PY [--python EXE]
                                         one USB image for a NEW device (erases its settings; never written automatically)
  notes --dist DIR [--notes TEXT] [--run URL]
                                         release notes (markdown) for the GitHub Release
"""
import argparse, hashlib, json, os, re, shutil, stat, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'tools'))
import orcon                                     # noqa: E402  release_manifest, write_bundle (one implementation)
from check_flash_package import PINNED           # noqa: E402  the board's bootloader, partition table, boot_app0

ENV = 'heltec_v2_sx1276_control'
VARIANT = 'control'
SLOT = 0x330000
INDEX = 'orcon-update.json'
FLASH_HDR = bytes.fromhex('0230')                # image header bytes 2-3: DIO, 8 MB / 40 MHz (tools/heltec_v2.sh)


class Refused(Exception):
    pass


def fail(msg):
    raise Refused(msg)


# ---- versions (same rule as src/fw_update.cpp parseSemver) ----------------------------------------------------------
SEMVER = re.compile(r'v?(0|[1-9]\d{0,2})\.(0|[1-9]\d{0,2})\.(0|[1-9]\d{0,2})')


def semver_number(text):
    m = SEMVER.fullmatch(text or '')
    if not m: fail(f'invalid version {text!r}: use MAJOR.MINOR.PATCH, each 0..999, no leading zeros, no suffix')
    a, b, c = map(int, m.groups())
    n = a * 1000000 + b * 1000 + c
    if not n: fail('version 0.0.0 is reserved for development builds')
    return n


def semver_text(n):
    return f'{n // 1000000}.{n // 1000 % 1000}.{n % 1000}'


def check_version(text, existing):
    """The number for text; refused when its tag already exists or a published version is not older."""
    n = semver_number(text)
    tag = 'v' + semver_text(n)
    seen = []
    for t in existing:
        t = t.strip()
        if not t: continue
        if t == tag or t == semver_text(n): fail(f'{tag} already exists (tag or release): a version is published once; see docs/RELEASES.md "herstel"')
        m = SEMVER.fullmatch(t)
        if m and t.startswith('v'): seen.append(semver_number(t))
    if seen and n <= max(seen):
        fail(f'{tag} is not newer than the newest published version v{semver_text(max(seen))}: boards refuse older releases')
    return n


# ---- release source ------------------------------------------------------------------------------------------------------
def repo_from_source():
    m = re.search(r'#define ORCON_RELEASE_REPO "([^"]+)"', (REPO / 'src/fw_update.h').read_text())
    return m.group(1) if m else ''


def check_source(repo, ref, event, default_branch, sha, head):
    if event != 'workflow_dispatch': fail(f'event {event!r}: an official release starts only by hand (workflow_dispatch)')
    if repo != repo_from_source(): fail(f'repository {repo!r} is not the one the firmware checks ({repo_from_source()!r})')
    if not default_branch or ref != f'refs/heads/{default_branch}': fail(f'ref {ref!r}: only the default branch {default_branch!r} is released')
    if not re.fullmatch(r'[0-9a-f]{40}', sha or ''): fail('commit is not a full SHA-1')
    if sha != head: fail(f'{sha[:7]} is no longer the head of {default_branch} ({(head or "?")[:7]}): start the release again')


def package_versions(text):
    rows = set()
    for line in text.splitlines():
        m = re.search(r'(?:Platform |── )([\w.-]+) @ ([\w.+-]+) \(required:', line)
        if m: rows.add(f'{m.group(1)} {m.group(2)}')
    return rows


def check_packages(listing):
    lock = {l.strip() for l in (REPO / 'tools/release/platformio-packages.lock').read_text().splitlines() if l.strip() and not l.startswith('#')}
    got = package_versions(listing)
    if got != lock: fail(f'PlatformIO packages differ from the lock: extra {sorted(got - lock)}, missing {sorted(lock - got)}')


def build_flags(version, commit):
    if not re.fullmatch(r'[0-9a-f]{40}', commit): fail('commit is not a full SHA-1')
    return f'-DFW_VERSION=\\"{commit}\\" -DORCON_RELEASE_VERSION={int(version)}'


# ---- the built image ---------------------------------------------------------------------------------------------------
def sha256(b):
    return hashlib.sha256(b).hexdigest()


def harness(bin_, *args):
    r = subprocess.run([str(bin_), *map(str, args)], capture_output=True, text=True)
    return r.returncode, (r.stdout.strip().splitlines() or [''])[-1]


def verify_build(build, version, commit, harness_bin, nm, out):
    build, out = Path(build), Path(out)
    fw = (build / 'firmware.bin').read_bytes()
    if len(fw) < 1024 or fw[0] != 0xE9: fail('firmware.bin is not an ESP32 application image')
    if fw[2:4] != FLASH_HDR: fail('firmware.bin flash header is not DIO / 8 MB / 40 MHz (the Heltec V2 layout)')
    if len(fw) > SLOT: fail(f'firmware.bin ({len(fw)} B) does not fit an OTA slot ({SLOT} B)')
    stamps = set(re.findall(rb'ORCON-RELEASE (\d+);', fw))
    if stamps != {str(version).encode()}: fail(f'release stamp {sorted(s.decode() for s in stamps)} != {version}: the image is not built as this release')
    if commit.encode() not in fw: fail('the commit is not stamped in the image (FW_VERSION)')
    files = {n: (build / n).read_bytes() for n in ('bootloader.bin', 'partitions.bin')}
    files['boot_app0.bin'] = boot_app0()
    for n, b in files.items():
        if sha256(b) != PINNED[n]: fail(f'{n} differs from the pinned board file: a release never changes the bootloader or the partition table')
    r = subprocess.run([sys.executable, '-I', str(REPO / 'tools/check_parts.py'), str(build / 'partitions.bin')], capture_output=True, text=True)
    if r.returncode: fail('partition table check failed: ' + (r.stdout + r.stderr)[-300:])
    r = subprocess.run([sys.executable, '-I', str(REPO / 'tools/check_rollback_hook.py'), str(nm), str(build / 'firmware.elf')], capture_output=True, text=True)
    if r.returncode: fail('rollback hook check failed: ' + (r.stdout + r.stderr)[-300:])
    # The device's own image rules: a v2 release manifest for exactly this image must be accepted as "control".
    out.mkdir(parents=True, exist_ok=True)
    (out / 'firmware.bin').write_bytes(fw)
    for n, b in files.items(): (out / n).write_bytes(b)
    text = orcon.release_manifest(ENV, commit, out / 'firmware.bin', int(version))
    probe = out / 'probe' / bundle_name(json.loads(text))
    probe.parent.mkdir(exist_ok=True)
    orcon.write_bundle(probe, text, '00' * 36, out / 'firmware.bin')
    code, verdict = harness(harness_bin, 'bundle', probe, sha256(fw), 0)
    shutil.rmtree(probe.parent)
    if code or verdict != 'ACCEPTED': fail(f'the device policy refuses this image as a control release: {verdict}')
    info = {'schema': 1, 'env': ENV, 'variant': VARIANT, 'commit': commit, 'version': int(version), 'name': semver_text(int(version)),
            'firmware': {'size': len(fw), 'sha256': sha256(fw)}, **{n.split('.')[0]: sha256(b) for n, b in files.items()}}
    (out / 'BUILD_INFO.json').write_text(json.dumps(info, indent=1, sort_keys=True) + '\n')
    return info


def boot_app0():
    p = Path.home() / '.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin'
    for c in sorted((Path.home() / '.platformio/packages').glob('framework-arduinoespressif32*/tools/partitions/boot_app0.bin')) + [p]:
        if c.is_file() and sha256(c.read_bytes()) == PINNED['boot_app0.bin']: return c.read_bytes()
    fail('boot_app0.bin of the pinned framework not found')


def load_info(dist):
    info = json.loads((Path(dist) / 'BUILD_INFO.json').read_text())
    fw = (Path(dist) / 'firmware.bin').read_bytes()
    if info.get('env') != ENV or info.get('variant') != VARIANT: fail('BUILD_INFO is not a control release')
    if sha256(fw) != info['firmware']['sha256'] or len(fw) != info['firmware']['size']: fail('firmware.bin differs from BUILD_INFO.json (artifact changed)')
    return info


def bundle_name(m):
    return f"orcon-{m['variant']}-v{semver_text(m['version'])}-{m['commit'][:7]}.orconfw"


# ---- manifest and signature ------------------------------------------------------------------------------------------------
def make_manifest(dist):
    info = load_info(dist)
    text = orcon.release_manifest(ENV, info['commit'], Path(dist) / 'firmware.bin', info['version'])
    m = json.loads(text)
    if (m['size'], m['sha256'], m['commit'], m['version']) != (info['firmware']['size'], info['firmware']['sha256'], info['commit'], info['version']):
        fail('manifest and BUILD_INFO disagree')
    (Path(dist) / 'manifest.txt').write_text(text)
    return text


def pubkey_der(pem_text):
    r = subprocess.run(['openssl', 'pkey', '-pubin', '-outform', 'DER'], input=pem_text.encode(), capture_output=True)
    if r.returncode: fail('public key unreadable')
    return r.stdout


def firmware_pubkey():
    h = (REPO / 'src/ota_pubkey.h').read_text()
    return '\n'.join(re.findall(r'"([^"]*)\\n"', h)) + '\n'


def sign(dist, secret):
    """The private key exists only inside this call: in a 0600 file in a 0700 directory, removed in every case."""
    if not secret or 'PRIVATE KEY-----' not in secret: fail('signing key missing: the protected environment did not provide ORCON_OTA_SIGNING_KEY')
    text = (Path(dist) / 'manifest.txt').read_text()
    tmp = tempfile.mkdtemp(prefix='orcon-sign-')
    try:
        os.chmod(tmp, 0o700)
        key = Path(tmp) / 'key.pem'
        fd = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f: f.write(secret if secret.endswith('\n') else secret + '\n')
        pub = subprocess.run(['openssl', 'pkey', '-in', str(key), '-pubout', '-outform', 'DER'], capture_output=True)
        if pub.returncode: fail('signing key unreadable (no passphrase-protected or malformed keys)')
        if pub.stdout != pubkey_der(firmware_pubkey()): fail('signing key does not match src/ota_pubkey.h: boards would refuse this release')
        r = subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(key)], input=text.encode(), capture_output=True)
        if r.returncode or not r.stdout: fail('signing failed')
        sig = r.stdout.hex()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    (Path(dist) / 'manifest.sig').write_text(sig + '\n')
    return sig


def verify_signature(text, sig_hex):
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / 'pub.pem').write_text(firmware_pubkey())
        (Path(d) / 'sig.der').write_bytes(bytes.fromhex(sig_hex))
        r = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(Path(d) / 'pub.pem'), '-signature', str(Path(d) / 'sig.der')],
                           input=text.encode(), capture_output=True)
        return r.returncode == 0


# ---- package ---------------------------------------------------------------------------------------------------------------
def clean_notes(notes):
    notes = ' '.join((notes or '').split())
    if len(notes.encode()) > 400: fail('notes longer than 400 bytes')
    if any(ord(c) < 0x20 or ord(c) == 0x7f for c in notes): fail('notes contain control characters')
    return notes


def package(dist, notes=''):
    dist = Path(dist)
    load_info(dist)
    text = (dist / 'manifest.txt').read_text()
    sig = (dist / 'manifest.sig').read_text().strip()
    if not re.fullmatch(r'[0-9a-f]{16,160}', sig) or len(sig) % 2: fail('signature file malformed')
    if not verify_signature(text, sig): fail('signature does not verify with src/ota_pubkey.h')
    m = json.loads(text)
    bundle = dist / bundle_name(m)
    orcon.write_bundle(bundle, text, sig, dist / 'firmware.bin')
    index = json.dumps({'schema': 1, 'manifest': text, 'sig': sig, 'notes': clean_notes(notes)}, separators=(',', ':'), ensure_ascii=False)
    if len(index.encode()) > 2048: fail('update index larger than the device accepts (2048 B)')
    (dist / INDEX).write_text(index)
    write_sums(dist)
    return bundle


def published(dist):
    """The files a release publishes: bundle, index, build info, checksums, optional factory image."""
    dist = Path(dist)
    return sorted(p for p in dist.iterdir() if p.is_file() and (p.suffix == '.orconfw' or p.name in (INDEX, 'BUILD_INFO.json', 'SHA256SUMS')
                                                               or p.name.endswith('-factory-usb.bin')))


def write_sums(dist):
    rows = [f'{sha256(p.read_bytes())}  {p.name}' for p in published(dist) if p.name != 'SHA256SUMS']
    (Path(dist) / 'SHA256SUMS').write_text('\n'.join(rows) + '\n')


def verify_package(dist, harness_bin):
    dist = Path(dist)
    info = load_info(dist)
    bundles = sorted(dist.glob('*.orconfw'))
    if len(bundles) != 1: fail('exactly one .orconfw expected')
    raw = bundles[0].read_bytes()
    nl = raw.find(b'\n')
    if nl < 0 or nl > 1024: fail('bundle head missing or too long')
    head = json.loads(raw[:nl])
    if sorted(head) != ['manifest', 'sig']: fail('bundle head fields')
    m = json.loads(head['manifest'])
    if raw[nl + 1:] != (dist / 'firmware.bin').read_bytes(): fail('the bundle image is not the verified firmware.bin')
    if (m['commit'], m['version'], m['sha256'], m['size'], m['env'], m['variant'], m['allow_tx']) != \
            (info['commit'], info['version'], info['firmware']['sha256'], info['firmware']['size'], ENV, VARIANT, True):
        fail('manifest does not match the build (commit, version, hash, size, env or variant)')
    if not verify_signature(head['manifest'], head['sig']): fail('bundle signature does not verify with src/ota_pubkey.h')
    code, v = harness(harness_bin, 'bundle', bundles[0], sha256(raw[nl + 1:]), 0)
    if code or v != 'ACCEPTED': fail(f'device policy refuses the bundle: {v}')
    code, v = harness(harness_bin, 'bundle', bundles[0], sha256(raw[nl + 1:]), info['version'] + 1)
    if v != 'older-release': fail(f'a newer board must refuse this release as older: {v}')
    code, v = harness(harness_bin, 'index', dist / INDEX, bundles[0], 0)
    if code or v != 'NEWER': fail(f'device update check refuses the index: {v}')
    code, v = harness(harness_bin, 'index', dist / INDEX, bundles[0], info['version'])
    if v != 'UP-TO-DATE': fail(f'a board on this release must see it as up to date: {v}')
    sums = {}
    for line in (dist / 'SHA256SUMS').read_text().splitlines():
        h, n = line.split('  ', 1); sums[n] = h
    for p in published(dist):
        if p.name != 'SHA256SUMS' and sums.get(p.name) != sha256(p.read_bytes()): fail(f'SHA256SUMS does not cover {p.name}')
    if set(sums) != {p.name for p in published(dist) if p.name != 'SHA256SUMS'}: fail('SHA256SUMS lists other files')
    return bundles[0]


def factory(dist, esptool, python=sys.executable):
    """bootloader + partition table + boot_app0 + the control image as ONE file from 0x0, for the first USB install of a NEW
    device. It erases NVS (WiFi, password, owner key, profiles): never for a device in use, never written automatically."""
    dist = Path(dist)
    info = load_info(dist)
    out = dist / f"orcon-control-v{info['name']}-{info['commit'][:7]}-factory-usb.bin"
    r = subprocess.run([python, str(esptool), '--chip', 'esp32', 'merge_bin', '-o', str(out), '--flash_mode', 'dio',
                        '--flash_freq', '40m', '--flash_size', '8MB', '0x1000', str(dist / 'bootloader.bin'), '0x8000',
                        str(dist / 'partitions.bin'), '0xe000', str(dist / 'boot_app0.bin'), '0x10000', str(dist / 'firmware.bin')],
                       capture_output=True, text=True)
    if r.returncode: fail('merge_bin failed: ' + (r.stdout + r.stderr)[-300:])
    img = out.read_bytes()
    for off, name in ((0x1000, 'bootloader.bin'), (0x8000, 'partitions.bin'), (0xE000, 'boot_app0.bin'), (0x10000, 'firmware.bin')):
        b = (dist / name).read_bytes()
        if img[off:off + len(b)] != b: fail(f'factory image: {name} at 0x{off:x} differs from the verified file')
    if len(img) != 0x10000 + len((dist / 'firmware.bin').read_bytes()): fail('factory image: unexpected length')
    write_sums(dist)
    return out


def notes_md(dist, notes='', run=''):
    dist = Path(dist)
    info = load_info(dist)
    sums = (dist / 'SHA256SUMS').read_text().strip()
    b = next(dist.glob('*.orconfw')).name
    fac = [p.name for p in published(dist) if p.name.endswith('-factory-usb.bin')]
    lines = [f"Orcon 15RF-controller voor de Heltec WiFi LoRa 32 V2: control-firmware v{info['name']}.", '']
    if clean_notes(notes): lines += [clean_notes(notes), '']
    lines += ['**Installeren (bestaande controller):** open de webinterface, *Systeem → Firmware*, en klik op *Update installeren* zodra de controller deze versie meldt. '
              f'Of download `{b}` en kies hem bij *Handmatig installeren*. De controller controleert handtekening, controlesom, type en versie, '
              'installeert in het andere geheugenslot en keert automatisch terug naar de vorige versie als de nieuwe niet binnen 5 minuten gezond opstart.', '']
    if fac:
        lines += [f'**Nieuwe controller (eerste installatie via USB):** `{fac[0]}` vanaf adres 0x0. Dit wist alle instellingen (WiFi, wachtwoord, profielen); '
                  'nooit gebruiken voor een controller die al in gebruik is.', '']
    lines += ['| | |', '|---|---|', f"| Versie | v{info['name']} (releasenummer {info['version']}) |", f"| Commit | `{info['commit']}` |",
              f"| Variant | `{ENV}` (control, met RF-zender) |", f"| Image | {info['firmware']['size']} B, SHA-256 `{info['firmware']['sha256']}` |", '',
              'Gebouwd, getest en ondertekend door de releaseworkflow' + (f' ({run})' if run else '') + '. Niet op hardware getest door de workflow: '
              'de RF-, WiFi- en OTA-hardwarevalidatie gebeurt na publicatie op een eigen controller (docs/RELEASES.md).', '',
              '```', sums, '```']
    return '\n'.join(lines) + '\n'


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest='cmd', required=True)
    v = s.add_parser('version'); v.add_argument('semver'); v.add_argument('--tags')
    so = s.add_parser('source')
    for k in ('repo', 'ref', 'event', 'default-branch', 'sha', 'head'): so.add_argument('--' + k, required=True)
    pk = s.add_parser('packages'); pk.add_argument('--list', required=True)
    fl = s.add_parser('flags'); fl.add_argument('--version', type=int, required=True); fl.add_argument('--commit', required=True)
    vb = s.add_parser('verify-build')
    for k in ('build', 'commit', 'harness', 'nm', 'out'): vb.add_argument('--' + k, required=True)
    vb.add_argument('--version', type=int, required=True)
    for name in ('manifest', 'sign', 'package', 'verify-package', 'factory', 'notes'):
        x = s.add_parser(name); x.add_argument('--dist', required=True)
        if name in ('package', 'notes'): x.add_argument('--notes', default='')
        if name == 'notes': x.add_argument('--run', default='')
        if name == 'verify-package': x.add_argument('--harness', required=True)
        if name == 'factory': x.add_argument('--esptool', required=True); x.add_argument('--python', default=sys.executable)
    a = p.parse_args(argv)
    try:
        if a.cmd == 'version':
            tags = Path(a.tags).read_text().splitlines() if a.tags else []
            n = check_version(a.semver, tags)
            print(f'number={n}\ntag=v{semver_text(n)}\nname={semver_text(n)}')
        elif a.cmd == 'source':
            check_source(a.repo, a.ref, a.event, a.default_branch, a.sha, a.head); print('source OK')
        elif a.cmd == 'packages': check_packages(Path(a.list).read_text()); print('PlatformIO packages match the lock')
        elif a.cmd == 'flags': print(build_flags(a.version, a.commit))
        elif a.cmd == 'verify-build':
            i = verify_build(a.build, a.version, a.commit, a.harness, a.nm, a.out)
            print(f"build OK: {ENV} v{i['name']} {a.commit[:7]} {i['firmware']['size']} B sha256 {i['firmware']['sha256']}")
        elif a.cmd == 'manifest': make_manifest(a.dist); print('manifest OK')
        elif a.cmd == 'sign':
            secret = os.environ.pop('ORCON_OTA_SIGNING_KEY', '')
            sign(a.dist, secret); secret = None
            print('signed (key matches src/ota_pubkey.h; key file removed)')
        elif a.cmd == 'package': print('package OK:', package(a.dist, a.notes).name)
        elif a.cmd == 'verify-package': print('package VERIFIED:', verify_package(a.dist, a.harness).name)
        elif a.cmd == 'factory': print('factory image:', factory(a.dist, a.esptool, a.python).name)
        elif a.cmd == 'notes': sys.stdout.write(notes_md(a.dist, a.notes, a.run))
    except (Refused, ValueError, KeyError, OSError) as e:
        print(f'REFUSED: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
