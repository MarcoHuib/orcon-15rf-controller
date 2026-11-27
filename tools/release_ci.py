#!/usr/bin/env python3
"""release_ci.py — the checks and steps of an official release (.github/workflows/release-control.yml).

A release publishes the Heltec V2 control firmware in two variants, each with its own signed manifest and its own
update index (src/fw_update.h): "control" (heltec_v2_sx1276_control, PlatformIO, read by every board without Matter,
v1.0.x included) and "control-matter" (heltec_v2_matter, ESP-IDF 5, the universal Matter firmware: no identity in the
image). A stable release or a beta (X.Y.Z-beta.N, a GitHub pre-release; only boards whose owner turned the beta channel
on accept it). Every step runs the same way on a developer machine; tools/test_release_ci.py runs them with throwaway
keys and tampered inputs. A --dist is one variant's directory (BUILD_INFO.json) or a release directory holding them.

  next --newest N [--channel stable|beta] [--betas "TAGS"] [--notes TEXT] -- patch|minor|major
                                         the next release, one step above N: the newest stable version among every tag
                                         and release (the history job's awk rule, the same as the publish floor; 0 =
                                         none). patch 1.0.0 -> 1.0.1; minor 1.0.5 -> 1.1.1; major 1.4.8 -> 2.0.1. A beta is
                                         that number's next beta (beta.1 when it has none; TAGS: the beta tags that exist).
                                         Refuses an empty history and a number outside 0..999 per part
  source --repo R --ref REF --event E --default-branch B --sha SHA --head SHA
                                         only a workflow_dispatch on the default branch of this repository, at its head
  packages --list FILE                   `pio pkg list -e heltec_v2_sx1276_control` must equal tools/release/platformio-packages.lock
  flags --version N [--pre P] --commit SHA
                                         PLATFORMIO_BUILD_FLAGS of the control release build
  verify-build [--variant control|control-matter] --build DIR [--boot-files DIR] --version N [--pre P] --commit SHA
               --harness BIN --nm NM --out DIR [--esptool PY]
                                         the built image: ESP image header, size, release stamp (with the beta), commit
                                         stamp, the variant's markers (the device's own policy, test/release_accept.cpp),
                                         pinned bootloader, partition table and boot_app0, the rollback hook; Matter: the
                                         Matter marker and NO embedded identity. Copies the inputs + BUILD_INFO.json
  manifest --dist DIR                    the signed-to-be v2 release manifest (orcon.py release_manifest)
  sign --dist DIR                        signs manifest.txt with the PEM in $ORCON_OTA_SIGNING_KEY (never printed or kept);
                                         refuses an empty secret or a key that is not the one in src/ota_pubkey.h
  package --dist DIR [--notes TEXT]      the .orconfw bundle, the variant's update index, SHA256SUMS
  verify-package --dist DIR --harness BIN
                                         signature (openssl, src/ota_pubkey.h) and the device's own rules on the package
  factory --dist DIR --esptool PY [--python EXE]
                                         one USB image for a NEW device (erases its settings; never written automatically)
  assemble --dist DIR --out DIR          the published files of every variant, each checked against its SHA256SUMS,
                                         one SHA256SUMS over all of them
  notes --dist DIR [--notes TEXT] [--run URL]
                                         release notes (markdown) for the GitHub Release
"""
import argparse, hashlib, json, os, re, shutil, stat, struct, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'tools'))
import orcon                                     # noqa: E402  release_manifest, write_bundle (one implementation)
from check_flash_package import PINNED           # noqa: E402  the board's bootloader, partition table, boot_app0

ENV = 'heltec_v2_sx1276_control'
VARIANT = 'control'
KINDS = {   # every variant a release carries: its build env and the update index its boards read (src/fw_update.h)
    'control': {'env': 'heltec_v2_sx1276_control', 'index': 'orcon-update.json'},
    'control-matter': {'env': 'heltec_v2_matter', 'index': 'orcon-update-matter.json'},
}
SLOT = 0x330000
INDEX = KINDS['control']['index']
MATTER_MARKER = b'ORCON-MATTER-FIRMWARE v1\0'              # idf/components/orcon_matter: orcon_matter_marker
IDENTITY_MARKER = b'ORCON-MATTER-EMBEDDED-IDENTITY v1'     # an image that holds a Matter identity: never released
FLASH_HDR = bytes.fromhex('0230')                # image header bytes 2-3: DIO, 8 MB / 40 MHz (tools/heltec_v2.sh)


class Refused(Exception):
    pass


def fail(msg):
    raise Refused(msg)


# ---- versions (same rule as src/fw_update.cpp parseSemver) ----------------------------------------------------------
SEMVER = re.compile(r'v?(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})', re.ASCII)   # ASCII digits only, as the device


def semver_number(text):
    m = SEMVER.fullmatch(text or '')
    if not m: fail(f'invalid version {text!r}: use MAJOR.MINOR.PATCH, each 0..999, no leading zeros, no suffix')
    a, b, c = map(int, m.groups())
    n = a * 1000000 + b * 1000 + c
    if not n: fail('version 0.0.0 is reserved for development builds')
    return n


