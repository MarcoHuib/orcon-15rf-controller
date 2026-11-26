"""check_privacy.py [files...] | --history BASE[..HEAD] — fail on data that must not be published (default: all
tracked files). The repository is public: real Orcon device IDs, MAC addresses,
LAN addresses and hardware recordings stay local. Allowed are the placeholder/example
values that the original sources already use, synthetic IDs (serial 000000..000999),
locally administered example MACs (02:00:00:...), RFC 5737 documentation addresses.
Matter (docs/MATTER.md): no device identity (DAC key, verifier, salt, certificates, passcode), no pairing code or QR
payload (only the CHIP specification's examples), no identity, firmware, key or raw log files.
--history checks every commit of a range the same way: its added files, its added lines and its message (a secret that
a later commit removes again is still public). Problems name the rule and the place, never the value."""
import re
import subprocess
import sys

ALLOWED_IDS = {"29:012345", "29:074565", "32:178990", "32:179422", "63:262143"}   # pre-existing examples
ALLOWED_IDS |= {"63:262142"}   # RAMSES NUL address (the 1FC9 bind offer target): a protocol constant, not a device
ALLOWED_IPS = {"0.0.0.0", "127.0.0.1", "10.0.0.5", "192.168.1.10", "192.168.4.1",
               "192.168.2.137", "192.168.2.139", "192.168.2.140"}                    # pre-existing defaults/examples
ID = re.compile(r"\b(\d\d):(\d{6})\b")
MAC = re.compile(r"\b[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}\b")
IP = re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?!\w|\.\d)")   # as \b...\b, but never inside a longer dotted number (an OID)
HOME = re.compile(r"(?:/Users|/home)/[A-Za-z][\w.-]*")   # personal absolute paths
# A private key: a PEM block whose payload is real key material, in any representation: a .pem file,
# one long line, C/JS string lines, a single escaped string, traditional encrypted PEM with headers
# (Codex OWNER-12). The release key, the dev OTA key or an owner key must never be committed; header
# markers and fake fixtures ("AAAA") are fine. docs/RELEASESLEUTEL.md, tests: tools/test_check_privacy.py.
KEYBLOCK = re.compile(r"-----BEGIN ([A-Z ]*)PRIVATE KEY-----(.*?)-----END \1PRIVATE KEY-----", re.S)
PEMHDR = re.compile(r"^[\s\"']*[A-Za-z-]+:[^\n]*$", re.M)   # Proc-Type / DEK-Info lines, also inside string literals
GLUE = re.compile(r"[\"']\s*\+\s*[\"']")   # "..." + "...": only this '+' is glue; a base64 '+' stays (Codex OWNER-12)


def key_material(text):
    """Line numbers of PEM private-key blocks whose payload is >= 64 base64 characters once the string
    glue of source files (quotes, escaped newlines, '+' between literals, commas, whitespace) is removed."""
    hits = []
    for m in KEYBLOCK.finditer(text):
        body = PEMHDR.sub("", m.group(2).replace("\\r", "").replace("\\n", "\n"))   # headers before the glue
        body = re.sub(r"[\"'\s,;]+", "", GLUE.sub("", body))
        if len(body) >= 64 and re.fullmatch(r"[A-Za-z0-9+/=]+", body):
            hits.append(text.count("\n", 0, m.start()) + 1)
    return hits


# Matter: a QR payload is "MT:" plus base-38 text; only the CHIP specification's own examples may appear.
QR = re.compile(r"MT:[0-9A-Z.\-]{19,}")
ALLOWED_QR = {"MT:Y.K9042C00KA0648G00", "MT:-24J0AFN00KA0648G00"}
# A manual pairing code: 11 digits (also 4-3-4) or 21, built as the Matter specification prescribes (5.1.4.1: the
# discriminator and passcode chunks, a valid passcode, the Verhoeff check digit). Only the CHIP example code is allowed;
# a test value such as 12345678901 is no code (its passcode is out of range) and stays allowed.
MANUAL = re.compile(r"(?<![\w-])(\d{4}-?\d{3}-?\d{4}|\d{21})(?![\w-])")
ALLOWED_MANUAL = {"34970112332"}
INVALID_PINS = {0, 11111111, 22222222, 33333333, 44444444, 55555555, 66666666, 77777777, 88888888, 99999999,
                12345678, 87654321}
