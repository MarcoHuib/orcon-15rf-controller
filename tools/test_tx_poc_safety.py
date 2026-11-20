"""Local-only phase 1 source guards and negative capability compiles. No hardware/network."""
from pathlib import Path
import argparse, configparser, hashlib, os, re, subprocess

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
# The listen-only guards of the former workflow step "Verify listen-only safety guards", moved here verbatim
# (the workflow is disabled and pinned below).
def listen_only_guards():
    from pathlib import Path

    main = Path("src/main.cpp").read_text()
    radio = Path("src/ramses_radio.cpp").read_text()
    config = Path("platformio.ini").read_text()

    # the backend block is the one carrying the RX_ONLY #error (include blocks also test RADIO_SX1276)
    sx = radio.split('#error "SX1276 backend is receive-only', 1)
    assert len(sx) == 2, "SX1276 backend (with its RX_ONLY #error) not found"
    tx = sx[1].split("bool sendFrame(const ramses::Frame& f) {", 1)
    assert len(tx) == 2, "SX1276 sendFrame not found"
    tx_body = tx[1].split("bool poll(ramses::Frame& out)", 1)[0]

    assert "g_radio.transmit(" not in tx_body, "SX1276 TX path is active"
    assert "return false;" in tx_body, "SX1276 sendFrame is not blocked"
    sx_body = sx[1].split("#endif // RADIO_SX1276", 1)[0]
    for bad in ("g_radio.transmit", "startTransmit", "transmitDirect", "RADIOLIB_SX127X_TX"):
        assert bad not in sx_body, f"SX1276 backend references {bad}"
    # RadioLib FSK getRSSI() with default args = startReceive()+standby(): kills direct RX
    assert "getRSSI()" not in sx_body, "SX1276 backend calls getRSSI() without skipReceive"

    import re
    net = Path("src/net.cpp").read_text()
    rxo = Path("src/rx_only.h").read_text()
    test = Path("test/test_orcon.cpp").read_text()
    # every applyCommand key must be in the test's refused list or the UI allowlist
    body = net.split("static const char* applyCommand(JsonDocument& d) {", 1)[1].split("\n}\n", 1)[0]
    keys = set(re.findall(r'd\["(\w+)"\]', body))
    allow = set(re.findall(r'"(\w+)"', rxo.split("OK[] = {", 1)[1].split("}", 1)[0]))
    tested = set(re.findall(r'"(\w+)"', test.split("TX[] = {", 1)[1].split("};", 1)[0]))
    assert allow == {"maskids", "showfilter", "roomidx"}, f"RX-only allowlist changed: {allow}"
    assert keys - allow <= tested, f"applyCommand keys not covered by test_orcon: {keys - allow - tested}"
    assert "#if RX_ONLY\n    for (JsonPair kv" in body, "applyCommand lacks the RX-only gate"
    for frag in ("ArduinoOTA.begin()", "ArduinoOTA.handle()", 'server.on("/update"'):
        i = net.index(frag)
        assert net.rfind("#if !RX_ONLY", 0, i) > net.rfind("#endif", 0, i), f"{frag} not gated by !RX_ONLY"
    # (the log page's unit-selection check went with the page: /log is gone, asserted below)
    print("PASS: RX-only command, OTA and update gates present")
    # periodic TX: tick() is called for TX builds only (tools/check_tick.py, also used by
    # heltec_v2.sh verify), and tick() refuses itself on RX-only
    import subprocess
    subprocess.run(["python3", "tools/check_tick.py"], input=main, text=True, check=True)
    orcon_src = Path("src/orcon.cpp").read_text()
    tick_body = orcon_src.split("void tick(uint32_t now) {", 1)[1].lstrip()
    assert tick_body.startswith("if (RX_ONLY) return;"), "tick() does not refuse first on RX-only"
    assert "[env:heltec_v2_sx1276]\n" in config
    assert "platform = espressif32@6.7.0" in config
    print("PASS: SX1276 transmission path is blocked")
    # OLED power-save must stay local to the display. Radio diagnostics
    # (name/RSSI) are read-only; no task or network/radio control here.
    oled = net.split("// OLED\n", 1)[1].split("// WiFi\n", 1)[0]
    oled += Path("src/oled_power.h").read_text()
    for bad in (r"\besp_\w*sleep\w*\s*\(", r"\bvTask(?:Suspend|Delete)\s*\(",
                r"\bWiFi\.", r"\bmqtt\.", r"\bSPI\.",
                r"\bradio::(?:begin|startReceive|sendFrame|poll)\s*\("):
        assert not re.search(bad, oled), f"OLED code controls a non-display subsystem: {bad}"
    print("PASS: OLED power-save does not sleep the CPU or control RX/WiFi/MQTT")