def semver_text(n):
    return f'{n // 1000000}.{n // 1000 % 1000}.{n % 1000}'


def version_of(name):
    """The release number of a tag or release name, with or without "v"; 0 for anything else. 0.0.0 is reserved for
    development builds and never counts as a release. Matched exactly, as the publish job's awk floor does."""
    m = SEMVER.fullmatch(name)
    return int(m.group(1)) * 1000000 + int(m.group(2)) * 1000 + int(m.group(3)) if m else 0


def check_version(text, existing):
    """The number for text; refused when its tag already exists or a published version is not older."""
    n = semver_number(text)
    tag = 'v' + semver_text(n)
    seen = [x for x in map(version_of, existing) if x]   # tags and release names; anything else is not a version
    if n in seen: fail(f'{tag} already exists (tag or release): a version is published once; see docs/RELEASES.md "herstel"')
    if seen and n <= max(seen):
        fail(f'{tag} is not newer than the newest published version v{semver_text(max(seen))}: boards refuse older releases')
    return n


BUMPS = ('patch', 'minor', 'major')
RELEASE_TAG = re.compile(r'v(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})(?:-beta\.([1-9][0-9]?))?', re.ASCII)


def release_tag(n, pre=0):
    return f'v{semver_text(n)}' + (f'-beta.{pre}' if pre else '')


def release_name(n, pre=0):
    return release_tag(n, pre)[1:]


def parse_release_tag(text):
    """(number, pre) of "vX.Y.Z" (pre 0) or "vX.Y.Z-beta.N" (N 1..99, src/fw_update.h parseSemverPre); None otherwise."""
    m = RELEASE_TAG.fullmatch(text or '')
    if not m: return None
    n = int(m.group(1)) * 1000000 + int(m.group(2)) * 1000 + int(m.group(3))
    return (n, int(m.group(4) or 0)) if n else None


def release_rank(n, pre):
    """otap::releaseRank: a stable release ranks above every beta of its own number."""
    return n * 100 + (pre if pre else 100)


def next_release(kind, newest, channel='stable', betas=()):
    """(number, pre): stable, one step above the newest stable (next_version); beta, that same number's next beta. The
    beta tags that exist count, whatever their state: a beta number is never used twice either."""
    n = next_version(kind, newest)
    if channel == 'stable': return n, 0
    if channel != 'beta': fail(f'channel {channel!r}: stable or beta')
    taken = [pre for num, pre in filter(None, map(parse_release_tag, betas)) if num == n and pre]
    pre = max(taken, default=0) + 1
    if pre > 99: fail(f'v{semver_text(n)} has 99 betas: release it, or choose a bigger step')
    return n, pre


def next_version(kind, newest):
    """The number of the next release: one step above `newest`, the newest version among every tag and release,
    published or not (a draft, or a tag left by a failed publish, keeps its number: a number is never used twice). A new
    minor or major starts its patches at 1. No history (0) is refused: a failed lookup must never restart the count."""
    if kind not in BUMPS: fail(f'release type {kind!r}: choose patch, minor or major')
    if not isinstance(newest, int) or not 0 <= newest <= 999999999: fail(f'newest version {newest!r}: not a release number')
    if not newest: fail('no earlier release in the tags and releases: refusing to start the count (did the lookup fail?)')
    top = newest
    a, b, c = top // 1000000, top // 1000 % 1000, top % 1000
    a, b, c = {'patch': (a, b, c + 1), 'minor': (a, b + 1, 1), 'major': (a + 1, 0, 1)}[kind]
    if max(a, b, c) > 999: fail(f'{kind} after v{semver_text(top)} leaves the range 0..999 per part: choose a bigger step')
    return check_version(f'{a}.{b}.{c}', [semver_text(top)])


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


UNUSED_LOCAL = ('tool-openocd-esp32', 'toolchain-riscv32-esp')   # listed when installed for other work; never used here


def check_packages(listing, core=None):
    """The env's packages from `pio pkg list`, plus PlatformIO Core's own SCons (resolved from a range by Core; Codex r3),
    must equal the lock exactly."""
    lock = {l.strip() for l in (REPO / 'tools/release/platformio-packages.lock').read_text().splitlines() if l.strip() and not l.startswith('#')}
    got = {r for r in package_versions(listing) if r.split()[0] not in UNUSED_LOCAL}
    scons = Path(core or os.environ.get('PLATFORMIO_CORE_DIR') or Path.home() / '.platformio') / 'packages/tool-scons/package.json'
    if scons.is_file(): got.add('tool-scons ' + json.loads(scons.read_text())['version'])
    if got != lock: fail(f'PlatformIO packages differ from the lock: extra {sorted(got - lock)}, missing {sorted(lock - got)}')


def build_flags(version, commit, pre=0):
    if not re.fullmatch(r'[0-9a-f]{40}', commit): fail('commit is not a full SHA-1')
    if not 0 <= int(pre) <= 99: fail('beta number outside 1..99')
    return f'-DFW_VERSION=\\"{commit}\\" -DORCON_RELEASE_VERSION={int(version)}' + (f' -DORCON_RELEASE_PRE={int(pre)}' if int(pre) else '')


