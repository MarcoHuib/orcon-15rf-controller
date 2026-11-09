"""Local-only phase 1 source guards and negative capability compiles. No hardware/network."""
from pathlib import Path
import argparse, configparser, re, subprocess, textwrap

p=argparse.ArgumentParser()
p.add_argument('--compiler',default='g++')
a=p.parse_args()

def run(args,**kw):
    return subprocess.run(args, text=True, capture_output=True, timeout=30, **kw)

def sx_safe(source):
    sx=source.split('#error "SX1276 backend is receive-only',1)[1].split('#endif // RADIO_SX1276',1)[0]
    assert all(x not in sx for x in ('g_radio.transmit','startTransmit','transmitDirect','RADIOLIB_SX127X_TX','getRSSI()'))
    body=sx.split('bool sendFrame(const ramses::Frame& f) {',1)[1].split('bool poll(',1)[0]
    assert 'return false;' in body and 'TX blocked: RX-only firmware' in body

radio=Path('src/ramses_radio.cpp').read_text()
sx_safe(radio)
for inserted in ('g_radio.transmit(', 'startTransmit(', 'transmitDirect(', 'getRSSI()'):
    mutated=radio.replace('    (void)f;', '    (void)f; '+inserted,1)
    try: sx_safe(mutated)
    except AssertionError: pass
    else: raise AssertionError('guard accepted mutation '+inserted)
# Execute the EXISTING guard body locally, not the workflow itself.
workflow=Path('.github/workflows/heltec-v2-rx-only.yml').read_text()
section=workflow.split('      - name: Verify listen-only safety guards\n',1)[1]
code=section.split("python3 - <<'PY'\n",1)[1].split('\n          PY',1)[0]
exec(compile(textwrap.dedent(code),'existing-guards-executed-locally','exec'))

for path in list(Path('src').glob('tx_poc*.cpp'))+list(Path('src').glob('tx_poc*.h')):
    text=path.read_text()
    for bad in ('RadioLib.h','SPI.h','HardwareSerial.h','startTransmit(', 'transmitDirect(', '.transmit('):
        assert bad not in text,(path,bad)
assert radio.count('txpoc::serviceOwner(')==1
owner=radio.split('static void rxTask(void*) {',1)[1].split('\nbool begin()',1)[0]
assert 'txpoc::serviceOwner(' in owner
net=Path('src/net.cpp').read_text()
routes,legacy=net.split('    char csrf[33]; pocToken(',1)[1].split('\n#else\n',1)
legacy=legacy.split('#endif // ORCON_TX_POC routes',1)[0]
on=lambda block:set(re.findall(r'server\.on\(\s*"([^"]+)"',block))
# Static route proof (runtime 403 needs hardware): exactly these PoC routes, every legacy one absent.
assert on(routes)=={'/','/api/tx-poc/state','/api/tx-poc/mode','/api/oled'},on(routes)
legacy_routes=on(legacy); assert {'/api/cmd','/api/ids','/api/wifi','/api/log','/update','/api/state'}<=legacy_routes
assert not (legacy_routes-{'/','/api/oled'})&on(routes)
assert 'server.onNotFound([](){ server.send(403,' in routes
assert 'pocAuthorize(true)' in routes.split('"/api/oled"',1)[1].split('\n',1)[0]
ready=net.split('static bool pocReady()',1)[1].split('\n}',1)[0]
assert 'txpoc::rxReady()' in ready and 'rxDiagnostics' not in ready  # RX health only via the atomic heartbeat
runtime=Path('src/tx_poc_runtime.cpp').read_text()
assert 'health.store(packHealth(now,rxHealthy),std::memory_order_release)' in runtime
assert 'health.load(std::memory_order_acquire)' in runtime
assert 'pocAuthorize(true)' in net.split('static void handlePocMode()',1)[1].split('\n}',1)[0]
assert 'radio::sendFrame' not in net.split('static void handlePocMode()',1)[1].split('\n}',1)[0]
main=Path('src/main.cpp').read_text()
assert '#if !RX_ONLY\n    orcon::tick(millis());\n#endif' in main
assert main.index('controller().consumeResult()') < main.index('while (radio::poll(f))') < main.index('controller().onFrame(') < main.index('controller().tick(')
assert Path('src/rx_only.h').read_text().count('"maskids", "showfilter", "roomidx"')==1
ini=configparser.ConfigParser(interpolation=None); ini.read('platformio.ini')
assert ini['platformio']['default_envs']=='heltec_v3_cc1101_ota' # existing default unchanged
baseline=ini['env:heltec_v2_sx1276']['build_flags']
assert 'ORCON_TX_POC' not in baseline and 'ORCON_TX_RF_ENABLE' not in baseline
assert '-DORCON_TX_RF_ENABLE=0' in ini['env:heltec_v2_sx1276_tx_poc_dryrun']['build_flags']
assert '-DORCON_TX_RF_ENABLE=1' in ini['env:heltec_v2_sx1276_tx_poc']['build_flags']

base=['RADIO_SX1276=1','BOARD_HELTEC_V2=1']
valid=[('rx-default',base,{'RX_ONLY':'1','ORCON_TX_POC':'0','ORCON_TX_RF_ENABLE':'0','MQTT_ENABLED':'1'}),
       ('dryrun',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=0'],{'RX_ONLY':'1','ORCON_TX_POC':'1','MQTT_ENABLED':'0'})]
for name,defs,wanted in valid:
    result=run([a.compiler,'-std=c++11','-I','src','-x','c++','-dM','-E','-']+['-D'+x for x in defs],input='#include "config.h"\n')
    assert result.returncode==0,(name,result.stderr)
    macros=dict(re.findall(r'^#define (\w+) (.*)$',result.stdout,re.M))
    assert all(macros.get(k)==v for k,v in wanted.items()),(name,wanted)
    print('PASS capability',name,'exit',result.returncode)
invalid=[('rf-reserved',base+['ORCON_TX_POC=1','RX_ONLY=1','ORCON_TX_RF_ENABLE=1'],'RF TX is not released'),
         ('rf-alone',['ORCON_TX_RF_ENABLE=1'],'RF TX is not released'),
         ('missing-board',['RADIO_SX1276=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
         ('legacy-unlocked',base+['ORCON_TX_POC=1','RX_ONLY=0'],'PoC requires'),
         ('nonboolean-legacy',base+['ORCON_TX_POC=1','RX_ONLY=2'],'PoC requires'),
         ('wrong-radio',['BOARD_HELTEC_V2=1','RADIO_SX1262=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
         ('dual-radio',base+['RADIO_CC1101_ASYNC=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
         ('pintest',base+['PINTEST=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
         ('invalid-capability',base+['ORCON_TX_POC=2'],'capabilities must be 0 or 1')]
for name,defs,diagnostic in invalid:
    result=run([a.compiler,'-std=c++11','-I','src','-x','c++','-fsyntax-only','-']+['-D'+x for x in defs],input='#include "config.h"\n')
    assert result.returncode!=0 and diagnostic in result.stderr,(name,result.returncode,result.stderr)
    print('PASS forbidden compile',name,'expected nonzero exit',result.returncode)
print('PASS local source safety, guard mutations and capability compiles')