listen_only_guards()
# Also from the workflow (flash-tooling step): check_tick.py refuses a tick() call outside #if !RX_ONLY.
mut=re.sub(r'^#if !RX_ONLY$','',Path('src/main.cpp').read_text(),flags=re.M)
assert run(['python3','tools/check_tick.py'],input=mut).returncode!=0,'check_tick.py accepted a tick() call outside #if !RX_ONLY'

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
legacy_routes=on(legacy); assert {'/api/cmd','/api/ids','/api/wifi','/update','/api/state'}<=legacy_routes
assert not (legacy_routes-{'/','/api/oled'})&on(routes)
# Phase-2 cleanup: the legacy dashboard pages, the house image and the old RF log are gone from every image.
assert not {'/dashboard','/wifi','/widget','/widget.html','/flow','/house.jpg','/debug','/log','/api/log'}&on(net),on(net)
assert 'server.onNotFound([](){ server.send(403,' in routes
# Web control: read-only monitoring only; the control image adds its settings routes, each one
# behind the web login (setupweb::authorize), nothing else.
webctl_base,ctl_block=webctl.split('#if ORCON_TX_CONTROL\n',1)
ctl_block,webctl_tail=ctl_block.split('#endif',1)
assert on(webctl_base+webctl_tail)=={'/api/state'},on(webctl_base+webctl_tail)
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
    # The mode-command gates live in ctlAdmit(), shared by the web page and Siri (docs/SIRI.md): the only
    # submitCtl() call, after every gate, and exactly two callers, each with its own authentication first.
    adm=fn(web,'static int ctlAdmit(uint8_t k, uint32_t request, const char*& e, uint32_t& waitMs) {')
    order=['"profile-storage-unavailable"','"profile-required"','"profile-protocol-unsupported"',
           '"profile-unit-mismatch"','"identity-conflict"','"tx-budget"','controller().submitCtl(']
    pos=[adm.find(x) for x in order]
    assert all(x>=0 for x in pos) and pos==sorted(pos),('send gate order',list(zip(order,pos)))
    assert 'identityClear' in adm.split('submitCtl(',1)[1] and 'txpoc::rxReady()' in adm.split('submitCtl(',1)[1]
    callers=len(re.findall(r'\bctlAdmit\s*\(',web))
    assert callers==3,('ctlAdmit callers',callers)           # the definition + the two handlers below
    assert not re.search(r'\barg\s*\(\s*"k"\s*\)',web),'a Siri key is never read from the URL'
    # Whole statements, whitespace-normalised, in this order: each gate with its early return (Codex SIRI-08:
    # tokens alone let "||" -> "&&", an ignored guard result or a weakened predicate pass). The behaviour
    # itself is tested over HTTP in test/test_setup_web.cpp (wrong key, Locked, URL key, no session).
    def in_order(body,pats,what):
        flat=re.sub(r'\s+',' ',body); at=-1
        for p in pats:
            m=re.compile(p).search(flat,at+1); assert m,(what,'missing or out of order',p); at=m.start()
    in_order(fn(web,'static void handleRemoteSend() {'),          # the web-control (first) definition
             [r'if \(!guard\(d, K, 3\)\) return;',r'ctlAdmit\(k, d\["request"\]\.as<uint32_t>\(\), e, w\)'],'web send')
    say=r'\{ siriSay\(\d{3}, "[^"]*"\); return; \}'
    in_order(fn(web,'static void handleSiri() {'),
             [r'if \(net::fromSetupAp\(\) \|\| !net::fromHomeNet\(\) \|\| !hostOk\(\)\) '+say,
              r'const provision::State st = ownerstore::state\(\);',
              r'if \(st != provision::State::Done && st != provision::State::Legacy\) '+say,
              r'const String auth = g_srv->header\("Authorization"\);',
              r'const char\* given = auth\.length\(\) > 7 && !strncasecmp\(auth\.c_str\(\), "Bearer ", 7\) \? auth\.c_str\(\) \+ 7 : "";',
              r'if \(authstore::siriKey\(key\) != authstore::Status::Ok \|\| !webauth::equal\(given, key\)\) '+say,
              r'ctlAdmit\(k, 0, e, w\)'],'siri')
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
for name,old,new in [('no-profile-gate','if(!profile){e="profile-required";return 409;}',''),
                     ('no-unit-gate','if(!(profile->unit==orcon::ids().fan)){e="profile-unit-mismatch";return 409;}',''),
                     ('auto-ack-ungated','if (CAN_PAIR && g_pair.st','if (g_pair.st'),
                     ('pairing-on-rf','(!ORCON_TX_POC || SIMULATED || ORCON_TX_PAIR)','(true)'),
                     ('second-send-path','static void handleRemoteState() { if (!authorize(false, true)) return; remoteBody(200); }','static void handleRemoteState() { txpoc::controller().submitCtl(1,4,orcon::ids(),true,true,true,0); }'),
                     ('no-budget-gate','if((waitMs=txpoc::budgetWaitMs(1,MODE_ONAIR))){e="tx-budget";return 429;}',''),
                     ('no-pair-budget','if (const uint32_t w = txpoc::budgetWaitMs(2, OFFER_ONAIR)) { failRetry(429, "tx-budget", w); return; }',''),
                     ('send-unguarded','    if (!guard(d, K, 3)) return;\n    if (!d["request"]','    if (!d["request"]'),
                     ('third-admit-caller','static void handleRemoteState() { if (!authorize(false, true)) return; remoteBody(200); }','static void handleRemoteState() { const char* e; uint32_t w; ctlAdmit(1, 0, e, w); }'),
                     ('siri-no-key','if (authstore::siriKey(key) != authstore::Status::Ok || !webauth::equal(given, key)) {','if (false) {'),
                     ('siri-any-subnet','net::fromSetupAp() || !net::fromHomeNet() || !hostOk()','net::fromSetupAp() || !hostOk()'),
                     ('siri-key-in-url','g_srv->header("Authorization")','g_srv->arg("k")'),
                     ('siri-locked-ok','if (st != provision::State::Done && st != provision::State::Legacy)','if (false)'),
                     ('siri-key-or-to-and','!= authstore::Status::Ok || !webauth::equal(given, key)','!= authstore::Status::Ok && !webauth::equal(given, key)'),
                     ('send-guard-ignored','    if (!guard(d, K, 3)) return;','    guard(d, K, 3);'),
                     ('siri-state-weakened','st != provision::State::Done && st != provision::State::Legacy','st == provision::State::New'),
                     ('siri-key-in-url-spaced','g_srv->header("Authorization")','g_srv->arg( "k" )'),
                     ('siri-no-return','siriSay(401, "De Siri-sleutel klopt niet. Kijk in de webinterface bij Systeem, Integraties.");\n        return;','siriSay(401, "De Siri-sleutel klopt niet. Kijk in de webinterface bij Systeem, Integraties.");'),
                     ('third-admit-caller-spaced','static void handleRemoteState() { if (!authorize(false, true)) return; remoteBody(200); }','static void handleRemoteState() { const char* e; uint32_t w; ctlAdmit (1, 0, e, w); }')]:
    mutated=web.replace(old,new,1); assert mutated!=web,name
    try: tx_paths_safe(mutated,net,radio,runtime,main_src)
    except AssertionError: print('PASS TX-path guard rejects mutation',name)
    else: raise AssertionError('TX-path guard accepted mutation '+name)
ini=configparser.ConfigParser(interpolation=None); ini.read('platformio.ini')
assert ini['platformio']['default_envs']=='heltec_v2_sx1276'   # Heltec V2 only; the safe default is the RX-only image
# No other radio or board back-end may come back next to the SX1276 (the PoC #error no longer lists them).
for p in sorted(Path('src').iterdir())+[Path('platformio.ini')]:
    for m in ('RADIO_SX1262','RADIO_CC1101','USE_CC1101_TX','BOARD_4848S040','PINTEST','NO_OLED'):
        assert m not in p.read_text(),(str(p),m)
# Phase-2 cleanup: no MQTT client, MQTT build switch or MQTT code (Home Assistant/Domoticz integration removed).
for p in sorted(Path('src').iterdir())+[Path('platformio.ini')]:
    for m in ('MQTT_ENABLED','PubSubClient','mqtt.'):
        assert m not in p.read_text(),(str(p),m)
assert all(n.startswith('env:heltec_v2_sx1276') for n in ini.sections() if n.startswith('env:') and n!='env:native'),ini.sections()
baseline=ini['env:heltec_v2_sx1276']['build_flags']
assert 'ORCON_TX_POC' not in baseline and 'ORCON_TX_RF_ENABLE' not in baseline
assert '-DORCON_TX_RF_ENABLE=0' in ini['env:heltec_v2_sx1276_tx_poc_dryrun']['build_flags']
assert '-DORCON_TX_RF_ENABLE=1' in ini['env:heltec_v2_sx1276_tx_poc']['build_flags']
# RF pairing is compiled into exactly two environments: the capped RF test image and the control image.
assert [n for n in ini.sections() if 'ORCON_TX_PAIR=1' in ini[n].get('build_flags','')]==['env:heltec_v2_sx1276_rftest','env:heltec_v2_sx1276_control']
assert [n for n in ini.sections() if 'ORCON_TX_CONTROL=1' in ini[n].get('build_flags','')]==['env:heltec_v2_sx1276_control']
assert 'MAX_PER_BOOT' not in ini['env:heltec_v2_sx1276_control']['build_flags']