def stamp(version, pre=0):
    """The release stamp the image carries (src/ota_signed.cpp RELEASE_STAMP)."""
    return f'{int(version)}' + (f'-beta.{int(pre)}' if int(pre) else '')


STAMP = re.compile(rb'ORCON-RELEASE (\d+(?:-beta\.\d+)?);')


# ---- the built image ---------------------------------------------------------------------------------------------------
def sha256(b):
    return hashlib.sha256(b).hexdigest()


def harness(bin_, *args):
    r = subprocess.run([str(bin_), *map(str, args)], capture_output=True, text=True)
    return r.returncode, (r.stdout.strip().splitlines() or [''])[-1]


def check_esp_image(b):
    """The complete ESP32 application image as the bootloader reads it: header, 1..16 segments, the XOR checksum (seed
    0xEF) as the last byte of the 16-byte zero padding, the appended SHA-256, nothing after it (Codex r3: the magic
    byte alone is not enough)."""
    if len(b) < 24 + 8 + 16 + 32 or b[0] != 0xE9: fail('firmware.bin is not an ESP32 application image')
    if not 1 <= b[1] <= 16: fail(f'firmware.bin: {b[1]} segments')
    if int.from_bytes(b[12:14], 'little') != 0 or b[23] != 1: fail('firmware.bin: not an ESP32 image with an appended SHA-256')
    off, cs = 24, 0xEF
    for _ in range(b[1]):
        if off + 8 > len(b): fail('firmware.bin: truncated segment header')
        ln = struct.unpack_from('<I', b, off + 4)[0]; off += 8
        if ln % 4 or off + ln > len(b): fail('firmware.bin: bad segment length')
        for v in b[off:off + ln]: cs ^= v
        off += ln
    pos = off + 15 - off % 16
    if pos + 33 != len(b) or any(b[off:pos]): fail('firmware.bin: bad padding or trailing data')
    if b[pos] != cs: fail('firmware.bin: segment checksum mismatch')
    if hashlib.sha256(b[:pos + 1]).digest() != b[pos + 1:]: fail('firmware.bin: appended SHA-256 mismatch')


def check_elf_image(build, esptool, python=sys.executable, elf='firmware.elf', bin_='firmware.bin', idf=False):
    """firmware.bin is exactly what the pinned esptool makes of the checked firmware.elf with the release settings
    (Codex r4: the ELF hash field alone does not bind the segments, addresses and entry point). ESP-IDF 5 adds the chip
    revision range to the image header (its own build's elf2image arguments); esptool "module" runs the installed one."""
    build = Path(build)
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / 'regen.bin'
        tool = [python, '-m', 'esptool'] if str(esptool) == 'module' else [python, str(esptool)]
        rev = ['--min-rev-full', '0', '--max-rev-full', '399'] if idf else []
        r = subprocess.run(tool + ['--chip', 'esp32', 'elf2image', '--flash_mode', 'dio', '--flash_freq', '40m',
                                   '--flash_size', '8MB', '--elf-sha256-offset', '0xb0', *rev, '-o', str(out), str(build / elf)],
                           capture_output=True, text=True)
        if r.returncode or not out.is_file(): fail('elf2image failed: ' + (r.stdout + r.stderr)[-300:])
        if out.read_bytes() != (build / bin_).read_bytes(): fail(f'{bin_} is not the image of the checked {elf}')