VERHOEFF_D = [[0, 1, 2, 3, 4, 5, 6, 7, 8, 9], [1, 2, 3, 4, 0, 6, 7, 8, 9, 5], [2, 3, 4, 0, 1, 7, 8, 9, 5, 6],
              [3, 4, 0, 1, 2, 8, 9, 5, 6, 7], [4, 0, 1, 2, 3, 9, 5, 6, 7, 8], [5, 9, 8, 7, 6, 0, 4, 3, 2, 1],
              [6, 5, 9, 8, 7, 1, 0, 4, 3, 2], [7, 6, 5, 9, 8, 2, 1, 0, 4, 3], [8, 7, 6, 5, 9, 3, 2, 1, 0, 4],
              [9, 8, 7, 6, 5, 4, 3, 2, 1, 0]]
VERHOEFF_P = [[0, 1, 2, 3, 4, 5, 6, 7, 8, 9], [1, 5, 7, 6, 2, 8, 3, 0, 9, 4], [5, 8, 0, 3, 7, 9, 6, 1, 4, 2],
              [8, 9, 1, 6, 0, 4, 3, 5, 2, 7], [9, 4, 5, 3, 1, 2, 6, 8, 7, 0], [4, 2, 8, 6, 5, 7, 3, 9, 0, 1],
              [2, 7, 9, 3, 8, 0, 6, 4, 1, 5], [7, 0, 4, 6, 9, 1, 3, 2, 5, 8]]


def pairing_code(digits):
    """True when digits (11 or 21 of them, no dashes) form a well-formed Matter manual pairing code."""
    c = 0
    for i, ch in enumerate(reversed(digits)):
        c = VERHOEFF_D[c][VERHOEFF_P[i % 8][int(ch)]]
    first, chunk2, chunk3 = int(digits[0]), int(digits[1:6]), int(digits[6:10])
    pin = (chunk3 << 14) | (chunk2 & 0x3FFF)
    ids_ok = len(digits) == 11 or (int(digits[10:15]) <= 0xFFFF and int(digits[15:20]) <= 0xFFFF)   # vendor, product
    return (c == 0 and (4 <= first <= 7) == (len(digits) == 21) and first <= 7 and chunk2 <= 0xFFFF
            and chunk3 <= 0x1FFF and ids_ok and 0 < pin < 99999999 and pin not in INVALID_PINS)


# The identity header (idf/components/orcon_matter/placeholder/orcon_matter_creds.h): only zero bytes and values.
CREDS_ARRAY = re.compile(r"\bk(?:DacPrivate|DacPublic|DacCert|PaiCert|Cd|Verifier|Salt)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}")
CREDS_VALUE = re.compile(r"\bk(?:Passcode|ManualCode|QrCode)\b[^=;\n!<>]*=(?!=)\s*(?:\"[^\"\n]+\"|[1-9])")
REAL_BYTES = re.compile(r"0[xX]0*[1-9a-fA-F]|\b[1-9]")
# Files that never belong here: a device identity, pairing codes, firmware images and maps, keys and certificates.
FORBIDDEN_FILE = re.compile(r"(^|/)(identity\.defaults|KOPPELCODE\.txt|sdkconfig)$|\.(bin|elf|map|orconfw|der|pem|p12|pfx|key)$")
CREDS_FILE = re.compile(r"(^|/)orcon_matter_creds\.h$")
PLACEHOLDER = "idf/components/orcon_matter/placeholder/orcon_matter_creds.h"
SELF = "tools/check_privacy.py"


def path_problems(f):
    out = []
    if f.endswith(".log") and not (f.startswith("test/fixtures/") and f.split("/")[-1].startswith("synthetic-")):
        out.append("fixture logs must be synthetic (synthetic-*.log)" if f.startswith("test/fixtures/") else
                   "raw logs stay local (only synthetic fixtures: test/fixtures/**/synthetic-*.log)")
    if FORBIDDEN_FILE.search(f):
        out.append("never committed: a device identity, pairing code, firmware image, key or certificate file")
    if CREDS_FILE.search(f) and f != PLACEHOLDER:
        out.append(f"a Matter identity header (only {PLACEHOLDER})")
    return out


