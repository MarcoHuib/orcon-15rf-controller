"""check_elf_tx.py <firmware.elf> <env> <toolchain-bin> — offline TX reachability in a V2 ELF.

RadioLib's transmit methods stay linked through vtables, so symbol presence alone says
nothing. This resolves the direct-call graph: call0/4/8/12 targets and l32r literal-pool
words that hold a function address (Xtensa long calls and function pointers), and requires:
  - no function outside RadioLib reaches a RadioLib transmit method through that graph;
  - radio::sendFrame is not linked, or reaches no RadioLib code at all;
  - dry-run: txpoc::DryRunBackend is the ONLY txpoc Backend vtable and reaches no radio/SPI;
  - one-shot RF test: only Sx1276Backend, and transmit is reached ONLY via
    txpoc::Sx1276Backend::start -> radio::sx1276::txBegin;
    RX-only: no txpoc code linked.
Limitation, printed in the report: register-indirect virtual calls (vtable slot loads) are
not resolved; the vtables holding transmit methods and the functions that load them are listed.
"""
import re
import struct
import subprocess
import sys
from pathlib import Path

RADIOLIB = re.compile(r'^(SX127\d|SX127x|PhysicalLayer|Module|RadioLibHal|ArduinoHal)::')
TX = re.compile(r'^(SX127\d|SX127x|PhysicalLayer)::\w*[Tt]ransmit\w*\(')
# Radio drivers and the SPI peripheral (Arduino spiXxx HAL, IDF spi_device/bus), not spi_flash_* cache checks.
RADIO_OR_SPI = re.compile(r'^(SX127\d|SX127x|PhysicalLayer|Module|RadioLibHal|ArduinoHal|SPIClass)::|^spi[A-Z]|^(spi_device|spi_bus|spicommon)_')


def elf_reader(path):
    data = Path(path).read_bytes()
    assert data[:4] == b'\x7fELF' and data[4] == 1 and data[5] == 1, 'not a 32-bit little-endian ELF'
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2e)
    secs = []
    for i in range(shnum):
        _, stype, _, addr, off, size = struct.unpack_from('<IIIIII', data, shoff + i * shentsize)
        if stype != 8 and addr and size:   # 8 = NOBITS
            secs.append((addr, off, size))

    def word(vaddr):
        for addr, off, size in secs:
            if addr <= vaddr and vaddr + 4 <= addr + size:
                return struct.unpack_from('<I', data, off + vaddr - addr)[0]
        return None
    return word


def closure(start, edges):
    seen, todo = set(start), list(start)
    while todo:
        for nxt in edges.get(todo.pop(), ()):
            if nxt not in seen:
                seen.add(nxt); todo.append(nxt)
    return seen