def verify_build(build, version, commit, harness_bin, nm, out, esptool=None, python=sys.executable, variant='control', pre=0,
                 boot_files=None):
    """control: a PlatformIO build directory (firmware.bin/.elf, bootloader.bin, partitions.bin). control-matter: the
    ESP-IDF build directory (tools/build_idf.sh --release: orcon_15rf.bin/.elf); its board files come from boot_files,
    the control build's, since a release never changes the bootloader or the partition table."""
    if variant not in KINDS: fail(f'variant {variant!r} is not released')
    if not 0 <= int(pre) <= 99: fail('beta number outside 1..99')
    matter = variant == 'control-matter'
    build, out = Path(build), Path(out)
    elf, bin_ = ('orcon_15rf.elf', 'orcon_15rf.bin') if matter else ('firmware.elf', 'firmware.bin')
    if matter and not boot_files: fail('a Matter release takes its board files from the control build (--boot-files)')
    boot = Path(boot_files) if matter else build
    fw = (build / bin_).read_bytes()
    check_esp_image(fw)
    # esptool writes the ELF's SHA-256 into the app description (offset 0xB0): the image IS the checked ELF's.
    if fw[0xB0:0xD0] != hashlib.sha256((build / elf).read_bytes()).digest(): fail(f'{bin_} was not made from the checked {elf}')
    if esptool: check_elf_image(build, esptool, python, elf, bin_, idf=matter)   # the workflow always passes it
    if fw[2:4] != FLASH_HDR: fail('firmware.bin flash header is not DIO / 8 MB / 40 MHz (the Heltec V2 layout)')
    if len(fw) > SLOT: fail(f'firmware.bin ({len(fw)} B) does not fit an OTA slot ({SLOT} B)')
    stamps = set(STAMP.findall(fw))
    if stamps != {stamp(version, pre).encode()}:
        fail(f'release stamp {sorted(s.decode() for s in stamps)} != {stamp(version, pre)}: the image is not built as this release')
    if commit.encode() not in fw: fail('the commit is not stamped in the image (FW_VERSION)')
    if (MATTER_MARKER in fw) != matter: fail('the Matter marker does not match the variant' + (' (no Matter image)' if matter else ' (a Matter image)'))
    if IDENTITY_MARKER in fw: fail('the image holds a Matter identity (a migration build): a release never carries one')
    files = {n: (boot / n).read_bytes() for n in ('bootloader.bin', 'partitions.bin')}
    files['boot_app0.bin'] = boot_app0()
    for n, b in files.items():
        if sha256(b) != PINNED[n]: fail(f'{n} differs from the pinned board file: a release never changes the bootloader or the partition table')
    r = subprocess.run([sys.executable, '-I', str(REPO / 'tools/check_parts.py'), str(boot / 'partitions.bin')], capture_output=True, text=True)
    if r.returncode: fail('partition table check failed: ' + (r.stdout + r.stderr)[-300:])
    r = subprocess.run([sys.executable, '-I', str(REPO / 'tools/check_rollback_hook.py'), str(nm), str(build / elf)], capture_output=True, text=True)
    if r.returncode: fail('rollback hook check failed: ' + (r.stdout + r.stderr)[-300:])
    # The device's own image rules: a v2 release manifest for exactly this image must be accepted as this variant.
    out.mkdir(parents=True, exist_ok=True)
    (out / 'firmware.bin').write_bytes(fw)
    for n, b in files.items(): (out / n).write_bytes(b)
    text = orcon.release_manifest(KINDS[variant]['env'], commit, out / 'firmware.bin', int(version), int(pre))
    probe = out / 'probe' / bundle_name(json.loads(text))
    probe.parent.mkdir(exist_ok=True)
    orcon.write_bundle(probe, text, '00' * 36, out / 'firmware.bin')
    code, verdict = harness(harness_bin, 'bundle', probe, sha256(fw), 0, 0, int(bool(pre)))
    shutil.rmtree(probe.parent)
    if code or verdict != 'ACCEPTED': fail(f'the device policy refuses this image as a {variant} release: {verdict}')
    info = {'schema': 1, 'env': KINDS[variant]['env'], 'variant': variant, 'commit': commit, 'version': int(version), 'pre': int(pre),
            'name': release_name(int(version), int(pre)),
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
    kind = KINDS.get(info.get('variant'))
    if not kind or info.get('env') != kind['env']: fail('BUILD_INFO is not a released variant (control or control-matter)')
    info.setdefault('pre', 0)   # a control build record from before betas
    if not isinstance(info['pre'], int) or isinstance(info['pre'], bool) or not 0 <= info['pre'] <= 99: fail('BUILD_INFO beta number malformed')
    if sha256(fw) != info['firmware']['sha256'] or len(fw) != info['firmware']['size']: fail('firmware.bin differs from BUILD_INFO.json (artifact changed)')
    if info.get('name') != release_name(int(info.get('version', 0)), info['pre']) or not re.fullmatch(r'[0-9a-f]{40}', str(info.get('commit'))):
        fail('BUILD_INFO version, name or commit malformed')
    for n in ('bootloader', 'partitions', 'boot_app0'):
        if info.get(n) != PINNED[n + '.bin'] or sha256((Path(dist) / (n + '.bin')).read_bytes()) != PINNED[n + '.bin']:
            fail(f'BUILD_INFO / {n}.bin is not the pinned board file')
    return info


def bundle_name(m):
    """fwup::bundleName: orcon-control-v1.2.3-abcdef1.orconfw, a beta orcon-control-v1.2.3-beta.4-abcdef1.orconfw."""
    return f"orcon-{m['variant']}-{release_tag(m['version'], m.get('pre', 0))}-{m['commit'][:7]}.orconfw"


def factory_name(info):
    return f"orcon-{info['variant']}-{release_tag(info['version'], info['pre'])}-{info['commit'][:7]}-factory-usb.bin"


def variant_dirs(dist):
    """dist itself when it is one variant's directory, else the variant directories in it (a release)."""
    dist = Path(dist)
    if (dist / 'BUILD_INFO.json').is_file(): return [dist]
    ds = [d for d in sorted(dist.iterdir()) if d.is_dir() and (d / 'BUILD_INFO.json').is_file()] if dist.is_dir() else []
    if not ds: fail(f'{dist}: no variant build (BUILD_INFO.json)')
    return ds


def release_infos(dist):
    """A release directory: every variant, one release (version, beta, commit). One variant's directory: just it."""
    ds = variant_dirs(dist)
    infos = [load_info(d) for d in ds]
    if len(ds) > 1 or not (Path(dist) / 'BUILD_INFO.json').is_file():
        if sorted(i['variant'] for i in infos) != sorted(KINDS): fail(f"a release carries every variant {sorted(KINDS)}: {sorted(i['variant'] for i in infos)}")
        if len({(i['version'], i['pre'], i['commit']) for i in infos}) != 1: fail('the variants are not one release (version, beta, commit)')
    return list(zip(ds, infos))


# ---- manifest and signature ------------------------------------------------------------------------------------------------
def make_manifest(dist):
    for d, info in release_infos(dist):
        text = orcon.release_manifest(info['env'], info['commit'], d / 'firmware.bin', info['version'], info['pre'])
        m = json.loads(text)
        if (m['size'], m['sha256'], m['commit'], m['version'], m.get('pre', 0)) != \
                (info['firmware']['size'], info['firmware']['sha256'], info['commit'], info['version'], info['pre']):
            fail('manifest and BUILD_INFO disagree')
        (d / 'manifest.txt').write_text(text)
    return text


def pubkey_der(pem_text):
    r = subprocess.run(['openssl', 'pkey', '-pubin', '-outform', 'DER'], input=pem_text.encode(), capture_output=True)
    if r.returncode: fail('public key unreadable')
    return r.stdout


def firmware_pubkey():
    h = (REPO / 'src/ota_pubkey.h').read_text()
    return '\n'.join(re.findall(r'"([^"]*)\\n"', h)) + '\n'


def sign(dist, secret):
    """The private key exists only inside this call: in a 0600 file in a 0700 directory, removed in every case. A release
    directory: every variant's manifest, with the one key."""
    if not secret or 'PRIVATE KEY-----' not in secret: fail('signing key missing: the protected environment did not provide ORCON_OTA_SIGNING_KEY')
    texts = [(d, (d / 'manifest.txt').read_text()) for d, _ in release_infos(dist)]
    sigs = []
    tmp = tempfile.mkdtemp(prefix='orcon-sign-')
    try:
        os.chmod(tmp, 0o700)
        key = Path(tmp) / 'key.pem'
        fd = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f: f.write(secret if secret.endswith('\n') else secret + '\n')
        pub = subprocess.run(['openssl', 'pkey', '-in', str(key), '-pubout', '-outform', 'DER'], capture_output=True)
        if pub.returncode: fail('signing key unreadable (no passphrase-protected or malformed keys)')
        if pub.stdout != pubkey_der(firmware_pubkey()): fail('signing key does not match src/ota_pubkey.h: boards would refuse this release')
        for d, text in texts:
            r = subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(key)], input=text.encode(), capture_output=True)
            if r.returncode or not r.stdout: fail('signing failed')
            sigs.append((d, r.stdout.hex()))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for d, sig in sigs: (d / 'manifest.sig').write_text(sig + '\n')
    return sigs[-1][1]


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
    bundle = None
    for d, info in release_infos(dist):
        text = (d / 'manifest.txt').read_text()
        sig = (d / 'manifest.sig').read_text().strip()
        if not re.fullmatch(r'[0-9a-f]{16,160}', sig) or len(sig) % 2: fail('signature file malformed')
        if not verify_signature(text, sig): fail('signature does not verify with src/ota_pubkey.h')
        m = json.loads(text)
        bundle = d / bundle_name(m)
        orcon.write_bundle(bundle, text, sig, d / 'firmware.bin')
        index = json.dumps({'schema': 1, 'manifest': text, 'sig': sig, 'notes': clean_notes(notes)}, separators=(',', ':'), ensure_ascii=False)
        if len(index.encode()) > 2048: fail('update index larger than the device accepts (2048 B)')
        (d / KINDS[info['variant']]['index']).write_text(index)
        write_sums(d)
    return bundle


