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
routes,rest=net.split('    char csrf[33]; pocToken(',1)[1].split('\n#elif ORCON_WEB_CONTROL\n',1)
webctl,legacy=rest.split('\n#else\n',1)
legacy=legacy.split('#endif // ORCON_TX_POC routes',1)[0]
on=lambda block:set(re.findall(r'server\.on\(\s*"([^"]+)"',block))
# Static route proof (runtime 403 needs hardware): exactly these PoC routes, every legacy one absent.
assert on(routes)=={'/','/api/tx-poc/state','/api/tx-poc/mode','/api/oled'},on(routes)
legacy_routes=on(legacy); assert {'/api/cmd','/api/ids','/api/wifi','/api/log','/update','/api/state'}<=legacy_routes
assert not (legacy_routes-{'/','/api/oled'})&on(routes)
assert 'server.onNotFound([](){ server.send(403,' in routes
# Web control: read-only monitoring only; the control image adds its settings routes, each one
# behind the web login (setupweb::authorize), nothing else.
webctl_base,ctl_block=webctl.split('#if ORCON_TX_CONTROL\n',1)
ctl_block,webctl_tail=ctl_block.split('#endif',1)
assert on(webctl_base+webctl_tail)=={'/log','/api/log','/api/state'},on(webctl_base+webctl_tail)
assert on(ctl_block)=={'/api/netcfg','/api/scan','/api/wifi','/api/oled'},on(ctl_block)
assert all('setupweb::authorize(' in l for l in ctl_block.splitlines() if 'server.on(' in l),ctl_block
# RX-only with the web login (the rollback image): settings behind the login, no identity or command route.
auth_legacy=legacy.split('#if ORCON_WEB_AUTH\n',1)[1].split('#else\n',1)[0]
assert on(auth_legacy)=={'/api/netcfg','/api/scan','/api/wifi','/api/oled'},on(auth_legacy)
assert all('setupweb::authorize(' in l for l in auth_legacy.splitlines() if 'server.on(' in l),auth_legacy
assert 'setupweb::begin(server)' in webctl
rt=Path('src/tx_poc_runtime.cpp').read_text()
# Pairing frames reach the RF backend only on the capped RF test image (ORCON_TX_PAIR).
pair_block=rt.split('#if ORCON_TX_PAIR\n',1)[1].split('#endif',1)[0]
assert 'makeBindOffer' in pair_block and 'makeBindAck' in pair_block
assert 'FrameKind::Bind' not in rt.replace(pair_block,'')
assert 'n > TX_MAX_BYTES' in radio and 'g_radio.packetMode()' in radio
assert 'pocAuthorize(true)' in routes.split('"/api/oled"',1)[1].split('\n',1)[0]
ready=net.split('static bool pocReady()',1)[1].split('\n}',1)[0]
assert 'txpoc::rxReady()' in ready and 'rxDiagnostics' not in ready  # RX health only via the atomic heartbeat
runtime=Path('src/tx_poc_runtime.cpp').read_text()
assert re.search(r'health.store\(packHealth\(now,\s*rxHealthy\),\s*std::memory_order_release\)',runtime)
assert 'health.load(std::memory_order_acquire)' in runtime
assert 'pocAuthorize(true)' in net.split('static void handlePocMode()',1)[1].split('\n}',1)[0]
assert '#if ORCON_TX_RF_ENABLE' in net.split('static void handlePocMode()',1)[1].split('String body=',1)[0]
assert 'rf-web-control-required' in net
assert '#if ORCON_TX_RF_ENABLE' in net.split('static void handlePocRoot()',1)[1].split('server.sendHeader',1)[0]
assert 'radio::sendFrame' not in net.split('static void handlePocMode()',1)[1].split('\n}',1)[0]
main=Path('src/main.cpp').read_text()
assert '#if !RX_ONLY\n    orcon::tick(millis());\n#endif' in main
assert main.index('controller().consumeResult(millis())') < main.index('while (radio::poll(f))') < main.index('controller().onFrame(') < main.index('controller().tick(')
assert Path('src/rx_only.h').read_text().count('"maskids", "showfilter", "roomidx"')==1
# ---- Phase 2 (RF targets kept in main): equivalent replacement for the removed phase-1 #error.
# Every transmission is user-initiated and fully gated in firmware, not only by the flash tool.
def fn(text,head):                                     # body of the first function starting with head
    return text.split(head,1)[1].split('\n}\n',1)[0]