def analyse(elf, env, bindir):
    tool = lambda name: str(Path(bindir) / f'xtensa-esp32-elf-{name}')
    word = elf_reader(elf)
    funcs, objects = {}, {}
    nm = subprocess.run([tool('nm'), '-C', '-S', '--defined-only', elf], check=True, capture_output=True, text=True).stdout
    for line in nm.splitlines():
        m = re.match(r'^([0-9a-f]{8}) ([0-9a-f]{8}) (\w) (.+)$', line)
        if not m:
            continue
        addr, size, kind, name = int(m[1], 16), int(m[2], 16), m[3], m[4]
        (funcs if kind in 'TtWw' else objects)[addr] = (name, size) if kind not in 'TtWw' else name
    edges, refs, unresolved, current = {}, {}, 0, None
    dis = subprocess.Popen([tool('objdump'), '-d', '-C', '--no-show-raw-insn', elf], stdout=subprocess.PIPE, text=True)
    for line in dis.stdout:
        head = re.match(r'^([0-9a-f]{8}) <(.+)>:$', line)
        if head:
            current = funcs.get(int(head[1], 16), head[2]); continue
        ins = re.match(r'^\s*[0-9a-f]+:\s+(\w+(?:\.\w+)?)\s+(.*)$', line)
        if not ins or current is None:
            continue
        op, arg = ins[1], ins[2]
        target = None
        if op in ('call0', 'call4', 'call8', 'call12'):
            target = int(arg.split()[0], 16)
        elif op == 'l32r':
            lit = re.search(r'([0-9a-f]{8}) <', arg)
            target = word(int(lit[1], 16)) if lit else None
            if target is not None and target not in funcs:
                for base, (name, size) in objects.items():
                    if base <= target < base + max(size, 1):
                        refs.setdefault(name, set()).add(current)
                target = None
        elif op.startswith('callx'):
            unresolved += 1
        if target in funcs:
            edges.setdefault(current, set()).add(funcs[target])
    if dis.wait():
        raise RuntimeError('objdump failed')

    names = set(funcs.values())
    tx = sorted(n for n in names if TX.match(n))
    reverse = {}
    for caller, callees in edges.items():
        for callee in callees:
            reverse.setdefault(callee, set()).add(caller)
    reaching = closure(tx, reverse) - set(tx)
    outside = sorted(n for n in reaching if not RADIOLIB.match(n))
    # The one-shot RF test image has exactly one approved path to the air; every other env none.
    RF_PATH = {'radio::sx1276::txBegin(unsigned char const*, unsigned int)', 'txpoc::Sx1276Backend::start(unsigned int)'}
    allowed = RF_PATH if env == 'heltec_v2_sx1276_rftest' else set()
    problems = [f'non-RadioLib function reaches transmit: {n}' for n in outside if n not in allowed]
    send = [n for n in names if n.startswith('radio::sendFrame(')]
    if send:
        leak = sorted(n for n in closure(send, edges) if RADIOLIB.match(n))
        problems += [f'radio::sendFrame reaches RadioLib: {n}' for n in leak]
    vt_tx = {}
    for addr, (name, size) in objects.items():
        if name.startswith('vtable for '):
            held = {funcs.get(word(a)) for a in range(addr, addr + size, 4)} & set(tx)
            if held:
                vt_tx[name] = sorted(held)
    poc = sorted(n for n in names if 'txpoc::' in n)
    backends = sorted(n for _, (n, _) in objects.items() if n.startswith('vtable for txpoc::'))
    if env == 'heltec_v2_sx1276':
        problems += [f'RX-only image links TX-PoC code: {n}' for n in poc[:5]]
    elif env in ('heltec_v2_sx1276_tx_poc_dryrun', 'heltec_v2_sx1276_webctl_dryrun'):
        if backends != ['vtable for txpoc::DryRunBackend']:
            problems.append(f'txpoc Backend implementations other than DryRunBackend: {backends}')
        dry = [n for n in names if n.startswith('txpoc::DryRunBackend::')]
        if not dry:
            problems.append('DryRunBackend not linked')
        leak = sorted(n for n in closure(dry, edges) if RADIO_OR_SPI.search(n))
        problems += [f'DryRunBackend reaches radio/SPI: {n}' for n in leak]
    elif env == 'heltec_v2_sx1276_rftest':
        if backends != ['vtable for txpoc::Sx1276Backend']:
            problems.append(f'RF test image must link only Sx1276Backend: {backends}')
        if set(outside) != RF_PATH:
            problems.append(f'RF test transmit path is not exactly {sorted(RF_PATH)}: {outside}')
    else:
        problems.append(f'env {env} is not a Heltec V2 image')

    print(f'ELF {elf} env={env}: {len(funcs)} functions, {sum(map(len, edges.values()))} resolved call/ref edges, '
          f'{unresolved} register-indirect calls NOT resolved')
    print(f'RadioLib transmit methods linked ({len(tx)}): ' + '; '.join(tx))
    print(f'direct callers reaching them ({len(reaching)}, all must be RadioLib): ' + '; '.join(sorted(reaching)))
    for name, held in sorted(vt_tx.items()):
        print(f'{name} holds {len(held)} transmit slot(s); loaded by: ' + '; '.join(sorted(refs.get(name, ())) or ['-']))
    print('radio::sendFrame: ' + ('linked, no RadioLib reachable' if send else 'not linked (dead code removed)'))
    print(f'txpoc symbols: {len(poc)}; txpoc vtables: {backends}')
    for p in problems:
        print('PROBLEM: ' + p)
    print('ELF TX reachability ' + ('OK (direct graph)' if not problems else 'FAILED'))
    return not problems


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit('usage: check_elf_tx.py <firmware.elf> <env> <toolchain-bin>')
    sys.exit(0 if analyse(*sys.argv[1:]) else 1)