def published(dist):
    """The files a variant publishes: bundle, its index, build info, checksums, optional factory image."""
    dist = Path(dist)
    info = json.loads((dist / 'BUILD_INFO.json').read_text())
    index = KINDS.get(info.get('variant'), KINDS['control'])['index']
    return sorted(p for p in dist.iterdir() if p.is_file() and (p.suffix == '.orconfw' or p.name in (index, 'BUILD_INFO.json', 'SHA256SUMS')
                                                               or p.name.endswith('-factory-usb.bin')))


def write_sums(dist):
    rows = [f'{sha256(p.read_bytes())}  {p.name}' for p in published(dist) if p.name != 'SHA256SUMS']
    (Path(dist) / 'SHA256SUMS').write_text('\n'.join(rows) + '\n')


def verify_package(dist, harness_bin, official=False, notes=None, release=None):
    """official: the workflow's run, where the factory image must be there. notes: the dispatch input, which the
    update index must carry exactly (normalised). release: the tag prepare chose; the build, and through it the manifest,
    the bundle and factory names and the update index checked below, must carry exactly that version. A release
    directory: every variant, one release."""
    bundle = None
    for d, info in release_infos(dist):
        bundle = verify_variant(d, info, harness_bin, official, notes, release)
    return bundle


def verify_variant(dist, info, harness_bin, official, notes, release):
    variant, pre = info['variant'], info['pre']
    index, other = KINDS[variant]['index'], next(v for v in KINDS if v != variant)
    beta = int(bool(pre))
    fw = (dist / 'firmware.bin').read_bytes()
    if release is not None:
        if release != release_tag(info['version'], pre) or parse_release_tag(release) != (info['version'], pre):
            fail(f"the package is v{info['name']}, not the release {release} that prepare chose")
        # The image itself, as published, not only its build record (Codex auto-version r5: a build changed after its check).
        if set(STAMP.findall(fw)) != {stamp(info['version'], pre).encode()} or info['commit'].encode() not in fw:
            fail(f"the image itself is not release {release} of commit {info['commit'][:7]} (release stamp or commit stamp)")
    if (MATTER_MARKER in fw) != (variant == 'control-matter') or IDENTITY_MARKER in fw:
        fail('the image is not this variant, or it holds a Matter identity')
    check_esp_image(fw)
    bundles = sorted(dist.glob('*.orconfw'))
    if len(bundles) != 1: fail('exactly one .orconfw expected')
    raw = bundles[0].read_bytes()
    nl = raw.find(b'\n')
    if nl < 0 or nl > 1024: fail('bundle head missing or too long')
    head = json.loads(raw[:nl])
    if sorted(head) != ['manifest', 'sig']: fail('bundle head fields')
    m = json.loads(head['manifest'])
    if raw[nl + 1:] != (dist / 'firmware.bin').read_bytes(): fail('the bundle image is not the verified firmware.bin')
    if (m['commit'], m['version'], m.get('pre', 0), m['sha256'], m['size'], m['env'], m['variant'], m['allow_tx']) != \
            (info['commit'], info['version'], pre, info['firmware']['sha256'], info['firmware']['size'], info['env'], variant, True):
        fail('manifest does not match the build (commit, version, beta, hash, size, env or variant)')
    if not verify_signature(head['manifest'], head['sig']): fail('bundle signature does not verify with src/ota_pubkey.h')
    code, v = harness(harness_bin, 'bundle', bundles[0], sha256(raw[nl + 1:]), 0, 0, beta)
    if code or v != 'ACCEPTED': fail(f'device policy refuses the bundle: {v}')
    code, v = harness(harness_bin, 'bundle', bundles[0], sha256(raw[nl + 1:]), info['version'] + 1, 0, beta)
    if v != 'older-release': fail(f'a newer board must refuse this release as older: {v}')
    if beta:
        code, v = harness(harness_bin, 'bundle', bundles[0], sha256(raw[nl + 1:]), 0, 0, 0)
        if v != 'beta-disabled': fail(f'a board without the beta channel must refuse a beta: {v}')
    code, v = harness(harness_bin, 'index', dist / index, bundles[0], 0, variant, 0, beta)
    if code or v != 'NEWER': fail(f'device update check refuses the index: {v}')
    code, v = harness(harness_bin, 'index', dist / index, bundles[0], info['version'], variant, pre, beta)
    if v != 'UP-TO-DATE': fail(f'a board on this release must see it as up to date: {v}')
    code, v = harness(harness_bin, 'index', dist / index, bundles[0], 0, other, 0, beta)
    if v != 'wrong-variant': fail(f'a {other} board must never take this {variant} release: {v}')
    sums = {}
    for line in (dist / 'SHA256SUMS').read_text().splitlines():
        h, n = line.split('  ', 1); sums[n] = h
    for p in published(dist):
        if p.name != 'SHA256SUMS' and sums.get(p.name) != sha256(p.read_bytes()): fail(f'SHA256SUMS does not cover {p.name}')
    if set(sums) != {p.name for p in published(dist) if p.name != 'SHA256SUMS'}: fail('SHA256SUMS lists other files')
    factories = [p for p in published(dist) if p.name.endswith('-factory-usb.bin')]
    if official and len(factories) != 1: fail('an official release carries exactly one factory image per variant')
    if notes is not None and json.loads((dist / index).read_text())['notes'] != clean_notes(notes): fail('update index notes differ from the release input')
    for f in factories:
        if f.name != factory_name(info): fail(f'unexpected factory image name {f.name}')
        check_factory(dist, f)
    return bundles[0]