def tx_paths_safe(web,net,radio,runtime,main):
    assert web.count('controller().submitCtl(')==1 and 'submitCtl' not in net+main
    send=fn(web,'static void handleRemoteSend() {')     # the web-control (first) definition
    order=['guard(d, K, 3)','"profile-storage-unavailable"','"profile-required"','"profile-protocol-unsupported"',
           '"profile-unit-mismatch"','"identity-conflict"','"tx-budget"','controller().submitCtl(']
    pos=[send.find(x) for x in order]
    assert all(x>=0 for x in pos) and pos==sorted(pos),('send gate order',list(zip(order,pos)))
    assert 'identityClear' in send.split('submitCtl(',1)[1] and 'txpoc::rxReady()' in send.split('submitCtl(',1)[1]
    assert net.count('controller().submit(')==1 and 'submit' not in main.replace('consumeResult','')
    poc=fn(net,'static void handlePocMode() {')
    assert poc.find('rf-web-control-required')>=0 and poc.find('rf-web-control-required')<poc.find('controller().submit(')
    assert web.count('submitBind(')==2
    press=fn(web,'static void handlePairPress() {'); tick=fn(web,'void tick(uint32_t now) {')
    assert press.find('if (!CAN_PAIR)')>=0 and press.find('if (!CAN_PAIR)')<press.find('submitBind(')
    assert 0<=press.find('budgetWaitMs(2, OFFER_ONAIR)')<press.find('submitBind(')   # room for offer + ack
    # The RF budget is enforced in the RF backend itself, before the only transmit call.
    assert 0<=runtime.find('if (txbudget::waitMs(g_budget, t, 1, est)) return false;')<runtime.find('radio::sx1276::txBegin(buf_, bytes_)')
    assert tick.find('if (CAN_PAIR &&')>=0 and tick.find('if (CAN_PAIR &&')<tick.find('submitBind(')
    assert 'CAN_PAIR = CAN_TX && CAN_RX && (!ORCON_TX_POC || SIMULATED || ORCON_TX_PAIR);' in web
    assert 'static const bool SIMULATED = !ORCON_TX_RF_ENABLE;' in web
    guard='#if defined(RADIO_SX1276) && ORCON_TX_POC && ORCON_TX_RF_ENABLE\nnamespace sx1276 {'
    rf=radio.split(guard,1)[1].split('#endif // RADIO_SX1276 && ORCON_TX_POC && ORCON_TX_RF_ENABLE',1)[0]
    assert radio.count('startTransmit(')==1 and 'startTransmit(' in rf
    be=runtime.split('#if ORCON_TX_RF_ENABLE\n',1)[1].split('#else\nstatic DryRunBackend backend;\n#endif',1)
    assert len(be)==2 and 'class Sx1276Backend' in be[0] and 'class Sx1276Backend' not in be[1]+runtime.split('#if ORCON_TX_RF_ENABLE\n',1)[0]
web=Path('src/setup_web.cpp').read_text(); main_src=Path('src/main.cpp').read_text()
tx_paths_safe(web,net,radio,runtime:=Path('src/tx_poc_runtime.cpp').read_text(),main_src)
for name,old,new in [('no-profile-gate','if(!profile){fail(409,"profile-required");return;}',''),
                     ('no-unit-gate','if(!(profile->unit==orcon::ids().fan)){fail(409,"profile-unit-mismatch");return;}',''),
                     ('auto-ack-ungated','if (CAN_PAIR && g_pair.st','if (g_pair.st'),
                     ('pairing-on-rf','(!ORCON_TX_POC || SIMULATED || ORCON_TX_PAIR)','(true)'),
                     ('second-send-path','static void handleRemoteState() { if (!authorize(false, true)) return; remoteBody(200); }','static void handleRemoteState() { txpoc::controller().submitCtl(1,4,orcon::ids(),true,true,true,0); }'),
                     ('no-budget-gate','if(const uint32_t w=txpoc::budgetWaitMs(1,MODE_ONAIR)){failRetry(429,"tx-budget",w);return;}',''),
                     ('no-pair-budget','if (const uint32_t w = txpoc::budgetWaitMs(2, OFFER_ONAIR)) { failRetry(429, "tx-budget", w); return; }','')]:
    mutated=web.replace(old,new,1); assert mutated!=web,name
    try: tx_paths_safe(mutated,net,radio,runtime,main_src)
    except AssertionError: print('PASS TX-path guard rejects mutation',name)
    else: raise AssertionError('TX-path guard accepted mutation '+name)
