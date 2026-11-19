"""Local-only phase 1 source guards and negative capability compiles. No hardware/network."""
from pathlib import Path
import argparse, configparser, hashlib, re, subprocess

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
try: hold_safe(ota_src.replace('if (!setupweb::authorize(true)) return;','',1))
except AssertionError: print('PASS hold guard rejects mutation unauthenticated-hold')
else: raise AssertionError('hold guard accepted mutation unauthenticated-hold')
# Workflows: none runs today (builds, tests and signing are local), but Actions are not banned for good. Every
# workflow file is pinned by content, because text checks on YAML can be bypassed (blank lines, if: false,
# continue-on-error, a second job, ...). The old RX-only workflow is pinned as an inert stub (manual start only,
# no permissions, no action, stops at once), so changing it fails this test. This runs locally (before every push)
# on the working copy and on HEAD's committed tree; GitHub does not consult it, and a manual run on a ref from before
# the stub uses the old file. A new workflow, such as a
# future release build, is added deliberately: first its own security checks here (least permissions; the OTA
# signing key only in the release-signing job, from a protected GitHub Environment secret, never in the repository
# or in push, PR or test workflows; nothing installed on a board without the owner's consent), then name and hash.
WORKFLOWS={'heltec-v2-rx-only.yml':'fbf3af64a87be3822246c0adc17ba3cb5ec42c1ddb725c455153f8fd4ea7a563'}   # file -> sha256 of the reviewed content
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
head=subprocess.run(['git','ls-tree','-z','HEAD','--','.github/workflows/'],capture_output=True)
if head.returncode: print('NOT TESTED: the committed workflow tree (no git checkout with a HEAD)')
else:
    tree={}
    for entry in filter(None,head.stdout.split(b'\0')):
        meta,path=entry.split(b'\t',1); mode,kind,obj=meta.split()
        name=path.decode().rsplit('/',1)[-1]
        if name.lower().endswith(('.yml','.yaml')):
            tree[name]=(subprocess.run(['git','cat-file','blob',obj.decode()],capture_output=True,check=True).stdout
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
print('PASS workflows match the reviewed, pinned list (the old RX-only workflow stays an inert stub)')
print('PASS local source safety, guard mutations and capability compiles')