def factory(dist, esptool, python=sys.executable):
    """bootloader + partition table + boot_app0 + the variant's image as ONE file from 0x0, for the first USB install of
    a NEW device. It erases NVS (WiFi, password, owner key, profiles): never for a device in use, never written
    automatically. A release directory: one per variant."""
    out = None
    for d, info in release_infos(dist):
        out = factory_one(d, info, esptool, python)
    return out


def factory_one(dist, info, esptool, python):
    out = dist / factory_name(info)
    r = subprocess.run([python, str(esptool), '--chip', 'esp32', 'merge_bin', '-o', str(out), '--flash_mode', 'dio',
                        '--flash_freq', '40m', '--flash_size', '8MB', '0x1000', str(dist / 'bootloader.bin'), '0x8000',
                        str(dist / 'partitions.bin'), '0xe000', str(dist / 'boot_app0.bin'), '0x10000', str(dist / 'firmware.bin')],
                       capture_output=True, text=True)
    if r.returncode: fail('merge_bin failed: ' + (r.stdout + r.stderr)[-300:])
    check_factory(dist, out)
    write_sums(dist)
    return out


def check_factory(dist, path):
    """Exactly the pinned bootloader, partition table and boot_app0 and the verified control image at their offsets, 0xFF
    in between (merge_bin padding), nothing after the image."""
    dist, img = Path(dist), Path(path).read_bytes()
    fw = (dist / 'firmware.bin').read_bytes()
    want = bytearray(b'\xff' * (0x10000 + len(fw)))
    for off, name in ((0x1000, 'bootloader.bin'), (0x8000, 'partitions.bin'), (0xE000, 'boot_app0.bin'), (0x10000, 'firmware.bin')):
        b = (dist / name).read_bytes()
        if name != 'firmware.bin' and sha256(b) != PINNED[name]: fail(f'factory image: {name} is not the pinned board file')
        want[off:off + len(b)] = b
    if img != bytes(want): fail(f'factory image {Path(path).name} is not exactly the verified boot files + image')