ini=configparser.ConfigParser(interpolation=None); ini.read('platformio.ini')
assert ini['platformio']['default_envs']=='heltec_v3_cc1101_ota' # existing default unchanged
baseline=ini['env:heltec_v2_sx1276']['build_flags']
assert 'ORCON_TX_POC' not in baseline and 'ORCON_TX_RF_ENABLE' not in baseline
assert '-DORCON_TX_RF_ENABLE=0' in ini['env:heltec_v2_sx1276_tx_poc_dryrun']['build_flags']
assert '-DORCON_TX_RF_ENABLE=1' in ini['env:heltec_v2_sx1276_tx_poc']['build_flags']
# RF pairing is compiled into exactly two environments: the capped RF test image and the control image.
assert [n for n in ini.sections() if 'ORCON_TX_PAIR=1' in ini[n].get('build_flags','')]==['env:heltec_v2_sx1276_rftest','env:heltec_v2_sx1276_control']
assert [n for n in ini.sections() if 'ORCON_TX_CONTROL=1' in ini[n].get('build_flags','')]==['env:heltec_v2_sx1276_control']
assert 'MAX_PER_BOOT' not in ini['env:heltec_v2_sx1276_control']['build_flags']

base=['RADIO_SX1276=1','BOARD_HELTEC_V2=1']
valid=[('rx-default',base,{'RX_ONLY':'1','ORCON_TX_POC':'0','ORCON_TX_RF_ENABLE':'0','MQTT_ENABLED':'1'}),
       ('dryrun',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=0'],{'RX_ONLY':'1','ORCON_TX_POC':'1','MQTT_ENABLED':'0'}),
       ('rf-web',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1'],{'RX_ONLY':'1','ORCON_TX_POC':'1','ORCON_TX_RF_ENABLE':'1','ORCON_WEB_CONTROL':'1','MQTT_ENABLED':'0'}),
       ('rf-test-cap',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_MAX_PER_BOOT=50'],{'ORCON_TX_RF_ENABLE':'1','ORCON_TX_MAX_PER_BOOT':'50','ORCON_TX_PAIR':'0','MQTT_ENABLED':'0'}),
       ('rf-test-pair',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_MAX_PER_BOOT=50','ORCON_TX_PAIR=1'],{'ORCON_TX_PAIR':'1','ORCON_TX_MAX_PER_BOOT':'50'}),
       ('control',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_PAIR=1','ORCON_TX_CONTROL=1'],
        {'ORCON_TX_CONTROL':'1','ORCON_TX_PAIR':'1','ORCON_TX_MAX_PER_BOOT':'0','ORCON_WEB_AUTH':'1','MQTT_ENABLED':'0'}),
       ('rx-default-login',base,{'ORCON_WEB_AUTH':'1','ORCON_TX_CONTROL':'0'}),
       ('other-board-no-login',['RADIO_CC1101_ASYNC=1'],{'ORCON_WEB_AUTH':'0'})]
for name,defs,wanted in valid:
    result=run([a.compiler,'-std=c++11','-I','src','-x','c++','-dM','-E','-']+['-D'+x for x in defs],input='#include "config.h"\n')
    assert result.returncode==0,(name,result.stderr)
    macros=dict(re.findall(r'^#define (\w+) (.*)$',result.stdout,re.M))
    assert all(macros.get(k)==v for k,v in wanted.items()),(name,wanted)
    print('PASS capability',name,'exit',result.returncode)
invalid=[('rf-alone',['ORCON_TX_RF_ENABLE=1'],'RF TX requires'),
         ('rf-wrong-board',['RADIO_SX1276=1','ORCON_TX_POC=1','RX_ONLY=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1'],'PoC requires'),
         ('rf-legacy-unlocked',base+['ORCON_TX_POC=1','RX_ONLY=0','ORCON_TX_RF_ENABLE=1'],'PoC requires'),
         ('rf-v3-radio',['BOARD_HELTEC_V2=1','RADIO_CC1101_ASYNC=1','ORCON_TX_POC=1','RX_ONLY=1','ORCON_TX_RF_ENABLE=1'],'PoC requires'),
         ('web-alone',['ORCON_WEB_CONTROL=1'],'Web control requires'),
         ('invalid-web',base+['ORCON_WEB_CONTROL=2'],'capabilities must be 0 or 1'),
         ('missing-board',['RADIO_SX1276=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
         ('legacy-unlocked',base+['ORCON_TX_POC=1','RX_ONLY=0'],'PoC requires'),
         ('nonboolean-legacy',base+['ORCON_TX_POC=1','RX_ONLY=2'],'PoC requires'),
         ('wrong-radio',['BOARD_HELTEC_V2=1','RADIO_SX1262=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
         ('dual-radio',base+['RADIO_CC1101_ASYNC=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
         ('pintest',base+['PINTEST=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
         ('invalid-capability',base+['ORCON_TX_POC=2'],'capabilities must be 0 or 1'),
         ('cap-without-rf',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_MAX_PER_BOOT=1'],'ORCON_TX_MAX_PER_BOOT is 0, or 1..100'),
         ('pair-without-cap',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_PAIR=1'],'ORCON_TX_PAIR requires'),
         ('pair-dryrun',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_WEB_CONTROL=1','ORCON_TX_PAIR=1'],'ORCON_TX_PAIR requires'),
         ('pair-without-web',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_TX_MAX_PER_BOOT=50','ORCON_TX_PAIR=1'],'ORCON_TX_PAIR requires'),
         ('invalid-pair',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_MAX_PER_BOOT=50','ORCON_TX_PAIR=2'],'ORCON_TX_PAIR requires'),
         ('cap-above-limit',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_TX_MAX_PER_BOOT=101'],'ORCON_TX_MAX_PER_BOOT is 0, or 1..100'),
         ('control-with-test-cap',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_PAIR=1','ORCON_TX_CONTROL=1','ORCON_TX_MAX_PER_BOOT=50'],'ORCON_TX_CONTROL is'),
         ('control-without-pair',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_CONTROL=1'],'ORCON_TX_CONTROL is'),
         ('control-dryrun',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_WEB_CONTROL=1','ORCON_TX_PAIR=1','ORCON_TX_CONTROL=1'],'ORCON_TX_PAIR requires'),
         ('control-without-login',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_PAIR=1','ORCON_TX_CONTROL=1','ORCON_WEB_AUTH=0'],'requires the web login'),
         ('login-without-signed-ota',['RADIO_CC1101_ASYNC=1','ORCON_WEB_AUTH=1'],'ORCON_WEB_AUTH needs ORCON_SIGNED_OTA')]
for name,defs,diagnostic in invalid:
    result=run([a.compiler,'-std=c++11','-I','src','-x','c++','-fsyntax-only','-']+['-D'+x for x in defs],input='#include "config.h"\n')
    assert result.returncode!=0 and diagnostic in result.stderr,(name,result.returncode,result.stderr)
    print('PASS forbidden compile',name,'expected nonzero exit',result.returncode)
bm=runtime.replace('if (txbudget::waitMs(g_budget, t, 1, est)) return false;','',1); assert bm!=runtime
try: tx_paths_safe(web,net,radio,bm,main_src)
except AssertionError: print('PASS TX-path guard rejects mutation no-backend-budget')
else: raise AssertionError('TX-path guard accepted mutation no-backend-budget')
# Codex SEC-01: the rollback-test route can undo an update, so its first act is the owner login
# check (session, CSRF, origin), and it only exists where that login exists.
def hold_safe(ota):
    body=fn(ota,'static void handleHold() {')
    assert body.lstrip().startswith('if (!setupweb::authorize(true)) return;'),'hold must authorize first'
    reg=ota.split('s.on("/api/ota/hold"',1)[0].rsplit('\n',2)[-2]
    assert reg.strip()=='#if !ORCON_TX_POC || ORCON_WEB_CONTROL',('hold registration guard',reg)
ota_src=Path('src/ota_signed.cpp').read_text(); hold_safe(ota_src)
try: hold_safe(ota_src.replace('if (!setupweb::authorize(true)) return;','',1))
except AssertionError: print('PASS hold guard rejects mutation unauthenticated-hold')
else: raise AssertionError('hold guard accepted mutation unauthenticated-hold')
print('PASS local source safety, guard mutations and capability compiles')