def line_problems(line):
    """The problems of one line: a file line, an added line of a commit or a line of a commit message."""
    out = []
    for m in ID.finditer(line):
        if m.group(0) not in ALLOWED_IDS and int(m.group(2)) > 999:
            out.append(f"device ID {m.group(1)}:****** is not synthetic")
    for m in MAC.finditer(line):
        if not m.group(0).lower().startswith("02:00:00:"):
            out.append("MAC address (use 02:00:00:xx:xx:xx)")
    for m in IP.finditer(line):
        ip = m.group(0)
        if ip not in ALLOWED_IPS and not ip.startswith(("192.0.2.", "198.51.100.", "203.0.113.")):
            if all(0 <= int(p) <= 255 for p in ip.split(".")):
                out.append("IPv4 address (use 192.0.2.x)")
    if HOME.search(line):
        out.append("personal home path (use $HOME or a relative path)")
    for m in QR.finditer(line):
        if m.group(0) not in ALLOWED_QR:
            out.append("Matter QR payload (only the CHIP specification's examples)")
    for m in MANUAL.finditer(line):
        digits = m.group(1).replace("-", "")
        if digits not in ALLOWED_MANUAL and pairing_code(digits):
            out.append("Matter pairing code (only the CHIP example code)")
    return out


def text_problems(text):
    """(line, problem) of the rules that span lines: private keys and the identity header's fields."""
    out = [(n, "private key material (never commit a private key)") for n in key_material(text)]
    for m in CREDS_ARRAY.finditer(text):
        if REAL_BYTES.search(m.group(1)):
            out.append((text.count("\n", 0, m.start()) + 1, "Matter identity bytes (only the zero placeholder)"))
    for m in CREDS_VALUE.finditer(text):
        out.append((text.count("\n", 0, m.start()) + 1, "Matter identity value (only the zero placeholder)"))
    return out


def git(*args):
    return subprocess.run(["git", "-c", "core.quotePath=false", *args], capture_output=True, text=True,
                          errors="replace", check=True).stdout


def history(rng):
    """Every commit of BASE..HEAD: its added files, its added lines (at their line in the new file) and its message."""
    base, _, head = rng.partition("..")
    commits = git("rev-list", "--reverse", f"{base}..{head or 'HEAD'}").split()
    found = []
    for c in commits:
        tag = c[:12]
        show = ("show", "--format=", "--no-color", "--no-ext-diff", "--diff-merges=first-parent")
        for f in git(*show, "--name-only", "--diff-filter=ACR", c).splitlines():
            if f and f != SELF:
                found += [f"{tag} {f}: {p}" for p in path_problems(f)]
        added, path, at = {}, None, 0
        for line in git(*show, "--unified=0", c).splitlines():
            if line.startswith("+++ "):
                path = line[6:] if line.startswith("+++ b/") else None
            elif line.startswith("@@"):
                m = re.search(r"\+(\d+)", line)
                at = int(m.group(1)) if m else 0
            elif line.startswith("+") and path and path != SELF:
                added.setdefault(path, []).append((at, line[1:]))
                at += 1
        for f, rows in added.items():
            for n, p in text_problems("\n".join(t for _, t in rows)):
                found.append(f"{tag} {f}:{rows[n - 1][0]}: {p}")
            for at, t in rows:
                found += [f"{tag} {f}:{at}: {p}" for p in line_problems(t)]
        for n, line in enumerate(git("log", "-1", "--format=%B", c).splitlines(), 1):
            found += [f"{tag} message:{n}: {p}" for p in line_problems(line)]
    return commits, found


args = sys.argv[1:]
problems = []
if args[:1] == ["--history"]:
    if len(args) != 2:
        sys.exit("usage: check_privacy.py --history BASE[..HEAD]")
    commits, problems = history(args[1])
    what = f"{len(commits)} commits ({args[1]})"
else:
    files = args or git("ls-files").splitlines()
    for f in files:
        if f == SELF:
            continue
        problems += [f"{f}: {p}" for p in path_problems(f)]
        try:
            text = open(f, encoding="utf-8", errors="replace").read()
        except (IsADirectoryError, FileNotFoundError):
            continue
        problems += [f"{f}:{n}: {p}" for n, p in text_problems(text)]
        for n, line in enumerate(text.splitlines(), 1):
            problems += [f"{f}:{n}: {p}" for p in line_problems(line)]
    what = f"{len(files)} files"
for p in problems:
    print("PRIVACY:", p)
print(f"checked {what}: {'OK' if not problems else str(len(problems)) + ' problem(s)'}")
sys.exit(1 if problems else 0)