base=['RADIO_SX1276=1','BOARD_HELTEC_V2=1']
valid=[('rx-default',base,{'RX_ONLY':'1','ORCON_TX_POC':'0','ORCON_TX_RF_ENABLE':'0'}),
       ('dryrun',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=0'],{'RX_ONLY':'1','ORCON_TX_POC':'1'}),
       ('rf-web',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1'],{'RX_ONLY':'1','ORCON_TX_POC':'1','ORCON_TX_RF_ENABLE':'1','ORCON_WEB_CONTROL':'1'}),
       ('rf-test-cap',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_MAX_PER_BOOT=50'],{'ORCON_TX_RF_ENABLE':'1','ORCON_TX_MAX_PER_BOOT':'50','ORCON_TX_PAIR':'0'}),
       ('rf-test-pair',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_MAX_PER_BOOT=50','ORCON_TX_PAIR=1'],{'ORCON_TX_PAIR':'1','ORCON_TX_MAX_PER_BOOT':'50'}),
       ('control',base+['RX_ONLY=1','ORCON_TX_POC=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1','ORCON_TX_PAIR=1','ORCON_TX_CONTROL=1'],
        {'ORCON_TX_CONTROL':'1','ORCON_TX_PAIR':'1','ORCON_TX_MAX_PER_BOOT':'0','ORCON_WEB_AUTH':'1'}),
       ('rx-default-login',base,{'ORCON_WEB_AUTH':'1','ORCON_TX_CONTROL':'0'}),
       ('no-board-no-login',[],{'ORCON_WEB_AUTH':'0'})]
for name,defs,wanted in valid:
    result=run([a.compiler,'-std=c++11','-I','src','-x','c++','-dM','-E','-']+['-D'+x for x in defs],input='#include "config.h"\n')
    assert result.returncode==0,(name,result.stderr)
    macros=dict(re.findall(r'^#define (\w+) (.*)$',result.stdout,re.M))
    assert all(macros.get(k)==v for k,v in wanted.items()),(name,wanted)
    print('PASS capability',name,'exit',result.returncode)
invalid=[('rf-alone',['ORCON_TX_RF_ENABLE=1'],'RF TX requires'),
         ('rf-wrong-board',['RADIO_SX1276=1','ORCON_TX_POC=1','RX_ONLY=1','ORCON_TX_RF_ENABLE=1','ORCON_WEB_CONTROL=1'],'PoC requires'),
         ('rf-legacy-unlocked',base+['ORCON_TX_POC=1','RX_ONLY=0','ORCON_TX_RF_ENABLE=1'],'PoC requires'),
         ('rf-missing-radio',['BOARD_HELTEC_V2=1','ORCON_TX_POC=1','RX_ONLY=1','ORCON_TX_RF_ENABLE=1'],'PoC requires'),
         ('web-alone',['ORCON_WEB_CONTROL=1'],'Web control requires'),
         ('invalid-web',base+['ORCON_WEB_CONTROL=2'],'capabilities must be 0 or 1'),
         ('missing-board',['RADIO_SX1276=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
         ('legacy-unlocked',base+['ORCON_TX_POC=1','RX_ONLY=0'],'PoC requires'),
         ('nonboolean-legacy',base+['ORCON_TX_POC=1','RX_ONLY=2'],'PoC requires'),
         ('missing-radio',['BOARD_HELTEC_V2=1','ORCON_TX_POC=1','RX_ONLY=1'],'PoC requires'),
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
         ('login-without-signed-ota',['ORCON_WEB_AUTH=1'],'ORCON_WEB_AUTH needs ORCON_SIGNED_OTA')]
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
# After an image is written and selected for the next boot, nothing reopens the writer before the restart (Codex r1):
# every way in (nonce, begin, the writer itself, the one-click download) refuses with restart-pending.
def selected_safe(ota):
    assert 'g_selected = true;\n    g_st = St::Done;' in ota,'the writer marks a selected image'
    up=fn(ota,'static void handleUploadData() {')
    assert 'if (!endWrite()) net::restartSoon();' in up and 'if (g_st != St::Done) abortWrite' in up,'a selected upload restarts even when the client is gone'
    mb=fn(ota,'static const char* manualBlocked() {')
    assert 'if (g_selected || net::restartQueued()) return "restart-pending";' in mb and 'if (setupweb::busy()) return "session-busy";' in mb,'manual steps: no restart queued, no RF/session in flight (Codex r3)'
    for h in ('static void handleNonce() {','static void handleBegin() {'):
        assert '\n'.join(fn(ota,h).split('\n')[:3]).count('if (const char* e = manualBlocked()) { fail(409, e); return; }')==1,(h,'must ask manualBlocked first')
    assert 'if (const char* e = manualBlocked())' in fn(ota,'static void handleUploadData() {'),'upload start re-checks'
    for h in ('static const char* startWrite() {','const char* claimDownload() {'):
        assert 'if (g_selected)' in '\n'.join(fn(ota,h).split('\n')[:3]),(h,'must refuse while a selected image waits for the restart')
    assert 'scrub(); }' in fn(ota,'static void abortWrite(') and fn(ota,'static const char* endWrite() {').count('scrub();')==3,'a rejected image is made unbootable'
    assert 'if (bootMismatch()) return "boot-slot-mismatch";' in mb and 'if (bootMismatch()) return "boot-slot-mismatch";' in fn(ota,'const char* claimDownload() {'),'never write the slot the bootloader still selects (Codex r4)'
    assert fn(ota,'static void handleUploadData() {').count('dropClient();')==4 and 'client().stop()' not in fn(ota,'static void handleUploadData() {'),'a refused upload is closed before its body (Codex r4)'
    assert 'if (c.fd() >= 0) shutdown(c.fd(), SHUT_RDWR);' in fn(ota,'static void dropClient() {'),'the live socket itself is shut down, not a copy of the client (Codex r6)'
    lw=re.sub(r'\s+',' ',re.sub(r'//[^\n]*','',fn(ota,'static void loopWatch(void*) {')))
    assert 'const uint32_t beat = g_beat.load(); const uint32_t still = millis() - beat; if (still <= limit) continue; const uint32_t beat2 = g_beat.load(); const uint32_t still2 = millis() - beat2; if (beat2 == beat && still2 > limit) {' in lw,'the watchdog reads the heartbeat before the clock, twice, and restarts only without a beat in between (Codex r6/r7)'
    nb=fn(Path('src/net.cpp').read_text(),'void begin() {')
    assert 'const bool backOk = !settled && ota::previousIsOwnerModel();' in nb and 'ota::rollbackNow();' in nb,'an unconfirmed update in the fail-closed boot returns only to an owner-model image (Codex r4)'
selected_safe(ota_src)
for name,old,new in (('begin-after-select','    if (g_selected || net::restartQueued()) return "restart-pending";','    if (net::restartQueued()) return "restart-pending";'),
                     ('upload-during-rf','    if (setupweb::busy()) return "session-busy";\n',''),
                     ('rejected-image-kept','esp_ota_abort(g_h); scrub(); g_st','esp_ota_abort(g_h); g_st'),
                     ('write-selected-slot','    if (bootMismatch()) return "boot-slot-mismatch";\n    if (g_st == St::Writing) return "update-busy";','    if (g_st == St::Writing) return "update-busy";'),
                     ('refused-upload-kept-open','g_err = "not-ready"; dropClient(); return;','g_err = "not-ready"; return;'),
                     ('refused-upload-copy-only','    if (c.fd() >= 0) shutdown(c.fd(), SHUT_RDWR);\n',''),
                     ('watchdog-clock-first','const uint32_t still = millis() - beat;','const uint32_t still = millis() - g_beat.load();'),
                     ('watchdog-ignores-recovery','if (beat2 == beat && still2 > limit) {','if (still2 > limit) {'),
                     ('writer-after-select','    if (g_selected) { g_st = St::Failed; return g_err = "restart-pending"; }\n',''),
                     ('download-after-select','    if (g_selected) return "restart-pending";\n',''),
                     ('no-restart-after-select','if (!endWrite()) net::restartSoon();','endWrite();')):
    m=ota_src.replace(old,new,1); assert m!=ota_src,name
    try: selected_safe(m)
    except AssertionError: print('PASS selected-image guard rejects mutation',name)
    else: raise AssertionError('selected-image guard accepted mutation '+name)
try: hold_safe(ota_src.replace('if (!setupweb::authorize(true)) return;','',1))
except AssertionError: print('PASS hold guard rejects mutation unauthenticated-hold')
else: raise AssertionError('hold guard accepted mutation unauthenticated-hold')
# Release update check (fw_update_esp.cpp, docs/UPDATECONTROLE.md): it can install firmware, so every route asks the owner's
# session first (the install and the check also CSRF + same origin), TLS always verifies against the pinned roots, nothing
# disables verification, and the download URL is never taken from the request.
def update_safe(src):
    for h,mut in (('static void handleStatus() {','authorize(false)'),('static void handleCheck() {','authorize(true)'),('static void handleInstall() {','authorize(true)')):
        assert fn(src,h).lstrip().startswith('if (!setupweb::authorize(%s)) return;'%mut.split('(')[1].rstrip(')')),(h,'must ask the owner first')
    assert src.count('c.connect(ip, 443, host, FWUP_ROOTS_PEM, nullptr, nullptr)')==1,'TLS must verify against the pinned roots'
    allsrc='\n'.join(x.read_text() for x in Path('src').glob('*.[ch]*'))
    for bad in ('setInsecure','MBEDTLS_SSL_VERIFY_NONE','MBEDTLS_SSL_VERIFY_OPTIONAL','setCACertBundle','HTTPUpdate'):
        assert bad not in allsrc,('forbidden in the firmware',bad)   # (Update.begin: legacy non-V2 /update in net.cpp; the image scanner refuses it)
    body=fn(src,'static void handleInstall() {')
    assert 'arg("url")' not in body and 'arg("plain")' in body and 'g_offer.m.version == want' in body,'install: only the offered version, never a URL'
    assert body.index('if (g_running)') < body.index('g_offer.m.version == want') and 'startJob(Job::Install, false, want, commit)' in body,'install: the approved offer cannot change underneath'
    assert 'g_offer.m.version == want && !strcmp(g_offer.m.commit, commit)' in body,'install: the build the owner saw (version and commit)'
    assert 'same = g_st.found == fwup::Found::Newer && g_offer.m.version == want && !strcmp(g_offer.m.commit, commit)' in fn(src,'static const char* startJob('),'startJob re-checks the approved build'
    assert 'ota::claimDownload()' in src and 'ota::releaseSink()' in src,'install must use the signed-OTA writer'
    wifi=fn(Path('src/net.cpp').read_text(),'static void handleWifiSave() {')
    assert wifi.lstrip().startswith('if (updsvc::busy())'),'saving WiFi (it reboots) waits for an install, on every build (Codex r3/r5)'
    prg=fn(Path('src/net.cpp').read_text(),'static void prgPoll(')
    assert 'if (updsvc::busy()) {' in prg,'the PRG menu (it restarts) waits for an install (Codex r5)'
    assert 'heartbeat();' in fn(ota_src,'void tick(uint32_t now) {') and 'xTaskCreatePinnedToCore(loopWatch' in ota_src,'the loop watchdog runs and is fed by the loop (Codex r5)'
    assert 'if (g_restart || net::restartQueued()) return "restart-pending";' in fn(src,'static const char* startJob('),'no new job while a restart is queued'
    assert 'ota::writerBusy()' in fn(src,'bool busy() {'),'a manual upload blocks RF commands too'
upd=Path('src/fw_update_esp.cpp').read_text(); update_safe(upd)
for name,old,new in (('status-without-login','if (!setupweb::authorize(false)) return; statusBody(200);','statusBody(200);'),
                     ('install-without-login','static void handleInstall() {\n    if (!setupweb::authorize(true)) return;','static void handleInstall() {'),
                     ('check-without-csrf','static void handleCheck() {\n    if (!setupweb::authorize(true)) return;','static void handleCheck() {\n    if (!setupweb::authorize(false)) return;'),
                     ('tls-without-roots','c.connect(ip, 443, host, FWUP_ROOTS_PEM, nullptr, nullptr)','c.connect(ip, 443)'),
                     ('install-from-url','g_offer.m.version == want','g_s->arg("url").length()'),
                     ('install-any-offer','startJob(Job::Install, false, want, commit)','startJob(Job::Install, false)'),
                     ('install-any-commit','LOCKED(offered = g_st.found == fwup::Found::Newer && g_offer.m.version == want && !strcmp(g_offer.m.commit, commit));','LOCKED(offered = g_st.found == fwup::Found::Newer && g_offer.m.version == want);'),):
    m=upd.replace(old,new,1); assert m!=upd,name
    try: update_safe(m)
    except AssertionError: print('PASS update guard rejects mutation',name)
    else: raise AssertionError('update guard accepted mutation '+name)
print('PASS update check: owner session first, pinned TLS roots, no insecure TLS, signed-OTA writer only')
# Workflows: none runs today (builds, tests and signing are local), but Actions are not banned for good. Every
# workflow file is pinned by content, because text checks on YAML can be bypassed (blank lines, if: false,
# continue-on-error, a second job, ...). The old RX-only workflow is pinned as an inert stub (manual start only,
# no permissions, no action, stops at once), so changing it fails this test. This runs locally (before every push)
# on the working copy and on HEAD's committed tree; GitHub does not consult it, and a manual run on a ref from before
# the stub uses the old file. A new workflow, such as a
# future release build, is added deliberately: first its own security checks here (least permissions; the OTA
# signing key only in the release-signing job, from a protected GitHub Environment secret, never in the repository
# or in push, PR or test workflows; nothing installed on a board without the owner's consent), then name and hash.
# The official release workflow (release-control.yml, docs/RELEASES.md) is the one workflow that runs. Besides its pin, its
# security properties are checked here on the parsed file, and every check has a mutation that must be refused: the pin
# says "changed", these say WHAT would be wrong (a trigger, a secret outside the signing job, a write token, an
# unpinned action, an expression in a script, another image, a skipped check, hardware access, ...).
def yaml_subset(text):
    assert text.isascii(),'non-ASCII in the workflow (e.g. a no-break space that this reader would strip; Codex r5)'
    """A strict reader for the YAML the release workflow uses: block mappings and sequences, '|' block scalars, plain
    and quoted scalars, {} and [a, b] flow items. Anything else (anchors, tags, folded scalars, odd indentation,
    duplicate keys) raises AssertionError: fail closed."""
    assert not re.search('[\r\x0b\x0c\x85\u2028\u2029]',text.replace('\r\n','\n')),'a line separator other than LF/CRLF (YAML would break the line there)'
    lines=text.replace('\r\n','\n').split('\n')
    ind=lambda l: len(l)-len(l.lstrip(' '))
    def nocomment(s):
        out,q='',None
        for i,c in enumerate(s):
            if q:
                out+=c
                if c==q: q=None
            elif c in '\'"' and (not out.strip() or out.rstrip().endswith((':','-','[',','))): q=c; out+=c
            elif c=='#' and (i==0 or s[i-1] in ' \t'): break
            else: out+=c
        assert q is None,('unterminated quote',s)
        return out.rstrip()
    def scalar(s):
        s=s.strip()
        if s=='{}': return {}
        if s.startswith('[') and s.endswith(']'): return [scalar(x) for x in s[1:-1].split(',') if x.strip()]
        if len(s)>=2 and s[0]==s[-1] and s[0] in '\'"':
            assert not (s[0]=='"' and '\\' in s),('escape sequence in a double-quoted YAML scalar (GitHub decodes it; this reader does not)',s)
            assert not (s[0]=="'" and "''" in s[1:-1]),("'' in a single-quoted YAML scalar (this reader does not decode it)",s)
            return s[1:-1]
        assert s and not s.startswith(('&','*','!','{','[','>','|','@','`')),('unsupported YAML value',s)
        return True if s=='true' else False if s=='false' else s
    def skip(i):
        while i<len(lines) and (not lines[i].strip() or lines[i].lstrip().startswith('#')): i+=1
        return i
    def node(i,parent):
        i=skip(i)
        if i>=len(lines) or ind(lines[i])<=parent: return None,i
        return (seq if lines[i].lstrip().startswith('- ') else mapping)(i,ind(lines[i]))
    def mapping(i,n):
        d={}
        while True:
            i=skip(i)
            if i>=len(lines) or ind(lines[i])<n: return d,i
            assert ind(lines[i])==n and '\t' not in lines[i][:n],('bad indentation',lines[i])
            line=nocomment(lines[i].strip())
            m=re.fullmatch(r'([A-Za-z_][\w.-]*):(?: (.*))?',line)
            assert m and not line.startswith('- '),('not a mapping entry',line)
            k,v=m.group(1),(m.group(2) or '').strip()
            assert k not in d,('duplicate key',k)
            if v=='|':
                i+=1; body=[]
                while i<len(lines) and (not lines[i].strip() or ind(lines[i])>n): body.append(lines[i]); i+=1
                while body and not body[-1].strip(): body.pop()
                bi=min(ind(l) for l in body if l.strip())
                d[k]='\n'.join(l[bi:] for l in body)+'\n'
            elif v: d[k]=scalar(v); i+=1
            else: d[k],i=node(i+1,n)
    def seq(i,n):
        out=[]
        while True:
            i=skip(i)
            if i>=len(lines) or ind(lines[i])<n: return out,i
            assert ind(lines[i])==n and lines[i].lstrip().startswith('- '),('bad sequence item',lines[i])
            rest=lines[i][n+2:]
            if re.match(r'[A-Za-z_][\w.-]*:( |$)',rest):
                lines[i]=' '*(n+2)+rest; v,i=mapping(i,n+2)
            else: v=scalar(nocomment(rest)); i+=1
            out.append(v)
    doc,i=mapping(0,0)
    assert skip(i)==len(lines),('unparsed YAML at line',i+1)
    return doc
REL_GUARD="github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/main' && github.repository == 'MarcoHuib/orcon-15rf-controller'"
REL_ACTIONS={'actions/checkout':'3d3c42e5aac5ba805825da76410c181273ba90b1','actions/setup-python':'6d4fd22b20465ccc0f7354476588e8df7c9ca0e0',   # v7.0.1, v7.1.0
             'actions/upload-artifact':'cf430e030ddbb5b0abf93d22962f4752f3646cd9','actions/download-artifact':'9000827ccba6bdab643e8b6fd33ac0654aef8333'}   # v7.0.2, v8.0.2
REL_EXPR={'github.event.repository.default_branch','github.event_name','github.ref','github.repository','github.run_id','github.server_url',
          'github.sha','github.token','inputs.notes','inputs.version','needs.build.outputs.firmware_sha256','needs.prepare.outputs.number',
          'needs.prepare.outputs.tag','needs.sign.outputs.bundle_name','needs.sign.outputs.bundle_sha256','runner.temp',
          'secrets.ORCON_OTA_SIGNING_KEY','steps.check.outputs.firmware_sha256','steps.package.outputs.bundle_name',
          'steps.package.outputs.bundle_sha256','steps.version.outputs.name','steps.version.outputs.number','steps.version.outputs.tag',
          'needs.verify.outputs.sums_sha256','needs.verify.outputs.notes_sha256','steps.final.outputs.sums_sha256','steps.final.outputs.notes_sha256'}
# Every check the release depends on, as its own exact script line (Codex r4: a prefix let "--help" or a wrapper pass).
REL_REQUIRED=(('prepare','python3 -I tools/release_ci.py source --repo "$REPO" --ref "$REF" --event "$EVENT" --default-branch "$DEFAULT_BRANCH" --sha "$SHA" --head "$head"'),
    ('prepare','python3 -I tools/release_ci.py version "$VERSION" --tags "$RUNNER_TEMP/tags.txt" --notes="$NOTES" >> "$GITHUB_OUTPUT"'),
    ('build','python -m venv "$RUNNER_TEMP/pio-venv"'),
    ('build','"$RUNNER_TEMP/pio-venv/bin/python" -m pip install --require-hashes --no-deps --only-binary=:all: --force-reinstall -r tools/release/requirements-platformio.txt'),
    ('build','"$RUNNER_TEMP/pio-venv/bin/python" -m pip check'),('build','echo "$RUNNER_TEMP/pio-venv/bin" >> "$GITHUB_PATH"'),
    ('build','python -I tools/release_ci.py packages --list "$RUNNER_TEMP/packages.txt"'),('build','bash test/run_host_tests.sh'),
    ('build','python tools/test_tx_poc_safety.py'),('build','git ls-files -z | xargs -0 python tools/check_privacy.py'),
    ('build','python tools/test_orcon_cli.py'),
    ('build','python -I tools/release_ci.py verify-build --build ".pio/build/$RELEASE_ENV" --version "$NUMBER" --commit "$SHA" --harness "$RUNNER_TEMP/harness/release_accept" --nm "$tc/xtensa-esp32-elf-nm" --out dist --esptool "$HOME/.platformio/packages/tool-esptoolpy/esptool.py"'),
    ('build','python -I tools/check_elf_tx.py ".pio/build/$RELEASE_ENV/firmware.elf" "$RELEASE_ENV" "$tc"'),
    ('build','RELEASE_BUILD_DIR=".pio/build/$RELEASE_ENV" RELEASE_VERSION="$NUMBER" RELEASE_COMMIT="$SHA" python tools/test_release_ci.py'),
    ('build','python -I tools/release_ci.py manifest --dist dist'),('sign','python3 -I tools/release_ci.py manifest --dist dist'),
    ('sign','python3 -I tools/release_ci.py package --dist dist --notes="$NOTES"'),
    ('verify','python3 -I tools/release_ci.py verify-package --dist dist --harness harness/release_accept --official --notes="$NOTES"'),
    ('publish','test "$(sha256sum SHA256SUMS | cut -d\' \' -f1)" = "$SUMS"'),('publish','test "$(sha256sum RELEASE_NOTES.md | cut -d\' \' -f1)" = "$NOTES_SHA"'),
    ('publish','sha256sum -c SHA256SUMS'),('publish','if [ "$floor" -ge "$NUMBER" ]; then'),
    ('publish','floor="$(awk \'/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)$/ { sub(/^v/, ""); split($0, p, "."); n = p[1] * 1000000 + p[2] * 1000 + p[3]; if (n > m) m = n } END { print m + 0 }\' "$RUNNER_TEMP/tags.txt")"'),
    ('publish','cmp <(ls "$RUNNER_TEMP/check" | sort) <( (printf \'%s\\n\' "${files[@]}"; echo SHA256SUMS) | sort)'),
    ('prepare','git ls-remote --tags "https://github.com/${REPO}.git" | cut -f2 | sed -n \'s#^refs/tags/##p\' | sed \'s/\\^{}$//\''),
    ('publish','git ls-remote --tags "https://github.com/${REPO}.git" | cut -f2 | sed -n \'s#^refs/tags/##p\' | sed \'s/\\^{}$//\''),
    ('publish','gh api -X POST "repos/${REPO}/git/refs" -f "ref=refs/tags/${TAG}" -f "sha=${SHA}" > /dev/null'),
    ('publish','printf \'%s\' "$id" | grep -qxE \'[0-9]+\''),
    ('publish','for f in "${files[@]}" SHA256SUMS; do cmp -- "$f" "$RUNNER_TEMP/check/$f"; done'),
    ('publish','gh api -X PATCH "repos/${REPO}/releases/${id}" -F draft=false -f make_latest=true -F "body=@RELEASE_NOTES.md" > /dev/null'))
# The only script lines that start with shell control syntax (a wrapper like "if false; then" around a check, or a block
# that swallows a failure, is refused); and constructs that could redefine or disable a command.
REL_CONTROL={'{','} > "$RUNNER_TEMP/tags.txt"','if [ "$floor" -ge "$NUMBER" ]; then','fi',
             'for f in "${files[@]}" SHA256SUMS; do cmp -- "$f" "$RUNNER_TEMP/check/$f"; done'}
REL_CTL_START=re.compile(r'(if|then|else|elif|fi|for|while|until|do|done|case|esac|select|function|[{}()])(\s|$|;|\()')
REL_GH={'gh api --paginate "repos/${REPO}/releases" --jq \'.[].tag_name\'',
        'gh api -X PATCH "repos/${REPO}/releases/${id}" -F draft=false -f make_latest=true -F "body=@RELEASE_NOTES.md" > /dev/null',
        'gh api -X POST "repos/${REPO}/git/refs" -f "ref=refs/tags/${TAG}" -f "sha=${SHA}" > /dev/null',
        'gh release create "$TAG" --repo "$REPO" --verify-tag --draft --title "Orcon control-firmware $TAG" --notes-file RELEASE_NOTES.md -- "${files[@]}" SHA256SUMS',
        'gh release download "$TAG" --repo "$REPO" --dir "$RUNNER_TEMP/check"',
        'id="$(gh api --paginate "repos/${REPO}/releases" --jq ".[] | select(.draft and .tag_name == \\"$TAG\\") | .id")"'}   # every GitHub CLI call, exactly as reviewed (Codex r4: an extra 'gh release edit')
REL_BANNED=('--help','()')
REL_BANNED_CMD=re.compile(r'(^|[;|]\s*)(alias|shopt|enable|set|unset|trap|eval|source|exec|builtin|command|\.)(\s|$)')
REL_PERMS={'prepare':{'contents':'read'},'build':{'contents':'read'},'sign':{'contents':'read'},'verify':{'contents':'read'},'publish':{'contents':'write'}}
REL_NEEDS={'prepare':None,'build':'prepare','sign':['prepare','build'],'verify':['prepare','build','sign'],'publish':['prepare','build','sign','verify']}
def release_workflow_safe(text):
    d=yaml_subset(text)
    assert set(d)=={'name','on','permissions','concurrency','defaults','env','jobs'},('top-level keys',sorted(d))
    assert list(d['on'])==['workflow_dispatch'],('trigger: manual workflow_dispatch only',d['on'])
    inputs=d['on']['workflow_dispatch']['inputs']
    assert set(inputs)=={'version','notes'} and all(x['type']=='string' for x in inputs.values()),('inputs',inputs)
    assert d['permissions']=={},'top-level permissions must be empty'
    assert d['concurrency']=={'group':'orcon-release-control','cancel-in-progress':False},'one release at a time, never cancelled midway'
    assert d['env']['RELEASE_ENV']=='heltec_v2_sx1276_control','only the control image is released'
    jobs=d['jobs']
    assert set(jobs)==set(REL_PERMS),('jobs',sorted(jobs))
    for name,j in jobs.items():
        assert set(j)<= {'name','needs','if','runs-on','timeout-minutes','environment','permissions','outputs','steps'},(name,'job keys',sorted(j))
        assert j['runs-on']=='ubuntu-24.04' and int(j['timeout-minutes'])<=75,(name,'runner/timeout')
        assert j.get('permissions')==REL_PERMS[name],(name,'permissions',j.get('permissions'))
        assert j.get('needs')==REL_NEEDS[name],(name,'needs',j.get('needs'))
        assert ('environment' in j)==(name=='sign') and j.get('environment',name if name!='sign' else '')!='', (name,'environment only on the signing job')
        if name=='sign': assert j['environment']=='orcon-production-release',('signing environment',j['environment'])
        assert j.get('if') in ((REL_GUARD,) if name in ('prepare','sign') else (None,)),(name,'job guard',j.get('if'))
        for st in j['steps']:
            assert set(st)<= {'name','id','uses','with','env','run'} and ('uses' in st)!=('run' in st),(name,'step keys',sorted(st))
            if 'uses' in st:
                assert st['uses'] in {f'{k}@{v}' for k,v in REL_ACTIONS.items()},(name,'unpinned or unapproved action (or another commit than reviewed)',st['uses'])
                if st['uses'].startswith('actions/checkout@'): assert st['with']=={'ref':'${{ github.sha }}','persist-credentials':False},(name,'checkout: exact commit, no stored credentials')
            else:
                assert '${{' not in st['run'],(name,'expression inside a script (use env)',st['run'][:60])
                for line in st['run'].split('\n'):
                    l=line.strip()
                    assert not l.startswith(('#','!')) and not any(t in l for t in ('||','&&','exit 0')+REL_BANNED) and not REL_BANNED_CMD.search(l),(name,'a script construct that can hide a failure',l)
                    assert not REL_CTL_START.match(l) or l in REL_CONTROL,(name,'shell control line outside the reviewed set',l)
                    assert not re.search(r'(^|[^\w-])gh\s',l) or l in REL_GH,(name,'GitHub CLI call outside the reviewed set',l)
            assert 'secrets' not in str(st.get('with')).lower() and 'github.token' not in str(st.get('with')),(name,'credential in an action input (Codex r5)')
            for k,v in (st.get('env') or {}).items():
                assert ('secrets' in str(v).lower())==(name=='sign' and k=='ORCON_OTA_SIGNING_KEY'),(name,'secret outside the signing step',k)
                assert 'github.token' not in str(v) or (k=='GH_TOKEN' and name in ('prepare','publish')),(name,'token outside prepare/publish',k)
            if name=='sign' and 'secrets.' in str(st.get('env')):
                assert st['env']=={'ORCON_OTA_SIGNING_KEY':'${{ secrets.ORCON_OTA_SIGNING_KEY }}'} and st['run'].strip()=='python3 -I tools/release_ci.py sign --dist dist',('sign step',st)
    flat=text.replace('\r\n','\n')
    assert flat.lower().count('secrets')==1 and 'secrets: inherit' not in flat,'the signing key is referenced exactly once, in the signing step'
    exprs=re.findall(r'\$\{\{\s*(.*?)\s*\}\}',flat)
    assert len(exprs)==flat.count('${{') and set(exprs)<=REL_EXPR,('expressions outside the reviewed list',sorted(set(exprs)-REL_EXPR))
    for bad in ('pull_request','push:','schedule:','workflow_run','repository_dispatch','release:','issue_comment','issues:','continue-on-error',
                'always()','actions/cache','write-all','id-token','heltec_v2_sx1276_tx_poc','dryrun','rftest','webctl','write_flash','erase_flash','heltec_v2.sh',
                'ota-install','/dev/tty','/dev/cu','setInsecure','--insecure','curl -k'):
        assert bad not in flat,('forbidden in the release workflow',bad)
    lines=lambda job:[l.strip() for st in jobs[job]['steps'] for l in st.get('run','').split('\n')]
    for job,need in REL_REQUIRED:
        assert need in lines(job),(job,'required check missing (as its own exact line)',need)
    assert d['defaults']=={'run':{'shell':'bash'}},('defaults: bash with the -eo pipefail template only',d['defaults'])
    assert all(st.get('shell') is None for j in jobs.values() for st in j['steps']),'no step changes the shell'
    # The publish order: verified inputs, the version floor, the tag, a draft, its files compared byte for byte, then public.
    order=['sha256sum -c SHA256SUMS','floor="$(awk','gh api -X POST "repos/${REPO}/git/refs"','gh release create "$TAG"','id="$(gh api','gh release download "$TAG"',
           'for f in "${files[@]}" SHA256SUMS; do cmp -- "$f" "$RUNNER_TEMP/check/$f"; done','gh api -X PATCH "repos/${REPO}/releases/${id}"']
    pub=lines('publish'); at=[next((i for i,l in enumerate(pub) if l.startswith(o)),-1) for o in order]
    assert -1 not in at and at==sorted(at),('publish steps missing or out of order',dict(zip(order,at)))
    assert any(st.get('run','').strip()=='bash test/run_host_tests.sh' and '-fsanitize=address' in str((st.get('env') or {}).get('CXXFLAGS'))
               for st in jobs['build']['steps']),'host tests with AddressSanitizer missing'
    assert [l.strip() for l in flat.split('\n') if 'pio run' in l]==['pio run -e "$RELEASE_ENV"'],'exactly one firmware build: the control env'
    assert not any('uses' in st and st['uses'].startswith('actions/checkout@') for st in jobs['publish']['steps']),'publish runs no repository code'
    assert all('run' in st or st['uses'].split('@')[0] in ('actions/checkout','actions/upload-artifact','actions/download-artifact') for st in jobs['sign']['steps']),'signing job: no setup actions'
WORKFLOWS={'heltec-v2-rx-only.yml':'fbf3af64a87be3822246c0adc17ba3cb5ec42c1ddb725c455153f8fd4ea7a563',   # file -> sha256 of the reviewed content
           'release-control.yml':'28e70cd6dd9f9bc132bc3681a15757987bae800bdfa7f67d274a84831e181f5f'}
def workflows_reviewed(files):   # files: name -> bytes (None: not a regular file) of every *.yml/*.yaml entry
    assert sorted(files)==sorted(WORKFLOWS),('workflow files differ from the reviewed list',sorted(files))
    for n,data in files.items():
        assert isinstance(data,bytes),(n,'must be a regular file, not a symlink or a directory')
        h=hashlib.sha256(data.replace(b'\r\n',b'\n')).hexdigest()
        assert h==WORKFLOWS[n],(n,'changed: review it (and its own security checks), then update its pinned hash',h)
wf_dir=Path('.github/workflows')
wf_files={x.name:(x.read_bytes() if x.is_file() and not x.is_symlink() else None)
          for x in sorted(wf_dir.iterdir()) if x.name.lower().endswith(('.yml','.yaml'))} if wf_dir.is_dir() and not wf_dir.is_symlink() else {}
workflows_reviewed(wf_files)
# GitHub runs what is committed: the same check on HEAD's tree (a committed change hidden by the working copy, or a
# symlinked .github/workflows, is refused too). Only top-level entries count, as for GitHub.
NOREPLACE=dict(os.environ,GIT_NO_REPLACE_OBJECTS='1')   # never a locally replaced object instead of the commit (Codex r5)
head=subprocess.run(['git','ls-tree','-z','HEAD','--','.github/workflows/'],capture_output=True,env=NOREPLACE)
if head.returncode:   # Codex r4: never pass silently; a source archive without git must say so explicitly
    assert os.environ.get('ORCON_GUARD_NO_GIT')=='1',('cannot read HEAD\'s committed workflow tree',head.stderr[-200:])
    print('NOT TESTED: the committed workflow tree (ORCON_GUARD_NO_GIT=1: no git checkout)')
else:
    tree={}
    for entry in filter(None,head.stdout.split(b'\0')):
        meta,path=entry.split(b'\t',1); mode,kind,obj=meta.split()
        name=path.decode().rsplit('/',1)[-1]
        if name.lower().endswith(('.yml','.yaml')):
            tree[name]=(subprocess.run(['git','cat-file','blob',obj.decode()],capture_output=True,check=True,env=NOREPLACE).stdout
                        if kind==b'blob' and mode in (b'100644',b'100755') else None)
    workflows_reviewed(tree)
stub=wf_files['heltec-v2-rx-only.yml'].replace(b'\r\n',b'\n'); pushed=stub.replace(b'  workflow_dispatch:\n',b'  workflow_dispatch:\n  push:\n',1)
assert pushed!=stub and 'unreviewed.yaml' not in WORKFLOWS
for name,files in [('stub-push-trigger',{**wf_files,'heltec-v2-rx-only.yml':pushed}),
                   ('unreviewed-workflow',{**wf_files,'unreviewed.yaml':b'on: push\n'}),
                   ('stub-removed',{k:v for k,v in wf_files.items() if k!='heltec-v2-rx-only.yml'}),
                   ('stub-symlink',{**wf_files,'heltec-v2-rx-only.yml':None})]:
    try: workflows_reviewed(files)
    except AssertionError: print('PASS workflow guard rejects',name)
    else: raise AssertionError('workflow guard accepted '+name)
rel=wf_files['release-control.yml'].decode().replace('\r\n','\n')
release_workflow_safe(rel)
print('PASS release workflow: manual trigger only, least permissions, key only in the signing step of the protected environment')
def rel_mut(old,new,count=1):
    assert rel.count(old)>=1,('mutation anchor missing',old); return rel.replace(old,new,count)
REL_MUTATIONS={
  'push-trigger': rel_mut('  workflow_dispatch:\n','  push:\n    branches: [main]\n  workflow_dispatch:\n'),
  'tag-trigger': rel_mut('  workflow_dispatch:\n','  push:\n    tags: [v*]\n  workflow_dispatch:\n'),
  'pr-target-trigger': rel_mut('  workflow_dispatch:\n','  pull_request_target:\n  workflow_dispatch:\n'),
  'pr-trigger': rel_mut('  workflow_dispatch:\n','  pull_request:\n  workflow_dispatch:\n'),
  'release-trigger': rel_mut('  workflow_dispatch:\n','  release:\n    types: [published]\n  workflow_dispatch:\n'),
  'schedule': rel_mut('  workflow_dispatch:\n','  schedule:\n    - cron: 0 3 * * *\n  workflow_dispatch:\n'),
  'secret-in-build': rel_mut('          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n','          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n          KEY: ${{ secrets.ORCON_OTA_SIGNING_KEY }}\n'),
  'secret-in-prepare': rel_mut('          GH_TOKEN: ${{ github.token }}\n','          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}\n'),
  'secrets-inherit': rel_mut('    environment: orcon-production-release\n','    environment: orcon-production-release\n    secrets: inherit\n'),
  'environment-on-build': rel_mut('    timeout-minutes: 75\n','    timeout-minutes: 75\n    environment: orcon-production-release\n'),
  'other-environment': rel_mut('    environment: orcon-production-release\n','    environment: development\n'),
  'secret-in-other-sign-step': rel_mut('          WANT: ${{ needs.build.outputs.firmware_sha256 }}\n','          WANT: ${{ needs.build.outputs.firmware_sha256 }}\n          K: ${{ secrets.ORCON_OTA_SIGNING_KEY }}\n'),
  'key-to-other-command': rel_mut('        run: python3 -I tools/release_ci.py sign --dist dist\n','        run: echo "$ORCON_OTA_SIGNING_KEY" > key.pem\n'),
  'other-branch': rel_mut("github.ref == 'refs/heads/main'","github.ref == 'refs/heads/develop'",2),
  'no-guard-on-sign': rel_mut("    if: "+REL_GUARD+"\n    runs-on: ubuntu-24.04\n    timeout-minutes: 10\n    environment","    runs-on: ubuntu-24.04\n    timeout-minutes: 10\n    environment"),
  'write-token-in-build': rel_mut('    timeout-minutes: 75\n    permissions:\n      contents: read\n','    timeout-minutes: 75\n    permissions:\n      contents: write\n'),
  'write-all': rel_mut('permissions: {}\n','permissions: write-all\n'),
  'id-token': rel_mut('    permissions:\n      contents: write\n','    permissions:\n      contents: write\n      id-token: write\n'),
  'unpinned-action': rel_mut('actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1','actions/checkout@v7'),
  'third-party-action': rel_mut('actions/setup-python@6d4fd22b20465ccc0f7354476588e8df7c9ca0e0','someone/setup-python@6d4fd22b20465ccc0f7354476588e8df7c9ca0e0'),
  'stored-credentials': rel_mut('          persist-credentials: false\n','          persist-credentials: true\n'),
  'expression-in-script': rel_mut('python3 -I tools/release_ci.py version "$VERSION"','python3 -I tools/release_ci.py version "${{ inputs.version }}"'),
  'continue-on-error': rel_mut('      - name: Host tests\n','      - name: Host tests\n        continue-on-error: true\n'),
  'dry-run-image': rel_mut('pio run -e "$RELEASE_ENV"','pio run -e heltec_v2_sx1276_tx_poc_dryrun'),
  'rf-test-image': rel_mut('  RELEASE_ENV: heltec_v2_sx1276_control\n','  RELEASE_ENV: heltec_v2_sx1276_rftest\n'),
  'rx-only-image': rel_mut('  RELEASE_ENV: heltec_v2_sx1276_control\n','  RELEASE_ENV: heltec_v2_sx1276\n'),
  'second-build': rel_mut('          pio run -e "$RELEASE_ENV"\n','          pio run -e "$RELEASE_ENV"\n          pio run -e heltec_v2_sx1276\n'),
  'skip-build-checks': rel_mut('          python -I tools/release_ci.py verify-build','          true'),
  'skip-old-workflow-guards': rel_mut('          python tools/test_tx_poc_safety.py\n',''),
  'skip-package-verify': rel_mut('          python3 -I tools/release_ci.py verify-package --dist dist --harness harness/release_accept --official --notes="$NOTES"\n',''),
  'cache': rel_mut('      - name: Python\n','      - name: Cache\n        uses: actions/cache@0000000000000000000000000000000000000000\n      - name: Python\n'),
  'hardware': rel_mut('          pio run -e "$RELEASE_ENV"\n','          pio run -e "$RELEASE_ENV"\n          bash tools/heltec_v2.sh flash rollback\n'),
  'publish-runs-repo-code': rel_mut('      - name: The verified files\n','      - name: Code\n        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1\n        with:\n          ref: ${{ github.sha }}\n          persist-credentials: false\n      - name: The verified files\n'),
  'cancel-in-progress': rel_mut('  cancel-in-progress: false\n','  cancel-in-progress: true\n'),
  'yaml-anchor': rel_mut('  RELEASE_ENV: heltec_v2_sx1276_control\n','  RELEASE_ENV: &e heltec_v2_sx1276_control\n'),
  'swallowed-verify': rel_mut('--official --notes="$NOTES"\n','--official --notes="$NOTES" || true\n'),
  'and-true-verify': rel_mut('--official --notes="$NOTES"\n','--official --notes="$NOTES" && true\n'),
  'negated-check': rel_mut('          python -I tools/release_ci.py verify-build','          ! python -I tools/release_ci.py verify-build'),
  'commented-safety': rel_mut('          python tools/test_tx_poc_safety.py\n','          # python tools/test_tx_poc_safety.py\n'),
  'set-plus-e': rel_mut('          python tools/test_tx_poc_safety.py\n','          set +e\n          python tools/test_tx_poc_safety.py\n'),
  'other-action-commit': rel_mut('actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1','actions/checkout@'+'1'*40),
  'escaped-secret': rel_mut('          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n','          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n          K: "${{ \\x73ecrets.ORCON_OTA_SIGNING_KEY }}"\n'),
  'secrets-index': rel_mut('          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n','          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n          K: ${{ secrets[\'ORCON_OTA_SIGNING_KEY\'] }}\n'),
  'tojson-secrets': rel_mut('          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n','          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n          K: ${{ toJSON(SECRETS) }}\n'),
  'token-in-build': rel_mut('          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n','          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n          GH_TOKEN: ${{ github.token }}\n'),
  'other-expression': rel_mut('          VERSION: ${{ inputs.version }}\n','          VERSION: ${{ github.event.inputs.version }}\n'),
  'no-asan': rel_mut('          CXXFLAGS: -fsanitize=address,undefined -fno-sanitize-recover=all\n','          CXXFLAGS: -fsanitize=undefined -fno-sanitize-recover=all\n'),
  'no-pip-check': rel_mut('          "$RUNNER_TEMP/pio-venv/bin/python" -m pip check\n',''),
  'bash-template-dropped': rel_mut('    shell: bash\n','    shell: bash {0}\n'),
  'step-shell': rel_mut('      - name: Host tests\n        env:','      - name: Host tests\n        shell: sh\n        env:'),
  'pip-check-help': rel_mut('          "$RUNNER_TEMP/pio-venv/bin/python" -m pip check\n','          "$RUNNER_TEMP/pio-venv/bin/python" -m pip check --help\n'),
  'if-false-wrapper': rel_mut('          python3 -I tools/release_ci.py verify-package','          if false; then\n          python3 -I tools/release_ci.py verify-package').replace('--official --notes="$NOTES"\n','--official --notes="$NOTES"\n          fi\n',1),
  'verify-help': rel_mut('--official --notes="$NOTES"\n','--official --notes="$NOTES" --help\n'),
  'function-redefine': rel_mut('          chmod 0755 harness/release_accept\n','          python3() { :; }\n          chmod 0755 harness/release_accept\n'),
  'bare-cr': rel_mut('      - name: Host tests\n','      - name: Host tests\n        # Host step configuration\r        if: false\n'),
  'no-regeneration': rel_mut(' --out dist --esptool "$HOME/.platformio/packages/tool-esptoolpy/esptool.py"\n',' --out dist\n'),
  'no-force-reinstall': rel_mut(' --force-reinstall -r tools/release',' -r tools/release'),
  'set-plus-e-after-pipe': rel_mut('          python tools/test_orcon_cli.py\n','          true | set +e\n          python tools/test_orcon_cli.py\n'),
  'source-script': rel_mut('          python tools/test_orcon_cli.py\n','          . ./evil.sh\n          python tools/test_orcon_cli.py\n'),
  'promote-current-draft': rel_mut('          gh api -X PATCH "repos/${REPO}/releases/${id}" -F draft=false -f make_latest=true -F "body=@RELEASE_NOTES.md" > /dev/null\n','          gh release edit "$TAG" --repo "$REPO" --draft=false --latest\n'),
  'draft-not-compared': rel_mut('          for f in "${files[@]}" SHA256SUMS; do cmp -- "$f" "$RUNNER_TEMP/check/$f"; done\n',''),
  'no-version-floor': rel_mut('          if [ "$floor" -ge "$NUMBER" ]; then\n','          if [ "$floor" -gt "999999999" ]; then\n'),
  'publish-before-compare': rel_mut('          gh release download "$TAG" --repo "$REPO" --dir "$RUNNER_TEMP/check"\n','          gh release edit "$TAG" --repo "$REPO" --draft=false --latest\n          gh release download "$TAG" --repo "$REPO" --dir "$RUNNER_TEMP/check"\n'),
  'unchecked-sums': rel_mut('          test "$(sha256sum SHA256SUMS | cut -d\' \' -f1)" = "$SUMS"\n',''),
  'quoted-apostrophe': rel_mut("        description: 'Versie: MAJOR.MINOR.PATCH, bijvoorbeeld 1.0.0'","        description: 'Versie''s: MAJOR.MINOR.PATCH'"),
  'floor-zero': rel_mut('END { print m + 0 }','END { print 0 }'),
  'no-asset-inventory': rel_mut('          cmp <(ls "$RUNNER_TEMP/check" | sort) <( (printf \'%s\\n\' "${files[@]}"; echo SHA256SUMS) | sort)\n',''),
  'no-break-space': rel_mut('    environment: orcon-production-release\n','    environment: orcon-production-release\u00a0\n'),
  'secret-as-action-input': rel_mut('        with:\n          name: release-signed\n','        with:\n          name: release-signed\n          token: ${{ secrets.ORCON_OTA_SIGNING_KEY }}\n').replace('          ORCON_OTA_SIGNING_KEY: ${{ secrets.ORCON_OTA_SIGNING_KEY }}\n','          ORCON_OTA_SIGNING_KEY: x\n'),
  'nested-tag-name': rel_mut("| cut -f2 | sed -n 's#^refs/tags/##p'","| sed -n 's#^.*refs/tags/##p'"),
  'duplicate-key': rel_mut('  PYTHONDONTWRITEBYTECODE: \'1\'\n','  PYTHONDONTWRITEBYTECODE: \'1\'\n  RELEASE_ENV: heltec_v2_sx1276_rftest\n'),
}
for name,txt in REL_MUTATIONS.items():
    assert txt!=rel,name
    try: release_workflow_safe(txt)
    except AssertionError as e: print('PASS release workflow guard rejects',name,'-',str(e)[:90])
    else: raise AssertionError('release workflow guard accepted '+name)
    try: workflows_reviewed({**wf_files,'release-control.yml':txt.encode()})
    except AssertionError: pass
    else: raise AssertionError('pin accepted mutation '+name)
print('PASS workflows match the reviewed, pinned list (the old RX-only workflow stays an inert stub)')
print('PASS local source safety, guard mutations and capability compiles')