def assemble(dist, out):
    """The files a release publishes, from every variant's verified directory: bundle, index, factory image and build
    record (as BUILD_INFO-<variant>.json), each checked against its variant's SHA256SUMS; one SHA256SUMS over all."""
    pairs = release_infos(dist)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()): fail(f'{out} is not empty')
    rows = {}
    for d, info in pairs:
        sums = dict(reversed(line.split('  ', 1)) for line in (d / 'SHA256SUMS').read_text().splitlines())
        for f in published(d):
            if f.name == 'SHA256SUMS': continue
            b = f.read_bytes()
            if sums.get(f.name) != sha256(b): fail(f"{info['variant']}/{f.name} differs from its SHA256SUMS")
            name = f"BUILD_INFO-{info['variant']}.json" if f.name == 'BUILD_INFO.json' else f.name
            if name in rows: fail(f'{name} twice in one release')
            (out / name).write_bytes(b)
            rows[name] = sha256(b)
    (out / 'SHA256SUMS').write_text(''.join(f'{h}  {n}\n' for n, h in sorted(rows.items())))
    return sorted(rows)


def notes_md(dist, notes='', run='', sums_file=None):
    pairs = release_infos(dist)
    by = {info['variant']: (d, info) for d, info in pairs}
    info = pairs[0][1]
    tag = release_tag(info['version'], info['pre'])
    sums = Path(sums_file).read_text().strip() if sums_file else '\n'.join((d / 'SHA256SUMS').read_text().strip() for d, _ in pairs)
    bundle = {v: next(d.glob('*.orconfw')).name for v, (d, _) in by.items()}
    fac = {v: next((f.name for f in published(d) if f.name.endswith('-factory-usb.bin')), None) for v, (d, _) in by.items()}
    lines = [f"Orcon 15RF-controller voor de Heltec WiFi LoRa 32 V2: control-firmware {tag}" + (" (bèta)." if info['pre'] else "."), '']
    if info['pre']:
        lines += ['**Bèta.** Alleen een controller waarvan de eigenaar het bètakanaal aanzette, krijgt deze versie aangeboden. '
                  'Zonder bètakanaal blijft de stabiele versie staan.', '']
    note = clean_notes(notes)
    lines += ['**Installeren (bestaande controller):** open de webinterface, *Systeem → Firmware*, en klik op *Update installeren* zodra de controller deze versie meldt. '
              'Elke controller krijgt zijn eigen variant: een gewone update voegt Matter nooit toe en haalt het nooit weg. '
              'Of kies het bestand van je variant bij *Handmatig installeren*. De controller controleert handtekening, controlesom, type en versie, '
              'installeert in het andere geheugenslot en keert automatisch terug naar de vorige versie als de nieuwe niet binnen 5 minuten gezond opstart.', '']
    if 'control-matter' in by:
        lines += [f"**Matter (experimenteel, niet gecertificeerd):** `{bundle['control-matter']}` is dezelfde firmware met Matter (Apple Woning en andere Matter-apps). "
                  'Een controller zonder Matter krijgt die nooit vanzelf: kies het bestand bij *Handmatig installeren* en bevestig de wissel. '
                  'Activeer daarna Matter op de pagina *Verbinden* en koppel met de QR-code. Apple Woning meldt een niet-gecertificeerd accessoire: '
                  "kies 'Voeg toch toe'. Dit project heeft geen eigen vendor-ID en is niet door de CSA gecertificeerd. "
                  f"Terug zonder Matter: `{bundle.get('control', '')}` met dezelfde bevestiging. "
                  'Een controller met v1.0.x installeert eerst deze control-versie (*Update installeren*); pas die kent de Matter-variant.', '']
    if any(fac.values()):
        lines += ['**Nieuwe controller (eerste installatie via USB):** ' + ' of '.join(
                      f"`{fac[v]}` ({'met Matter' if v == 'control-matter' else 'zonder Matter'})" for v in sorted(by) if fac[v]) +
                  ' vanaf adres 0x0. Dit wist alle instellingen (WiFi, wachtwoord, profielen); nooit gebruiken voor een controller die al in gebruik is.', '']
    if note:   # the owner's text, never markup: it must not hide the instructions above (Codex r4)
        lines += ['**Notities:** ' + ''.join(f'&#{ord(c)};' if c in '&<>"\'\\`*_[]#|~!' else c for c in note), '']
    rel = f"{tag} (releasenummer {info['version']}" + (f", bèta {info['pre']}" if info['pre'] else '') + ')'
    lines += ['| | |', '|---|---|', f"| Versie | {rel} |", f"| Commit | `{info['commit']}` |"]
    for v in sorted(by):
        i = by[v][1]
        what = 'control, met RF-zender' if v == 'control' else 'control-matter: control met Matter, de identiteit op het apparaat'
        lines += [f"| Variant `{i['env']}` | {what}; image {i['firmware']['size']} B, SHA-256 `{i['firmware']['sha256']}` |"]
    lines += ['', 'Gebouwd, getest en ondertekend door de releaseworkflow' + (f' ({run})' if run else '') + '. Niet op hardware getest door de workflow: '
              'de RF-, WiFi-, Matter- en OTA-hardwarevalidatie gebeurt na publicatie op een eigen controller (docs/RELEASES.md).', '',
              '```', sums, '```']
    return '\n'.join(lines) + '\n'


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest='cmd', required=True)
    nx = s.add_parser('next'); nx.add_argument('kind'); nx.add_argument('--newest', required=True); nx.add_argument('--notes', default='')
    nx.add_argument('--channel', default='stable'); nx.add_argument('--betas', default='')
    so = s.add_parser('source')
    for k in ('repo', 'ref', 'event', 'default-branch', 'sha', 'head'): so.add_argument('--' + k, required=True)
    pk = s.add_parser('packages'); pk.add_argument('--list', required=True)
    fl = s.add_parser('flags'); fl.add_argument('--version', type=int, required=True); fl.add_argument('--commit', required=True)
    fl.add_argument('--pre', type=int, default=0)
    vb = s.add_parser('verify-build')
    for k in ('build', 'commit', 'harness', 'nm', 'out', 'esptool'): vb.add_argument('--' + k, required=True)
    vb.add_argument('--version', type=int, required=True); vb.add_argument('--pre', type=int, default=0)
    vb.add_argument('--variant', default='control', choices=sorted(KINDS)); vb.add_argument('--boot-files')
    asm = s.add_parser('assemble'); asm.add_argument('--dist', required=True); asm.add_argument('--out', required=True)
    for name in ('manifest', 'sign', 'package', 'verify-package', 'factory', 'notes'):
        x = s.add_parser(name); x.add_argument('--dist', required=True)
        if name in ('package', 'notes'): x.add_argument('--notes', default='')
        if name == 'notes': x.add_argument('--run', default=''); x.add_argument('--sums')
        if name == 'verify-package':
            x.add_argument('--harness', required=True); x.add_argument('--official', action='store_true'); x.add_argument('--notes')
            x.add_argument('--release')
        if name == 'factory': x.add_argument('--esptool', required=True); x.add_argument('--python', default=sys.executable)
    a = p.parse_args(argv)
    try:
        if a.cmd == 'next':
            if not re.fullmatch(r'0|[1-9][0-9]{0,8}', a.newest, re.ASCII): fail(f'newest version {a.newest!r}: not a release number')
            n, pre = next_release(a.kind, int(a.newest), a.channel, a.betas.split())
            clean_notes(a.notes)   # refused here, before anything is built
            print(f'newest stable tag or release v{semver_text(int(a.newest))}, {a.kind} {a.channel}: {release_tag(n, pre)}', file=sys.stderr)
            print(f'number={n}\npre={pre}\ntag={release_tag(n, pre)}\nname={release_name(n, pre)}\nrank={release_rank(n, pre)}\n'
                  f'prerelease={"true" if pre else "false"}')
        elif a.cmd == 'source':
            check_source(a.repo, a.ref, a.event, a.default_branch, a.sha, a.head); print('source OK')
        elif a.cmd == 'packages': check_packages(Path(a.list).read_text()); print('PlatformIO packages match the lock')
        elif a.cmd == 'flags': print(build_flags(a.version, a.commit, a.pre))
        elif a.cmd == 'verify-build':
            i = verify_build(a.build, a.version, a.commit, a.harness, a.nm, a.out, a.esptool, variant=a.variant, pre=a.pre, boot_files=a.boot_files)
            print(f"build OK: {i['env']} v{i['name']} {a.commit[:7]} {i['firmware']['size']} B sha256 {i['firmware']['sha256']}")
        elif a.cmd == 'assemble': print('release files:', ' '.join(assemble(a.dist, a.out)))
        elif a.cmd == 'manifest': make_manifest(a.dist); print('manifest OK')
        elif a.cmd == 'sign':
            secret = os.environ.pop('ORCON_OTA_SIGNING_KEY', '')
            sign(a.dist, secret); secret = None
            print('signed (key matches src/ota_pubkey.h; key file removed)')
        elif a.cmd == 'package': print('package OK:', package(a.dist, a.notes).name)
        elif a.cmd == 'verify-package': print('package VERIFIED:', verify_package(a.dist, a.harness, a.official, a.notes, a.release).name)
        elif a.cmd == 'factory': print('factory image:', factory(a.dist, a.esptool, a.python).name)
        elif a.cmd == 'notes': sys.stdout.write(notes_md(a.dist, a.notes, a.run, a.sums))
    except (Refused, ValueError, KeyError, OSError) as e:
        print(f'REFUSED: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
