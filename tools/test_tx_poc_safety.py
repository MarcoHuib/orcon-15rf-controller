"""Local-only phase 1 source guards and negative capability compiles. No hardware/network."""
from pathlib import Path
import argparse, configparser, hashlib, os, re, subprocess, sys, tempfile

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
    # The mode-command gates live in ctlAdmit(), shared by the web page, Siri (docs/SIRI.md) and Matter
    # (docs/MATTER.md): the only submitCtl() call, after every gate, and exactly three callers, each with its own
    # authentication first (Matter: the Matter stack's session and access control, before a request reaches us).
    adm=fn(web,'static int ctlAdmit(uint8_t k, uint32_t request, const char*& e, uint32_t& waitMs) {')
    order=['"profile-storage-unavailable"','"profile-required"','"profile-protocol-unsupported"',
           '"profile-unit-mismatch"','"identity-conflict"','"tx-budget"','controller().submitCtl(']
    pos=[adm.find(x) for x in order]
    assert all(x>=0 for x in pos) and pos==sorted(pos),('send gate order',list(zip(order,pos)))
    assert 'identityClear' in adm.split('submitCtl(',1)[1] and 'txpoc::rxReady()' in adm.split('submitCtl(',1)[1]
    callers=len(re.findall(r'\bctlAdmit\s*\(',web))
    assert callers==4,('ctlAdmit callers',callers)           # the definition + the two handlers + matterControl below
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
              r'if \(st != provision::State::Done\) '+say,
              r'const String auth = g_srv->header\("Authorization"\);',
              r'const char\* given = auth\.length\(\) > 7 && !strncasecmp\(auth\.c_str\(\), "Bearer ", 7\) \? auth\.c_str\(\) \+ 7 : "";',
              r'if \(authstore::siriKey\(key\) != authstore::Status::Ok \|\| !webauth::equal\(given, key\)\) '+say,
              r'ctlAdmit\(k, 0, e, w\)'],'siri')
    # Matter: the five keys Siri takes (never the timer), no page sequence, like Siri.
    in_order(fn(web,'int matterControl(const char* key, const char*& e, uint32_t& waitMs) {'),
             [r'const uint8_t k = keyFromName\(key\);',
              r'if \(k == K_NONE \|\| k == K_TIMER\) \{ e = "bad-key"; waitMs = 0; return 400; \}',
              r'return ctlAdmit\(k, 0, e, waitMs\);'],'matter')
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
# The Matter admission (setupweb::matterControl) has exactly one caller: the Matter component's loop-task handoff.
uses=sorted(p.name for p in Path('src').iterdir() if 'matterControl(' in p.read_text())
assert uses==['setup_web.cpp','setup_web.h'] and web.count('matterControl(')==1,uses
mtr=[p for p in sorted(Path('idf/components/orcon_matter').rglob('*')) if p.is_file() and 'matterControl(' in p.read_text(errors='replace')]
assert [str(p) for p in mtr]==['idf/components/orcon_matter/orcon_matter.cpp'],mtr   # its declaration and one call
mtr=mtr[0].read_text()
assert mtr.count('matterControl(')==2 and mtr.count('\nint matterControl(const char* key, const char*& e, uint32_t& waitMs);\n')==1
assert mtr.count('setupweb::matterControl(')==1
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
                     ('siri-locked-ok','if (st != provision::State::Done)','if (false)'),
                     ('siri-key-or-to-and','!= authstore::Status::Ok || !webauth::equal(given, key)','!= authstore::Status::Ok && !webauth::equal(given, key)'),
                     ('send-guard-ignored','    if (!guard(d, K, 3)) return;','    guard(d, K, 3);'),
                     ('siri-state-weakened','st != provision::State::Done)','st == provision::State::New)'),
                     ('siri-key-in-url-spaced','g_srv->header("Authorization")','g_srv->arg( "k" )'),
                     ('siri-no-return','siriSay(401, "De Siri-sleutel klopt niet. Kijk in de webinterface bij Systeem, Integraties.");\n        return;','siriSay(401, "De Siri-sleutel klopt niet. Kijk in de webinterface bij Systeem, Integraties.");'),
                     ('matter-timer-key','if (k == K_NONE || k == K_TIMER) { e = "bad-key"; waitMs = 0; return 400; }','if (k == K_NONE) { e = "bad-key"; waitMs = 0; return 400; }'),
                     ('matter-any-key','if (k == K_NONE || k == K_TIMER) { e = "bad-key"; waitMs = 0; return 400; }',''),
                     ('matter-page-sequence','return ctlAdmit(k, 0, e, waitMs);','return ctlAdmit(k, 1, e, waitMs);'),
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
         ('login-without-signed-ota',['ORCON_WEB_AUTH=1'],'ORCON_WEB_AUTH exists only on the signed Heltec V2 builds')]
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
def update_safe(src,cb=None):
    cb=Path('src/fw_update_roots_cb.h').read_text() if cb is None else cb
    for h,mut in (('static void handleStatus() {','authorize(false)'),('static void handleCheck() {','authorize(true)'),('static void handleInstall() {','authorize(true)')):
        assert fn(src,h).lstrip().startswith('if (!setupweb::authorize(%s)) return;'%mut.split('(')[1].rstrip(')')),(h,'must ask the owner first')
    # The pinned roots on every build: WiFiClientSecure gets the PEM buffer; a Matter build (docs/MATTER.md) offers the
    # same nine roots as DER through mbedTLS's trusted-CA callback (fw_update_roots_cb.h), VERIFY_REQUIRED.
    tls=src.split('struct TlsTransport : fwup::Transport {\n',1)[1].split('    const char* err',1)[0]
    assert tls==('#if ORCON_MATTER\n    rootscb::Client c;\n    static constexpr const char* kRoots = nullptr;   // the callback supplies them\n'
                 '#else\n    WiFiClientSecure c;\n    static constexpr const char* kRoots = FWUP_ROOTS_PEM;\n#endif\n'),'TLS must verify against the pinned roots'
    assert src.count('c.connect(ip, 443, host, kRoots, nullptr, nullptr)')==1,'TLS must verify against the pinned roots'
    att=fn(cb,'static esp_err_t attach(void* conf) {')
    assert 'mbedtls_ssl_conf_authmode((mbedtls_ssl_config*)conf, MBEDTLS_SSL_VERIFY_REQUIRED);' in att and \
           'mbedtls_ssl_conf_ca_cb((mbedtls_ssl_config*)conf, caCallback, nullptr);' in att,'callback roots: VERIFY_REQUIRED'
    assert 'Client() { sslclient->bundle_attach_cb = attach; _use_ca_bundle = true; }' in cb,'the Matter client always attaches them'
    pick=fn(cb,'static int caCallback(void*, mbedtls_x509_crt const* child, mbedtls_x509_crt** candidates) {')
    assert pick.count('_parse_der_nocopy(')==pick.count('ROOT_DER[i], ROOT_DER_LEN[i])')==2,'the callback offers the pinned roots only'
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
                     ('tls-without-roots','c.connect(ip, 443, host, kRoots, nullptr, nullptr)','c.connect(ip, 443)'),
                     ('tls-pem-dropped','kRoots = FWUP_ROOTS_PEM;','kRoots = nullptr;'),
                     ('tls-matter-plain-client','    rootscb::Client c;','    NetworkClientSecure c;'),
                     ('install-from-url','g_offer.m.version == want','g_s->arg("url").length()'),
                     ('install-any-offer','startJob(Job::Install, false, want, commit)','startJob(Job::Install, false)'),
                     ('install-any-commit','LOCKED(offered = g_st.found == fwup::Found::Newer && g_offer.m.version == want && !strcmp(g_offer.m.commit, commit));','LOCKED(offered = g_st.found == fwup::Found::Newer && g_offer.m.version == want);'),):
    m=upd.replace(old,new,1); assert m!=upd,name
    try: update_safe(m)
    except AssertionError: print('PASS update guard rejects mutation',name)
    else: raise AssertionError('update guard accepted mutation '+name)
cbh=Path('src/fw_update_roots_cb.h').read_text()
for name,old,new in (('tls-callback-verify-optional','MBEDTLS_SSL_VERIFY_REQUIRED);','MBEDTLS_SSL_VERIFY_OPTIONAL);'),
                     ('tls-callback-not-attached','sslclient->bundle_attach_cb = attach; ',''),
                     ('tls-callback-trusts-peer','_parse_der_nocopy(list, ROOT_DER[i], ROOT_DER_LEN[i])','_parse_der_nocopy(list, child->raw.p, child->raw.len)')):
    m=cbh.replace(old,new,1); assert m!=cbh,name
    try: update_safe(upd,m)
    except AssertionError: print('PASS update guard rejects mutation',name)
    else: raise AssertionError('update guard accepted mutation '+name)
# The callback's DER roots are exactly the pinned PEM roots: regenerated (fingerprints checked) and compared.
with tempfile.TemporaryDirectory() as d:
    g=run([sys.executable,'-I','tools/gen_roots_der.py','src/fw_update_roots.h',os.path.join(d,'der.h')])
    assert g.returncode==0 and Path(d,'der.h').read_text()==Path('src/fw_update_roots_der.h').read_text(),('DER roots differ from the pinned roots',g.stderr)
print('PASS update check: owner session first, pinned TLS roots, no insecure TLS, signed-OTA writer only')
# The firmware signing key never sets or resets a password (docs/HERSTELSLEUTEL_MIGRATIE.md): no release-key recovery
# route, request, code or state comes back, the CLI has no auth-code, and an OTA trial boot on a board without an owner key
# is never confirmed (the previous image, with whatever recovery it has, returns until the owner registers a key).
LEGACY_RECOVERY=('/api/auth/setup','/api/auth/nonce','/api/auth/reset','orcon-auth-reset','vendorRecoveryOk','State::Legacy',
                 'STORED_LEGACY','checkResetRequest','issueCode','webauth::Recovery','auth-code','herstelcode')
def active_conds(text,pos):                             # the preprocessor conditions in force at pos
    st=[]
    for l in text[:pos].split('\n'):
        s=l.strip()
        if re.match(r'#\s*if',s): st.append(s)
        elif re.match(r'#\s*el',s): st[-1]=s
        elif re.match(r'#\s*endif',s): st.pop()
    return st
def code_only(text):                                    # comments blanked (their newlines kept), string literals kept
    return re.sub(r'"(?:\\.|[^"\\\n])*"|/\*.*?\*/|//[^\n]*',lambda m: m.group(0) if m.group(0)[0]=='"' else re.sub(r'[^\n]','',m.group(0)),text,flags=re.S)
def legacy_recovery_safe(files,ota):
    for path,text in files.items():
        for bad in LEGACY_RECOVERY: assert bad not in text,(path,bad,'release-key password recovery must stay removed')
    pol=files['src/ota_policy.cpp']
    assert 'if (s.releaseKeyReset) return "image-has-release-key-recovery";' in fn(pol,'const char* checkImage('),'no image with the old recovery is written again'
    assert 'releaseKeyReset |= has(b, n, PAT(P_VR));' in pol,'the scanner looks for it'
    ts=re.sub(r'\s+',' ',re.sub(r'//[^\n]*','',fn(pol,'Trial trialStep(')))
    assert ts.endswith(' if (now >= rollbackAfter) return Trial::Rollback; if (healthy && now >= confirmAfter && (!lastTry || now - lastTry >= 1000)) { lastTry = now; return Trial::TryConfirm; } return Trial::Wait;'),'trial decision (host-tested): the deadline first (Codex legacy r3)'
    ci=fn(pol,'const char* checkImage(')
    assert 'if (!s.ownerKey) return "image-without-owner-key";' in ci and 'ownerKey |= has(b, n, PAT(P_OK));' in pol,'every incoming image has owner-key recovery (Codex legacy r3)'
    # The plain owner-key marker exists only in ota_policy.h and in the status of ownership builds: a build without the
    # model (and its scanner) never carries it, so it can never be admitted (Codex legacy r3).
    for path,text in files.items():
        if path.endswith('ota_policy.h'): assert text.count('"ORCON-OWNER-KEY v1"')==1,path; continue
        assert 'ORCON-OWNER-KEY' not in text,(path,'plain owner-key marker')
        assert 'ORCON-MATTER-EMBEDDED-IDENTITY' not in text,(path,'plain embedded-identity marker: every image would read as a migration build')
        if 'OWNER_KEY_MARKER' in text: assert path.endswith('ota_signed.cpp') and text.count('OWNER_KEY_MARKER')==1 and '#if ORCON_OWNER_MODEL\n             otap::OWNER_KEY_MARKER,' in text,(path,'owner-key marker outside an ownership build')
    own=files['src/owner_store.cpp']
    assert 'return ok && onCurve(q);' in fn(own,'bool ownerKey(uint8_t q[65]) {') and 'mbedtls_ecp_check_pubkey(&g, &p) == 0' in fn(own,'static bool onCurve('),'only a usable key counts (Codex legacy r3)'
    assert 'bool recoveryActive() { return g_boot == Mode::Recovery && physicalActive(); }' in own,'WiFi wissen opens no recovery (Codex legacy r3)'
    assert 'inline bool ownerRecovery(const Scanner& s) { return s.ownerModel && (s.ownerKey || s.releaseKeyReset); }' in files['src/ota_policy.h'],'rollback-target classifier (host-tested)'
    assert 'inline bool keepAfterReset(const Scanner& s) { return ownerRecovery(s) && !s.matterIdentity; }' in files['src/ota_policy.h'],'a factory reset never keeps an image with an embedded Matter identity (host-tested)'
    assert 'if (otap::keepAfterReset(s)) return true;' in own and 'return otap::ownerRecovery(s) && s.signedOta && !s.legacyOta;' in fn(ota,'bool previousIsOwnerModel() {'),'a model-only build is never kept or chosen as a rollback target (Codex legacy r4)'
    web=files['src/setup_web.cpp']
    assert 'physicalActive' not in web and 'c.physical = ownerstore::recoveryActive();' in fn(web,'static bool provGate('),'the key and recovery routes follow the Herstelmodus only'
    h=re.sub(r'\s+',' ',fn(ota,'void tick(uint32_t now) {'))
    assert 'const bool healthy = !g_hold && net::wifiConnected() && radio::rxDiagnostics().rxState == 0;' in h,'health check'
    assert 'switch (otap::trialStep(now, healthy, lastTry, CONFIRM_AFTER_MS, ROLLBACK_AFTER_MS)) {' in h,'the trial follows otap::trialStep'
    assert 'case otap::Trial::TryConfirm: if (ownerKeyRegistered() && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {' in h,'a trial boot needs an owner key'
    assert re.search(r'case otap::Trial::Rollback: [^{}]*?rollbackNow\(\); return; case otap::Trial::Wait: return; \}',h),'the deadline rolls back'
    assert ota.count('esp_ota_mark_app_valid_cancel_rollback(')==1,'tick is the only confirmation'
    k=fn(ota,'static bool ownerKeyUsable() {')
    assert k.startswith('\n#if ORCON_OWNER_MODEL\n    uint8_t q[65];\n') and '\n    const bool ok = ownerstore::state() == provision::State::Done && ownerstore::ownerKey(q) && ownerstore::cryptoReady();\n    return ok;\n#else' in k,'the key is read before every attempt, not assumed or cached, and usable by this image'
    assert fn(ota,'static bool ownerKeyRegistered() {').startswith('\n    const bool ok = ownerKeyUsable();\n') and fn(ota,'static bool ownerKeyRegistered() {').rstrip().endswith('return ok;'),'the trial reads the same check'
    # No image is written without that key: writing erases the other slot, which may hold the last image with the old
    # recovery, and the trial would not confirm it anyway (Codex legacy r7). Both writer paths pass an admission with it.
    for f in ('static const char* manualBlocked() {','const char* claimDownload() {'): assert 'if (g_pending || g_stateOpen) return "confirm-running-image-first";\n    if (!ownerKeyUsable()) return "owner-key-first";\n' in fn(ota,f),(f,'no write without a usable owner key')
    assert ota.count('startWrite()')==3 and '        if (const char* e = manualBlocked()) { g_st = St::Failed; g_err = e; dropClient(); return; }\n#if ORCON_OWNER_MODEL\n        if (!setupweb::uploadAuthorized())' in fn(ota,'static void handleUploadData() {'),'the upload passes the admission before startWrite'
    assert 'if (!g_dl) return "not-ready";' in fn(ota,'    const char* begin(const char* manifest, const char* sig) override {') and re.sub(r'\s+','',ota).count('g_dl=true')==1 and fn(ota,'const char* claimDownload() {').index('owner-key-first') < fn(ota,'const char* claimDownload() {').index('g_dl = true;'),'the one-click writer only runs after claimDownload'
    assert 'if (const char* e = ota::claimDownload()) { delete c; return e; }' in files['src/fw_update_esp.cpp'],'the one-click install claims the writer first'
    assert 'extern "C" bool verifyRollbackLater() { return true; }' in ota,'the core never confirms a new image at startup'
    rs=fn(ota,'static otap::RunState runningState() {')
    assert 'esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;' in rs and 'const bool ok = esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK;' in rs and 'return otap::runState(ok, uint32_t(st));' in rs,'the state follows otap::runState (host-tested; Codex legacy r6)'
    rt=fn(ota,'static void readTrialState() {')
    assert 'g_pending = r == otap::RunState::Trial;' in rt and 'g_stateOpen = r == otap::RunState::Unknown;' in rt and ota.count('g_pending = ')==2,'the trial state comes from the OTA data'
    assert 'readTrialState();' in fn(ota,'void begin(WebServer& s) {') and 'static std::atomic<bool> g_stateOpen{true};' in ota,'unknown until read (fail closed)'
    # Only readTrialState sets it, so nothing opens an unknown state by the clock (Codex legacy r6).
    assert re.sub(r'\s+','',ota).count('g_stateOpen=')==1 and not re.search(r'g_stateOpen\s*\.',ota),'no other write to the unknown state'
    assert 'if (g_stateOpen && now - lastRead >= 1000) { lastRead = now; readTrialState();' in re.sub(r'\s+',' ',re.sub(r'//[^\n]*','',fn(ota,'void tick(uint32_t now) {'))),'an unknown trial state is read again, at every uptime (Codex legacy r3-r6)'
    assert 'bool trialNow() { return runningState() == otap::RunState::Trial; }' in ota,'the fail-closed boot reads the state itself'
    assert h.index('readTrialState();') < h.index('if (!g_pending) return;'),'read again before the trial decision'
    for f in ('static const char* manualBlocked() {','const char* claimDownload() {'): assert 'if (g_pending || g_stateOpen) return "confirm-running-image-first";' in fn(ota,f),(f,'no update while the trial state is unknown')
    assert 'bool pendingVerify() { return g_pending || g_stateOpen; }' in ota,'the one-click install waits too'
    assert 'if (g_pending || g_stateOpen) { g_st = St::Failed; return g_err = "confirm-running-image-first"; }' in fn(ota,'static const char* startWrite() {').split('    if (esp_ota_begin(')[0],'the shared write entry checks it too, before the SDK (Codex legacy r6)'
    # The main loop runs the trial in every signed build: one call at loop level, inside exactly #if ORCON_SIGNED_OTA,
    # and that switch is on for the Heltec V2 SX1276 builds (Codex legacy r5/r6).
    # A standalone statement at loop level after net::loop(), with no return or goto before it (Codex legacy r7).
    main=code_only(files['src/main.cpp']); lp=main.index('void loop() {')+len('void loop() {'); call=main.find('\n    net::loop();\n#if ORCON_SIGNED_OTA\n    ota::tick(millis());')
    pre=re.sub(r'"(?:\\.|[^"\\])*"','""',main[lp:call])   # a commented-out call does not count (Codex legacy r8)
    assert main.count('ota::tick(')==1 and lp < call < lp+len(fn(main,'void loop() {')) and active_conds(main,main.index('\n    ota::tick(millis());'))==['#if ORCON_SIGNED_OTA'],'the main loop runs the trial'
    assert pre.count('{')==pre.count('}') and not re.search(r'\b(return|goto)\b',pre),'the trial call is reached in every loop pass'
    cfg=files['src/config.h']
    assert '#if defined(BOARD_HELTEC_V2) && defined(RADIO_SX1276)\n#define ORCON_SIGNED_OTA 1\n#else\n#define ORCON_SIGNED_OTA 0\n#endif' in cfg and re.findall(r'#\s*(define|undef)\s+ORCON_SIGNED_OTA\b',cfg)==['define','define'],'only the board switch sets it'
    for path,text in files.items(): assert not re.search(r'#\s*(define|undef)\s+ORCON_SIGNED_OTA\b',text) or path.endswith('config.h'),(path,'ORCON_SIGNED_OTA is set in config.h only')
    nb=re.sub(r'\s+',' ',re.sub(r'//[^\n]*','',fn(files['src/net.cpp'],'void begin() {')))
    assert 'if (millis() - lastRead >= 1000) { lastRead = millis(); settled = !ownerstore::unsettledBoot(); }' in nb and 'if (ota::trialNow() && ota::previousIsOwnerModel()) {' in nb and nb.count('ota::rollbackNow();')==1,'the fail-closed boot returns only from a trial read now, to an owner-model image (Codex r4, legacy r6)'
    assert fn(ota,'static bool ownerConsent() {').startswith('\n#if ORCON_OWNER_MODEL\n    return setupweb::authorize(true);\n#else'),'manual install: the owner session and CSRF token (Codex legacy r3)'
    for h in ('static void handleNonce() {','static void handleBegin() {'): assert fn(ota,h).lstrip().startswith('if (!ownerConsent()) return;'),(h,'asks the owner first')
    # The helpers themselves: a session AND its CSRF token, on every ownership-model build (Codex legacy r7).
    web=files['src/setup_web.cpp']; az=fn(web,'bool authorize(bool mutation, bool allowAp) {'); ua=fn(web,'bool uploadAuthorized() {')
    # The whole login branch, exactly: no early return before the checks (Codex legacy r8).
    assert az.split('\n#else\n',1)[0]=='\n#if ORCON_WEB_AUTH\n    if (mutation) { if (!sameOrigin(allowAp)) return false; }\n    else if (!hostOk()) { fail(403, "forbidden-request"); return false; }\n    webauth::Session* s = currentSession();\n    if (!s) { fail(401, "login-required"); return false; }\n    if (mutation && !webauth::equal(g_srv->header("X-Orcon-Token").c_str(), s->csrf)) { fail(403, "bad-token"); return false; }\n    return true;','owner consent: same origin, session and CSRF token'
    assert ua.split('\n#else\n',1)[0]=='\n#if ORCON_WEB_AUTH\n    if (!hostOk()) return false;\n    const String origin = g_srv->header("Origin");      // kept alive: c_str() of a temporary would dangle\n    if (origin.length() && !requestAllowed("application/json", origin.c_str(), g_srv->hostHeader().c_str())) return false;   // same origin when sent\n    webauth::Session* s = currentSession();\n    return s && webauth::equal(g_srv->header("X-Orcon-Token").c_str(), s->csrf);','the upload: session and CSRF token'
    assert '#if ORCON_WEB_AUTH && (!ORCON_TX_POC || ORCON_WEB_CONTROL)\n#define ORCON_OWNER_MODEL 1\n' in cfg,'the ownership model always has the login'
    assert '#if ORCON_OWNER_MODEL\n        if (!setupweb::uploadAuthorized()) { g_st = St::Failed; g_err = "login-required"; dropClient(); return; }\n#endif\n        if (startWrite()) return;' in fn(ota,'static void handleUploadData() {'),'the upload checks the session before it writes'
    assert re.search(r'#if ORCON_OWNER_MODEL\n(?:\s*//[^\n]*\n)*\s*s\.on\("/api/ota/nonce",[^\n]*\n\s*s\.on\("/api/ota/begin",[^\n]*\n\s*s\.on\("/api/ota/upload",[^\n]*\n#endif',ota) and ota.count('s.on("/api/ota/begin"')==1,'no network writer without the owner session (Codex legacy r2)'
LR_FILES={str(x):x.read_text() for x in sorted(Path('src').glob('*.[ch]*'))+[Path('src/setup_page.html'),Path('tools/orcon.py')] if x.name!='setup_page.h'}
legacy_recovery_safe(LR_FILES,ota_src)
for name,where,old,new in (('reset-route-back','src/setup_web.cpp','    s.on("/api/auth/login",    HTTP_POST, handleLogin);','    s.on("/api/auth/reset",    HTTP_POST, handleLogin);\n    s.on("/api/auth/login",    HTTP_POST, handleLogin);'),
                           ('legacy-state-back','src/provision.h','enum class State : uint8_t { New, Setup, Done, Locked };','enum class State : uint8_t { New, Setup, Done, Legacy, Locked };\nconstexpr State OLD = State::Legacy;'),
                           ('cli-auth-code-back','tools/orcon.py',"sub.add_parser('keygen')","sub.add_parser('keygen'); sub.add_parser('auth-code')"),
                           ('page-code-form-back','src/setup_page.html','<p class="note" id="aHintOwn"></p>','<p class="note" id="aHintOwn"></p><p>Vraag een herstelcode aan</p>'),
                           ('signed-reset-request-back','src/web_auth.cpp','bool cookieSid(','const char* checkResetRequest(const char*, const char*);\nbool cookieSid('),
                           ('policy-allows-old-recovery','src/ota_policy.cpp','    if (s.releaseKeyReset) return "image-has-release-key-recovery";\n',''),
                           ('trial-no-rate-limit','src/ota_policy.cpp','(!lastTry || now - lastTry >= 1000)','true'),
                           ('trial-ignores-health','src/ota_policy.cpp','if (healthy && now >= confirmAfter','if (now >= confirmAfter'),
                           ('confirm-starves-rollback','src/ota_policy.cpp','    if (now >= rollbackAfter) return Trial::Rollback;','    if (false) return Trial::Rollback;'),
                           ('policy-without-owner-key','src/ota_policy.cpp','    if (!s.ownerKey) return "image-without-owner-key";\n',''),
                           ('plain-marker-in-scanner','src/ota_policy.cpp','ownerKey |= has(b, n, PAT(P_OK));','ownerKey |= has(b, n, OWNER_KEY_MARKER);'),
                           ('marker-in-every-build','src/ota_signed.cpp','#if ORCON_OWNER_MODEL\n             otap::OWNER_KEY_MARKER,','#if 1\n             otap::OWNER_KEY_MARKER,'),
                           ('off-curve-key','src/owner_store.cpp','    return ok && onCurve(q);','    return ok;'),
                           ('wifi-reset-recovers','src/owner_store.cpp','bool recoveryActive() { return g_boot == Mode::Recovery && physicalActive(); }','bool recoveryActive() { return physicalActive(); }'),
                           ('gate-on-physical','src/setup_web.cpp','c.physical = ownerstore::recoveryActive();','c.physical = ownerstore::physicalActive();'),
                           ('classifier-model-only','src/ota_policy.h','return s.ownerModel && (s.ownerKey || s.releaseKeyReset);','return s.ownerModel;'),
                           ('reset-keeps-model-only','src/owner_store.cpp','if (otap::keepAfterReset(s)) return true;','if (s.ownerModel) return true;'),
                           ('reset-keeps-embedded-identity','src/ota_policy.h','{ return ownerRecovery(s) && !s.matterIdentity; }','{ return ownerRecovery(s); }'),
                           ('reset-by-recovery-only','src/owner_store.cpp','if (otap::keepAfterReset(s)) return true;','if (otap::ownerRecovery(s)) return true;'),
                           ('plain-identity-marker-in-scanner','src/ota_policy.cpp','matterIdentity |= has(b, n, PAT(P_MATTER_ID));','matterIdentity |= has(b, n, "ORCON-MATTER-EMBEDDED-IDENTITY v1");'),
                           ('tick-not-called','src/main.cpp','    ota::tick(millis());','    (void)0;'),
                           ('tick-disabled-block','src/main.cpp','#if ORCON_SIGNED_OTA\n    ota::tick(millis());','#if 0\n    ota::tick(millis());'),
                           ('tick-wrapped-out','src/main.cpp','\n#if ORCON_SIGNED_OTA\n    ota::tick(millis());               // confirm a freshly installed image, or roll back\n#endif\n','\n#if 0\n#if ORCON_SIGNED_OTA\n    ota::tick(millis());               // confirm a freshly installed image, or roll back\n#endif\n#endif\n'),
                           ('signed-ota-off','src/config.h','#define ORCON_SIGNED_OTA 1','#define ORCON_SIGNED_OTA 0'),
                           ('signed-ota-undef','src/main.cpp','void loop() {','#undef ORCON_SIGNED_OTA\n#define ORCON_SIGNED_OTA 0\nvoid loop() {'),
                           ('back-without-trial','src/net.cpp','if (ota::trialNow() && ota::previousIsOwnerModel()) {','if (ota::previousIsOwnerModel()) {'),
                           ('if-false-before-tick','src/main.cpp','\n#if ORCON_SIGNED_OTA\n    ota::tick(millis());','\n    if (false)\n#if ORCON_SIGNED_OTA\n    ota::tick(millis());'),
                           ('tick-in-false-block','src/main.cpp','    net::loop();\n#if ORCON_SIGNED_OTA\n    ota::tick(millis());               // confirm a freshly installed image, or roll back\n#endif\n','    if (false) {\n    net::loop();\n#if ORCON_SIGNED_OTA\n    ota::tick(millis());               // confirm a freshly installed image, or roll back\n#endif\n    }\n'),
                           ('return-before-tick','src/main.cpp','    // 3) web / OLED\n','    return;\n    // 3) web / OLED\n'),
                           ('signed-ota-redefined','src/config.h','#else\n#define ORCON_SIGNED_OTA 0\n#endif','#else\n#define ORCON_SIGNED_OTA 0\n#endif\n#undef ORCON_SIGNED_OTA\n#define ORCON_SIGNED_OTA 0'),
                           ('tick-block-commented','src/main.cpp','    net::loop();\n#if ORCON_SIGNED_OTA\n    ota::tick(millis());               // confirm a freshly installed image, or roll back\n#endif\n','    /*\n    net::loop();\n#if ORCON_SIGNED_OTA\n    ota::tick(millis());               // confirm a freshly installed image, or roll back\n#endif\n    */\n'),
                           ('upload-early-return','src/setup_web.cpp','#if ORCON_WEB_AUTH\n    if (!hostOk()) return false;\n    const String origin','#if ORCON_WEB_AUTH\n    if (!hostOk()) return false;\n    return false;\n    const String origin'),
                           ('consent-early-return','src/setup_web.cpp','    if (mutation) { if (!sameOrigin(allowAp)) return false; }\n','    return true;\n    if (mutation) { if (!sameOrigin(allowAp)) return false; }\n'),
                           ('upload-session-only','src/setup_web.cpp','    return s && webauth::equal(g_srv->header("X-Orcon-Token").c_str(), s->csrf);','    return s != nullptr;'),
                           ('consent-without-csrf','src/setup_web.cpp','    if (mutation && !webauth::equal(g_srv->header("X-Orcon-Token").c_str(), s->csrf)) { fail(403, "bad-token"); return false; }\n',''),
                           ('owner-model-without-login','src/config.h','#if ORCON_WEB_AUTH && (!ORCON_TX_POC || ORCON_WEB_CONTROL)\n#define ORCON_OWNER_MODEL 1','#if (!ORCON_TX_POC || ORCON_WEB_CONTROL)\n#define ORCON_OWNER_MODEL 1'),
                           ('one-click-unclaimed','src/fw_update_esp.cpp','if (const char* e = ota::claimDownload()) { delete c; return e; }','(void)0;'),
                           ('settled-read-once','src/net.cpp','            if (millis() - lastRead >= 1000) { lastRead = millis(); settled = !ownerstore::unsettledBoot(); }\n','')):
    files=dict(LR_FILES); m=files[where].replace(old,new,1); assert m!=files[where],name; files[where]=m
    try: legacy_recovery_safe(files,ota_src)
    except AssertionError: print('PASS legacy-recovery guard rejects mutation',name)
    else: raise AssertionError('legacy-recovery guard accepted mutation '+name)
for name,old,new in (('trial-without-key','if (ownerKeyRegistered() && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)','if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)'),
                     ('key-assumed','    const bool ok = ownerstore::state() == provision::State::Done && ownerstore::ownerKey(q) && ownerstore::cryptoReady();','    const bool ok = true;'),
                     ('key-read-once','    const bool ok = ownerstore::state() == provision::State::Done','    static const bool ok = ownerstore::state() == provision::State::Done'),
                     ('key-without-crypto',' && ownerstore::ownerKey(q) && ownerstore::cryptoReady();',' && ownerstore::ownerKey(q);'),
                     ('core-confirms-at-boot','extern "C" bool verifyRollbackLater() { return true; }','extern "C" bool verifyRollbackLater() { return false; }'),
                     ('trial-state-ignored','g_pending = r == otap::RunState::Trial;','g_pending = false;'),
                     ('unknown-latched-open','    if (g_stateOpen && now - lastRead >= 1000) {','    if (g_stateOpen && now >= ROLLBACK_AFTER_MS) g_stateOpen = false;\n    if (g_stateOpen && now - lastRead >= 1000) {'),
                     ('unknown-opened-by-store','    if (g_stateOpen && now - lastRead >= 1000) {','    if (now >= ROLLBACK_AFTER_MS) g_stateOpen.store(false);\n    if (g_stateOpen && now - lastRead >= 1000) {'),
                     ('sdk-gate-only','    if (g_pending || g_stateOpen) { g_st = St::Failed; return g_err = "confirm-running-image-first"; }\n',''),
                     ('state-without-policy','return otap::runState(ok, uint32_t(st));','return ok ? otap::RunState::Settled : otap::RunState::Unknown;'),
                     ('trial-now-cached','bool trialNow() { return runningState() == otap::RunState::Trial; }','bool trialNow() { return g_pending; }'),
                     ('unknown-never-retried','        lastRead = now; readTrialState();','        lastRead = now;'),
                     ('unknown-starts-open','static std::atomic<bool> g_stateOpen{true};','static std::atomic<bool> g_stateOpen{false};'),
                     ('writer-while-unknown','bool pendingVerify() { return g_pending || g_stateOpen; }','bool pendingVerify() { return g_pending; }'),
                     ('signer-only-writer','#if ORCON_OWNER_MODEL\n    // Writing firmware needs','#if 1\n    // Writing firmware needs'),
                     ('confirm-ignores-health','const bool healthy = !g_hold && net::wifiConnected() && radio::rxDiagnostics().rxState == 0;','const bool healthy = true;'),
                     ('deadline-without-rollback','            rollbackNow();\n            return;\n        case otap::Trial::Wait:','            return;\n        case otap::Trial::Wait:'),
                     ('second-confirm','void rollbackNow() {','static void confirmEarly() { esp_ota_mark_app_valid_cancel_rollback(); }\nvoid rollbackNow() {'),
                     ('consent-without-session','    return setupweb::authorize(true);\n#else','    return hostOk();\n#else'),
                     ('nonce-without-consent','static void handleNonce() {\n    if (!ownerConsent()) return;','static void handleNonce() {'),
                     ('upload-without-session','        if (!setupweb::uploadAuthorized()) {','        if (false) {'),
                     ('manual-without-key','    if (!ownerKeyUsable()) return "owner-key-first";\n#if !ORCON_TX_POC','#if !ORCON_TX_POC'),
                     ('download-without-key','    if (!ownerKeyUsable()) return "owner-key-first";\n    g_nonce[0] = 0;','    g_nonce[0] = 0;'),
                     ('key-check-disabled','static bool ownerKeyUsable() {\n#if ORCON_OWNER_MODEL','static bool ownerKeyUsable() {\n#if 0'),
                     ('trial-key-assumed','    const bool ok = ownerKeyUsable();','    const bool ok = true;'),
                     ('upload-unblocked','        if (const char* e = manualBlocked()) { g_st = St::Failed; g_err = e; dropClient(); return; }\n',''),
                     ('sink-without-claim','        if (!g_dl) return "not-ready";\n',''),
                     ('second-claim','    g_dl = true;\n    return nullptr;','    g_dl = true;\n    return nullptr;\n}\nstatic void claimAgain() {\n    g_dl = true;'),
                     ('fallback-model-only','return otap::ownerRecovery(s) && s.signedOta','return s.ownerModel && s.signedOta')):
    m=ota_src.replace(old,new,1); assert m!=ota_src,name
    try: legacy_recovery_safe(LR_FILES,m)
    except AssertionError: print('PASS legacy-recovery guard rejects mutation',name)
    else: raise AssertionError('legacy-recovery guard accepted mutation '+name)
print('PASS no password recovery with the firmware signing key; a trial boot without an owner key is never confirmed')
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
          'github.sha','github.token','inputs.notes','inputs.release_type','needs.build.outputs.firmware_sha256','needs.prepare.outputs.number',
          'needs.prepare.outputs.tag','runner.temp',
          'secrets.ORCON_OTA_SIGNING_KEY','steps.check.outputs.firmware_sha256','needs.history.outputs.newest','steps.list.outputs.newest','steps.version.outputs.name','steps.version.outputs.number','steps.version.outputs.tag',
          'needs.verify.outputs.sums_sha256','needs.verify.outputs.notes_sha256','steps.final.outputs.sums_sha256','steps.final.outputs.notes_sha256',
          # the beta channel and the Matter variant (a beta carries both variants; docs/RELEASES.md)
          'inputs.channel','needs.history.outputs.betas','steps.list.outputs.betas','needs.prepare.outputs.pre','needs.prepare.outputs.rank',
          'needs.prepare.outputs.is_beta','needs.prepare.outputs.latest','steps.version.outputs.pre','steps.version.outputs.rank',
          'steps.version.outputs.is_beta','steps.version.outputs.latest','needs.build.outputs.builds_sha256','steps.check.outputs.builds_sha256',
          'needs.sign.outputs.packages_sha256','steps.package.outputs.packages_sha256','steps.floor.outputs.top'}
REL_AWK='/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)$/ { sub(/^v/, ""); split($0, p, "."); n = p[1] * 1000000 + p[2] * 1000 + p[3]; if (n > m) m = n } END { print m + 0 }'   # the version rule of the history job and of the publish floor
REL_BETA_AWK='/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)-beta[.][1-9][0-9]?$/ { sub(/^v/, ""); printf "v%s ", $0 }'   # the beta tags (history)
REL_PFLOOR_AWK='/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)-beta[.][1-9][0-9]?$/ { t = $0; sub(/^v/, "", t); split(t, a, "-beta."); split(a[1], p, "."); if (p[1] * 1000000 + p[2] * 1000 + p[3] == n) if (a[2] + 0 > m) m = a[2] + 0 } END { print m + 0 }'   # the highest beta of this number (publish)
REL_TOP_AWK='/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)(-beta[.][1-9][0-9]?)?$/ { t = $0; sub(/^v/, "", t); split(t, a, "-beta."); split(a[1], p, "."); r = (p[1] * 1000000 + p[2] * 1000 + p[3]) * 100 + (a[2] == "" ? 100 : a[2]); if (r > m) m = r } END { print m + 0 }'   # the highest rank of every tag (publish: the beta channel)
# Every check the release depends on, as its own exact script line (Codex r4: a prefix let "--help" or a wrapper pass).
REL_REQUIRED=(('prepare','python3 -I tools/release_ci.py source --repo "$REPO" --ref "$REF" --event "$EVENT" --default-branch "$DEFAULT_BRANCH" --sha "$SHA" --head "$head"'),
    ('prepare','python3 -I tools/release_ci.py next --newest "$NEWEST" --channel "$CHANNEL" --betas="$BETAS" --notes="$NOTES" -- "$RELEASE_TYPE" >> "$GITHUB_OUTPUT"'),
    ('history','betas="$(awk \''+REL_BETA_AWK+'\' "$RUNNER_TEMP/tags.txt")"'),('history','echo "betas=$betas" >> "$GITHUB_OUTPUT"'),
    ('build','bash tools/fetch_idf_deps.sh'),('build','bash tools/build_idf.sh --release "$NUMBER" --pre "$PRE"'),
    ('build','bash tools/release_matter_check.sh "$NUMBER" "$PRE" "$SHA"'),
    ('build','echo "builds_sha256=$(cat dist/*/BUILD_INFO.json | sha256sum | cut -d\' \' -f1)" >> "$GITHUB_OUTPUT"'),
    ('sign','test "$(cat dist/*/BUILD_INFO.json | sha256sum | cut -d\' \' -f1)" = "$BUILDS"'),
    ('verify','test "$(cat dist/*/SHA256SUMS | sha256sum | cut -d\' \' -f1)" = "$WANT"'),
    ('verify','python3 -I tools/release_ci.py assemble --dist dist --out publish'),
    ('publish','pfloor="$(awk -v n="$NUMBER" \''+REL_PFLOOR_AWK+'\' "$RUNNER_TEMP/tags.txt")"'),('publish','if [ "$pfloor" -ge "$PRE" ]; then'),
    ('publish','top="$(awk \''+REL_TOP_AWK+'\' "$RUNNER_TEMP/tags.txt")"'),('publish','if [ "$RANK" -gt "$TOP" ]; then'),
    ('build','python -m venv "$RUNNER_TEMP/pio-venv"'),
    ('build','"$RUNNER_TEMP/pio-venv/bin/python" -m pip install --require-hashes --no-deps --only-binary=:all: --force-reinstall -r tools/release/requirements-platformio.txt'),
    ('build','"$RUNNER_TEMP/pio-venv/bin/python" -m pip check'),('build','echo "$RUNNER_TEMP/pio-venv/bin" >> "$GITHUB_PATH"'),
    ('build','python -I tools/release_ci.py packages --list "$RUNNER_TEMP/packages.txt"'),('build','bash test/run_host_tests.sh'),
    ('build','python tools/test_tx_poc_safety.py'),('build','git ls-files -z | xargs -0 python tools/check_privacy.py'),
    ('build','python tools/test_orcon_cli.py'),
    ('build','python -I tools/release_ci.py verify-build --build ".pio/build/$RELEASE_ENV" --version "$NUMBER" --pre "$PRE" --commit "$SHA" --harness "$RUNNER_TEMP/harness/release_accept" --nm "$tc/xtensa-esp32-elf-nm" --out dist/control --esptool "$HOME/.platformio/packages/tool-esptoolpy/esptool.py"'),
    ('build','python -I tools/check_elf_tx.py ".pio/build/$RELEASE_ENV/firmware.elf" "$RELEASE_ENV" "$tc"'),
    ('build','RELEASE_BUILD_DIR=".pio/build/$RELEASE_ENV" RELEASE_VERSION="$NUMBER" RELEASE_PRE="$PRE" RELEASE_COMMIT="$SHA" python tools/test_release_ci.py'),
    ('build','python -I tools/release_ci.py manifest --dist dist'),('sign','python3 -I tools/release_ci.py manifest --dist dist'),
    ('sign','python3 -I tools/release_ci.py package --dist dist --notes="$NOTES"'),
    ('verify','python3 -I tools/release_ci.py verify-package --dist dist --harness harness/release_accept --release "$TAG" --official --notes="$NOTES"'),
    ('publish','test "$(sha256sum SHA256SUMS | cut -d\' \' -f1)" = "$SUMS"'),('publish','test "$(sha256sum RELEASE_NOTES.md | cut -d\' \' -f1)" = "$NOTES_SHA"'),
    ('publish','sha256sum -c SHA256SUMS'),('publish','if [ "$floor" -ge "$NUMBER" ]; then'),
    ('publish','floor="$(awk \''+REL_AWK+'\' "$RUNNER_TEMP/tags.txt")"'),
    ('publish','cmp <(ls "$RUNNER_TEMP/check" | sort) <( (printf \'%s\\n\' "${files[@]}"; echo SHA256SUMS) | sort)'),
    ('history','git ls-remote --tags "https://github.com/${REPO}.git" | cut -f2 | sed -n \'s#^refs/tags/##p\' | sed \'s/\\^{}$//\''),
    ('history','gh api --paginate "repos/${REPO}/releases" --jq \'.[].tag_name\''),('history','newest="$(awk \''+REL_AWK+'\' "$RUNNER_TEMP/tags.txt")"'),
    ('history','echo "newest=$newest" >> "$GITHUB_OUTPUT"'),
    ('publish','git ls-remote --tags "https://github.com/${REPO}.git" | cut -f2 | sed -n \'s#^refs/tags/##p\' | sed \'s/\\^{}$//\''),
    ('publish','gh api -X POST "repos/${REPO}/git/refs" -f "ref=refs/tags/${TAG}" -f "sha=${SHA}" > /dev/null'),
    ('publish','printf \'%s\' "$id" | grep -qxE \'[0-9]+\''),
    ('publish','for f in "${files[@]}" SHA256SUMS; do cmp -- "$f" "$RUNNER_TEMP/check/$f"; done'),
    ('publish','gh api -X PATCH "repos/${REPO}/releases/${id}" -F draft=false -F "prerelease=$PRERELEASE" -f "make_latest=$LATEST" -F "body=@RELEASE_NOTES.md" > /dev/null'))
# The only script lines that start with shell control syntax (a wrapper like "if false; then" around a check, or a block
# that swallows a failure, is refused); and constructs that could redefine or disable a command.
REL_CONTROL={'{','} > "$RUNNER_TEMP/tags.txt"','if [ "$floor" -ge "$NUMBER" ]; then','fi','if [ "$PRE" -gt 0 ]; then','if [ "$pfloor" -ge "$PRE" ]; then',
             'if [ "$RANK" -gt "$TOP" ]; then','if ! gh release view channel-beta --repo "$REPO" > /dev/null 2>&1; then',
             'for f in orcon-update.json orcon-update-matter.json; do','if [ -f "$f" ]; then','done',
             'for f in "${files[@]}" SHA256SUMS; do cmp -- "$f" "$RUNNER_TEMP/check/$f"; done'}
REL_CTL_START=re.compile(r'(if|then|else|elif|fi|for|while|until|do|done|case|esac|select|function|[{}()])(\s|$|;|\()')
REL_GH={'gh api --paginate "repos/${REPO}/releases" --jq \'.[].tag_name\'',
        'gh api -X PATCH "repos/${REPO}/releases/${id}" -F draft=false -F "prerelease=$PRERELEASE" -f "make_latest=$LATEST" -F "body=@RELEASE_NOTES.md" > /dev/null',
        'if ! gh release view channel-beta --repo "$REPO" > /dev/null 2>&1; then',
        'gh release create channel-beta --repo "$REPO" --prerelease --target "$SHA" --title "Beta-kanaal (update-index)" --notes "De update-index van het beta-kanaal. De firmware staat in de releases zelf."',
        'gh release upload channel-beta --repo "$REPO" --clobber -- "$f"',
        'gh api -X POST "repos/${REPO}/git/refs" -f "ref=refs/tags/${TAG}" -f "sha=${SHA}" > /dev/null',
        'gh release create "$TAG" --repo "$REPO" --verify-tag --draft --title "Orcon control-firmware $TAG" --notes-file RELEASE_NOTES.md -- "${files[@]}" SHA256SUMS',
        'gh release download "$TAG" --repo "$REPO" --dir "$RUNNER_TEMP/check"',
        'id="$(gh api --paginate "repos/${REPO}/releases" --jq ".[] | select(.draft and .tag_name == \\"$TAG\\") | .id")"'}   # every GitHub CLI call, exactly as reviewed (Codex r4: an extra 'gh release edit')
REL_GH_JOB={'history':{'gh api --paginate "repos/${REPO}/releases" --jq \'.[].tag_name\''},'publish':REL_GH}   # read-only in history (Codex auto-version r2)
REL_LIST='{\n  git ls-remote --tags "https://github.com/${REPO}.git" | cut -f2 | sed -n \'s#^refs/tags/##p\' | sed \'s/\\^{}$//\'\n  gh api --paginate "repos/${REPO}/releases" --jq \'.[].tag_name\'\n} > "$RUNNER_TEMP/tags.txt"\n'
REL_JOBS={   # the jobs that hold a credential or decide the version, every step in order, whole (Codex auto-version r4)
    'history':[{'name': 'Every tag and every release, drafts included (no repository code)', 'id': 'list', 'env': {'REPO': '${{ github.repository }}', 'GH_TOKEN': '${{ github.token }}'}, 'run': '{\n  git ls-remote --tags "https://github.com/${REPO}.git" | cut -f2 | sed -n \'s#^refs/tags/##p\' | sed \'s/\\^{}$//\'\n  gh api --paginate "repos/${REPO}/releases" --jq \'.[].tag_name\'\n} > "$RUNNER_TEMP/tags.txt"\nnewest="$(awk \'/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)$/ { sub(/^v/, ""); split($0, p, "."); n = p[1] * 1000000 + p[2] * 1000 + p[3]; if (n > m) m = n } END { print m + 0 }\' "$RUNNER_TEMP/tags.txt")"\necho "newest=$newest" >> "$GITHUB_OUTPUT"\nbetas="$(awk \'/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)-beta[.][1-9][0-9]?$/ { sub(/^v/, ""); printf "v%s ", $0 }\' "$RUNNER_TEMP/tags.txt")"\necho "betas=$betas" >> "$GITHUB_OUTPUT"\n'}],
    'prepare':[{'name': 'Check out the exact commit', 'uses': 'actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1', 'with': {'ref': '${{ github.sha }}', 'persist-credentials': False}}, {'name': 'Only a manual run on the head of the default branch of this repository', 'env': {'REPO': '${{ github.repository }}', 'REF': '${{ github.ref }}', 'EVENT': '${{ github.event_name }}', 'DEFAULT_BRANCH': '${{ github.event.repository.default_branch }}', 'SHA': '${{ github.sha }}'}, 'run': 'head="$(git ls-remote "https://github.com/${REPO}.git" "refs/heads/${DEFAULT_BRANCH}" | cut -f1)"\npython3 -I tools/release_ci.py source --repo "$REPO" --ref "$REF" --event "$EVENT" --default-branch "$DEFAULT_BRANCH" --sha "$SHA" --head "$head"\n'}, {'name': 'The next version after every tag and release', 'id': 'version', 'env': {'RELEASE_TYPE': '${{ inputs.release_type }}', 'CHANNEL': '${{ inputs.channel }}', 'NOTES': '${{ inputs.notes }}', 'NEWEST': '${{ needs.history.outputs.newest }}', 'BETAS': '${{ needs.history.outputs.betas }}'}, 'run': 'python3 -I tools/release_ci.py next --newest "$NEWEST" --channel "$CHANNEL" --betas="$BETAS" --notes="$NOTES" -- "$RELEASE_TYPE" >> "$GITHUB_OUTPUT"\n'}],
    'sign':[{'name': 'Check out the exact commit (release tools only)', 'uses': 'actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1', 'with': {'ref': '${{ github.sha }}', 'persist-credentials': False}}, {'name': 'The verified build', 'uses': 'actions/download-artifact@9000827ccba6bdab643e8b6fd33ac0654aef8333', 'with': {'name': 'release-build', 'path': 'dist'}}, {'name': 'Same images as the build job checked', 'env': {'WANT': '${{ needs.build.outputs.firmware_sha256 }}', 'BUILDS': '${{ needs.build.outputs.builds_sha256 }}'}, 'run': 'test -n "$WANT"\ntest "$(sha256sum dist/control/firmware.bin | cut -d\' \' -f1)" = "$WANT"\ntest "$(cat dist/*/BUILD_INFO.json | sha256sum | cut -d\' \' -f1)" = "$BUILDS"\npython3 -I tools/release_ci.py manifest --dist dist\n'}, {'name': 'Sign the release manifest (the only step that sees the key)', 'env': {'ORCON_OTA_SIGNING_KEY': '${{ secrets.ORCON_OTA_SIGNING_KEY }}'}, 'run': 'python3 -I tools/release_ci.py sign --dist dist'}, {'name': 'Bundles, update indexes and checksums', 'id': 'package', 'env': {'NOTES': '${{ inputs.notes }}'}, 'run': 'python3 -I tools/release_ci.py package --dist dist --notes="$NOTES"\necho "packages_sha256=$(cat dist/*/SHA256SUMS | sha256sum | cut -d\' \' -f1)" >> "$GITHUB_OUTPUT"\n'}, {'name': 'Hand the signed package to the verify job', 'uses': 'actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9', 'with': {'name': 'release-signed', 'path': 'dist/', 'if-no-files-found': 'error', 'retention-days': '3'}}],
    'verify':[{'name': 'Check out the exact commit', 'uses': 'actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1', 'with': {'ref': '${{ github.sha }}', 'persist-credentials': False}}, {'name': 'The signed package', 'uses': 'actions/download-artifact@9000827ccba6bdab643e8b6fd33ac0654aef8333', 'with': {'name': 'release-signed', 'path': 'dist'}}, {'name': 'The device-policy harness', 'uses': 'actions/download-artifact@9000827ccba6bdab643e8b6fd33ac0654aef8333', 'with': {'name': 'release-harness', 'path': 'harness'}}, {'name': 'Signatures, device policy, update indexes, checksums', 'id': 'final', 'env': {'WANT': '${{ needs.sign.outputs.packages_sha256 }}', 'FW': '${{ needs.build.outputs.firmware_sha256 }}', 'TAG': '${{ needs.prepare.outputs.tag }}', 'NOTES': '${{ inputs.notes }}', 'RUN_URL': '${{ github.server_url }}/${{ github.repository }}/actions/runs/${{ github.run_id }}'}, 'run': 'test "$(cat dist/*/SHA256SUMS | sha256sum | cut -d\' \' -f1)" = "$WANT"\ntest "$(sha256sum dist/control/firmware.bin | cut -d\' \' -f1)" = "$FW"\nchmod 0755 harness/release_accept\npython3 -I tools/release_ci.py verify-package --dist dist --harness harness/release_accept --release "$TAG" --official --notes="$NOTES"\npython3 -I tools/release_ci.py assemble --dist dist --out publish\npython3 -I tools/release_ci.py notes --dist dist --notes="$NOTES" --run "$RUN_URL" --sums publish/SHA256SUMS > "$RUNNER_TEMP/RELEASE_NOTES.md"\ncp "$RUNNER_TEMP/RELEASE_NOTES.md" publish/\necho "sums_sha256=$(sha256sum publish/SHA256SUMS | cut -d\' \' -f1)" >> "$GITHUB_OUTPUT"\necho "notes_sha256=$(sha256sum publish/RELEASE_NOTES.md | cut -d\' \' -f1)" >> "$GITHUB_OUTPUT"\n'}, {'name': 'Hand the verified files to the publish job', 'uses': 'actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9', 'with': {'name': 'release-verified', 'path': 'publish/', 'if-no-files-found': 'error', 'retention-days': '3'}}],
    'publish':[{'name': 'The verified files', 'uses': 'actions/download-artifact@9000827ccba6bdab643e8b6fd33ac0654aef8333', 'with': {'name': 'release-verified', 'path': 'publish'}}, {'name': 'Exactly the verified files', 'env': {'SUMS': '${{ needs.verify.outputs.sums_sha256 }}', 'NOTES_SHA': '${{ needs.verify.outputs.notes_sha256 }}'}, 'run': 'cd publish\ntest "$(sha256sum SHA256SUMS | cut -d\' \' -f1)" = "$SUMS"\ntest "$(sha256sum RELEASE_NOTES.md | cut -d\' \' -f1)" = "$NOTES_SHA"\nsha256sum -c SHA256SUMS\ntest "$(ls | sort)" = "$( (cut -d\' \' -f3- SHA256SUMS; echo SHA256SUMS; echo RELEASE_NOTES.md) | sort)"\n'}, {'name': 'Still the newest version, then the tag at the released commit (fails when it already exists)', 'id': 'floor', 'env': {'GH_TOKEN': '${{ github.token }}', 'REPO': '${{ github.repository }}', 'TAG': '${{ needs.prepare.outputs.tag }}', 'NUMBER': '${{ needs.prepare.outputs.number }}', 'PRE': '${{ needs.prepare.outputs.pre }}', 'SHA': '${{ github.sha }}'}, 'run': '{\n  git ls-remote --tags "https://github.com/${REPO}.git" | cut -f2 | sed -n \'s#^refs/tags/##p\' | sed \'s/\\^{}$//\'\n  gh api --paginate "repos/${REPO}/releases" --jq \'.[].tag_name\'\n} > "$RUNNER_TEMP/tags.txt"\nfloor="$(awk \'/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)$/ { sub(/^v/, ""); split($0, p, "."); n = p[1] * 1000000 + p[2] * 1000 + p[3]; if (n > m) m = n } END { print m + 0 }\' "$RUNNER_TEMP/tags.txt")"\nif [ "$floor" -ge "$NUMBER" ]; then\n  echo "::error::$TAG is not newer than every tag and release (newest $floor): a rerun of an older release is refused"; exit 1\nfi\npfloor="$(awk -v n="$NUMBER" \'/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)-beta[.][1-9][0-9]?$/ { t = $0; sub(/^v/, "", t); split(t, a, "-beta."); split(a[1], p, "."); if (p[1] * 1000000 + p[2] * 1000 + p[3] == n) if (a[2] + 0 > m) m = a[2] + 0 } END { print m + 0 }\' "$RUNNER_TEMP/tags.txt")"\nif [ "$PRE" -gt 0 ]; then\n  if [ "$pfloor" -ge "$PRE" ]; then\n    echo "::error::$TAG: beta $pfloor of this version exists already: a beta number is published once"; exit 1\n  fi\nfi\ntop="$(awk \'/^v?(0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)[.](0|[1-9][0-9]?[0-9]?)(-beta[.][1-9][0-9]?)?$/ { t = $0; sub(/^v/, "", t); split(t, a, "-beta."); split(a[1], p, "."); r = (p[1] * 1000000 + p[2] * 1000 + p[3]) * 100 + (a[2] == "" ? 100 : a[2]); if (r > m) m = r } END { print m + 0 }\' "$RUNNER_TEMP/tags.txt")"\necho "top=$top" >> "$GITHUB_OUTPUT"\ngh api -X POST "repos/${REPO}/git/refs" -f "ref=refs/tags/${TAG}" -f "sha=${SHA}" > /dev/null\n'}, {'name': 'Draft release, check its files, then publish (a beta as a pre-release, never "latest")', 'env': {'GH_TOKEN': '${{ github.token }}', 'REPO': '${{ github.repository }}', 'TAG': '${{ needs.prepare.outputs.tag }}', 'PRERELEASE': '${{ needs.prepare.outputs.is_beta }}', 'LATEST': '${{ needs.prepare.outputs.latest }}'}, 'run': 'cd publish\nmapfile -t files < <(cut -d\' \' -f3- SHA256SUMS)\ngh release create "$TAG" --repo "$REPO" --verify-tag --draft --title "Orcon control-firmware $TAG" --notes-file RELEASE_NOTES.md -- "${files[@]}" SHA256SUMS\nid="$(gh api --paginate "repos/${REPO}/releases" --jq ".[] | select(.draft and .tag_name == \\"$TAG\\") | .id")"\nprintf \'%s\' "$id" | grep -qxE \'[0-9]+\'\nmkdir -p "$RUNNER_TEMP/check"\ngh release download "$TAG" --repo "$REPO" --dir "$RUNNER_TEMP/check"\ncmp <(ls "$RUNNER_TEMP/check" | sort) <( (printf \'%s\\n\' "${files[@]}"; echo SHA256SUMS) | sort)\nfor f in "${files[@]}" SHA256SUMS; do cmp -- "$f" "$RUNNER_TEMP/check/$f"; done\ngh api -X PATCH "repos/${REPO}/releases/${id}" -F draft=false -F "prerelease=$PRERELEASE" -f "make_latest=$LATEST" -F "body=@RELEASE_NOTES.md" > /dev/null\n'}, {'name': "The beta channel's update indexes, when this release outranks every earlier tag (src/fw_update.h channel-beta)", 'env': {'GH_TOKEN': '${{ github.token }}', 'REPO': '${{ github.repository }}', 'SHA': '${{ github.sha }}', 'RANK': '${{ needs.prepare.outputs.rank }}', 'TOP': '${{ steps.floor.outputs.top }}'}, 'run': 'cd publish\nif [ "$RANK" -gt "$TOP" ]; then\n  if ! gh release view channel-beta --repo "$REPO" > /dev/null 2>&1; then\n    gh release create channel-beta --repo "$REPO" --prerelease --target "$SHA" --title "Beta-kanaal (update-index)" --notes "De update-index van het beta-kanaal. De firmware staat in de releases zelf."\n  fi\n  for f in orcon-update.json orcon-update-matter.json; do\n    if [ -f "$f" ]; then\n      gh release upload channel-beta --repo "$REPO" --clobber -- "$f"\n    fi\n  done\nfi\n'}],
}
REL_PINNED={   # the build steps that turn the version into the images, whole (the build job itself is re-checked by verify)
    ('build', 'Release build of the control firmware'):{'env': {'NUMBER': '${{ needs.prepare.outputs.number }}', 'PRE': '${{ needs.prepare.outputs.pre }}', 'SHA': '${{ github.sha }}'}, 'run': 'PLATFORMIO_BUILD_FLAGS="$(python -I tools/release_ci.py flags --version "$NUMBER" --pre "$PRE" --commit "$SHA")"\nexport PLATFORMIO_BUILD_FLAGS\npio run -e "$RELEASE_ENV"\npio pkg list -e "$RELEASE_ENV" | tee "$RUNNER_TEMP/packages.txt"\npython -I tools/release_ci.py packages --list "$RUNNER_TEMP/packages.txt"\n'},
    ('build', 'Release build of the Matter firmware (a beta only; the universal firmware, no identity, pinned ESP-Matter image)'):{'env': {'NUMBER': '${{ needs.prepare.outputs.number }}', 'PRE': '${{ needs.prepare.outputs.pre }}', 'SHA': '${{ github.sha }}'}, 'run': 'if [ "$PRE" -gt 0 ]; then\n  bash tools/fetch_idf_deps.sh\n  bash tools/build_idf.sh --release "$NUMBER" --pre "$PRE"\n  bash tools/release_matter_check.sh "$NUMBER" "$PRE" "$SHA"\nfi\n'},
    ('build', 'Check the image and prepare the signing input'):{'id': 'check', 'env': {'NUMBER': '${{ needs.prepare.outputs.number }}', 'PRE': '${{ needs.prepare.outputs.pre }}', 'SHA': '${{ github.sha }}'}, 'run': 'tc="$HOME/.platformio/packages/toolchain-xtensa-esp32/bin"\nmkdir -p "$RUNNER_TEMP/harness"\ng++ -std=c++17 -O1 -Wall -Wextra -Werror -I src -I ".pio/libdeps/$RELEASE_ENV/ArduinoJson/src" test/release_accept.cpp src/fw_update.cpp src/ota_policy.cpp -o "$RUNNER_TEMP/harness/release_accept"\npython -I tools/release_ci.py verify-build --build ".pio/build/$RELEASE_ENV" --version "$NUMBER" --pre "$PRE" --commit "$SHA" --harness "$RUNNER_TEMP/harness/release_accept" --nm "$tc/xtensa-esp32-elf-nm" --out dist/control --esptool "$HOME/.platformio/packages/tool-esptoolpy/esptool.py"\npython -I tools/check_elf_tx.py ".pio/build/$RELEASE_ENV/firmware.elf" "$RELEASE_ENV" "$tc"\nRELEASE_BUILD_DIR=".pio/build/$RELEASE_ENV" RELEASE_VERSION="$NUMBER" RELEASE_PRE="$PRE" RELEASE_COMMIT="$SHA" python tools/test_release_ci.py\npython -I tools/release_ci.py factory --dist dist --esptool "$HOME/.platformio/packages/tool-esptoolpy/esptool.py"\npython -I tools/release_ci.py manifest --dist dist\necho "firmware_sha256=$(sha256sum dist/control/firmware.bin | cut -d\' \' -f1)" >> "$GITHUB_OUTPUT"\necho "builds_sha256=$(cat dist/*/BUILD_INFO.json | sha256sum | cut -d\' \' -f1)" >> "$GITHUB_OUTPUT"\n'},
}
REL_OUTPUTS={'history':{'newest':'${{ steps.list.outputs.newest }}','betas':'${{ steps.list.outputs.betas }}'},
             'prepare':{k:'${{ steps.version.outputs.'+k+' }}' for k in ('number','pre','tag','name','rank','is_beta','latest')}}
# A script never reassigns a variable that carries the version, its inputs or its target (the environment binds it).
REL_BOUND='RELEASE_TYPE|CHANNEL|NOTES|NEWEST|BETAS|TAG|NUMBER|PRE|RANK|TOP|PRERELEASE|LATEST|REPO|SHA|GH_TOKEN|WANT|BUILDS|FW|SUMS|NOTES_SHA|RUN_URL|RELEASE_ENV'
REL_ASSIGN=re.compile(r'(^|[\s;(])((export|declare|typeset|readonly|local)\s+(-\w+\s+)*)?('+REL_BOUND+r')(\[[^]]*\])?\+?=|\b(read|mapfile|readarray)\b[^|;]*\b('+REL_BOUND+r')\b|\bprintf\s+(-\S+\s+)*-v\b|\b(declare|typeset|local)\s+-\w*n|\blet\s')
REL_ENV_BANNED={'BASH_ENV','ENV','SHELLOPTS','BASHOPTS','PROMPT_COMMAND','PS4','IFS','PATH','HOME','LD_PRELOAD','LD_LIBRARY_PATH','LD_AUDIT',
                'PYTHONPATH','PYTHONSTARTUP','PYTHONHOME','PYTHONUSERBASE','GH_HOST','GH_REPO','GH_ENTERPRISE_TOKEN','GITHUB_TOKEN',
                'GIT_CONFIG_GLOBAL','GIT_CONFIG_SYSTEM','GIT_CONFIG_PARAMETERS','GIT_CONFIG_COUNT','GIT_SSH_COMMAND','GIT_EXEC_PATH'}
REL_BANNED=('--help','()')
REL_BANNED_CMD=re.compile(r'(^|[;|]\s*)(alias|shopt|enable|set|unset|trap|eval|source|exec|builtin|command|\.)(\s|$)')
REL_PERMS={'history':{'contents':'write'},'prepare':{'contents':'read'},'build':{'contents':'read'},'sign':{'contents':'read'},'verify':{'contents':'read'},'publish':{'contents':'write'}}
# Every variable that carries the version, its inputs or its target has exactly one source (Codex auto-version r1).
REL_BIND={'RELEASE_TYPE':'${{ inputs.release_type }}','NOTES':'${{ inputs.notes }}','NEWEST':'${{ needs.history.outputs.newest }}',
          'TAG':'${{ needs.prepare.outputs.tag }}','NUMBER':'${{ needs.prepare.outputs.number }}','REPO':'${{ github.repository }}','SHA':'${{ github.sha }}',
          'CHANNEL':'${{ inputs.channel }}','BETAS':'${{ needs.history.outputs.betas }}','PRE':'${{ needs.prepare.outputs.pre }}',
          'RANK':'${{ needs.prepare.outputs.rank }}','TOP':'${{ steps.floor.outputs.top }}','PRERELEASE':'${{ needs.prepare.outputs.is_beta }}',
          'LATEST':'${{ needs.prepare.outputs.latest }}','BUILDS':'${{ needs.build.outputs.builds_sha256 }}'}
REL_NEEDS={'history':None,'prepare':'history','build':'prepare','sign':['prepare','build'],'verify':['prepare','build','sign'],'publish':['prepare','build','sign','verify']}
def release_workflow_safe(text):
    d=yaml_subset(text)
    assert set(d)=={'name','on','permissions','concurrency','defaults','env','jobs'},('top-level keys',sorted(d))
    assert list(d['on'])==['workflow_dispatch'],('trigger: manual workflow_dispatch only',d['on'])
    inputs=d['on']['workflow_dispatch']['inputs']
    # Only the kind of release and its channel are chosen; the version itself follows from the tags and releases (release_ci.py next).
    assert set(inputs)=={'release_type','channel','notes'},('inputs: release type, channel and notes only (no version)',sorted(inputs))
    assert {k:v for k,v in inputs['channel'].items() if k!='description'}=={'required':True,'type':'choice','default':'stable','options':['stable','beta']},('channel: stable (default) or beta',inputs['channel'])
    assert {k:v for k,v in inputs['release_type'].items() if k!='description'}=={'required':True,'type':'choice','default':'patch','options':['patch','minor','major']},('release type: patch (default), minor or major',inputs['release_type'])
    assert {k:v for k,v in inputs['notes'].items() if k!='description'}=={'required':False,'type':'string','default':''},('notes: optional text',inputs['notes'])
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
        assert j.get('if') in ((REL_GUARD,) if name in ('history','prepare','sign') else (None,)),(name,'job guard',j.get('if'))
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
                    assert not re.search(r'(?<![\w./-])[\'"\\]*gh[\'"]*(\s|$|;|\))',l) or l in REL_GH_JOB.get(name,set()),(name,'GitHub CLI call outside the reviewed set of this job',l)
                    assert not REL_ASSIGN.search(l),(name,'a script reassigns a bound variable',l)
            assert 'secrets' not in str(st.get('with')).lower() and 'github.token' not in str(st.get('with')),(name,'credential in an action input (Codex r5)')
            for k,v in (st.get('env') or {}).items():
                assert ('secrets' in str(v).lower())==(name=='sign' and k=='ORCON_OTA_SIGNING_KEY'),(name,'secret outside the signing step',k)
                assert 'github.token' not in str(v) or (k=='GH_TOKEN' and name in ('history','publish')),(name,'token outside history/publish',k)
                assert k not in REL_BIND or v==REL_BIND[k],(name,'binding: one source for every version input and target',k,v)
                assert k not in REL_ENV_BANNED,(name,'a variable that changes how a shell or tool starts',k)
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
    assert [l.strip() for l in flat.split('\n') if 'pio run' in l]==['pio run -e "$RELEASE_ENV"'],'exactly one PlatformIO build: the control env'
    # The Matter firmware: built once, universal (never with an identity), and checked by its own script in the pinned image.
    assert [l.strip() for l in flat.split('\n') if 'build_idf' in l]==['bash tools/build_idf.sh --release "$NUMBER" --pre "$PRE"'],'exactly one ESP-IDF build: the universal Matter firmware of a beta'
    assert not any(t in flat for t in ('--identity','ORCON_MATTER_IDENTITY','.orcon-matter','orcon_matter_creds','docker ')),'a release never builds or handles a Matter identity, and runs no container of its own'
    mat=[st['run'] for st in jobs['build']['steps'] if 'build_idf' in st.get('run','')]
    assert mat==['if [ "$PRE" -gt 0 ]; then\n  bash tools/fetch_idf_deps.sh\n  bash tools/build_idf.sh --release "$NUMBER" --pre "$PRE"\n  bash tools/release_matter_check.sh "$NUMBER" "$PRE" "$SHA"\nfi\n'],('the Matter build: a beta only, built and checked',mat)
    pub=[st['run'] for st in jobs['publish']['steps'] if 'channel-beta' in st.get('run','')]
    assert len(pub)==1 and pub[0].startswith('cd publish\nif [ "$RANK" -gt "$TOP" ]; then\n') and pub[0].count('gh release upload')==1,'the beta channel moves only to a release that outranks every earlier tag'
    assert not any('uses' in st and st['uses'].startswith('actions/checkout@') for st in jobs['publish']['steps']),'publish runs no repository code'
    assert all('run' in st for st in jobs['history']['steps']),'history (write token, for the drafts) runs no action and no repository code'
    for job,want in REL_OUTPUTS.items(): assert jobs[job].get('outputs')==want,(job,'outputs: exactly the version hand-over',jobs[job].get('outputs'))
    for job,want in REL_JOBS.items(): assert jobs[job]['steps']==want,(job,'the reviewed job: every step in order, with its action, inputs, env and script')
    assert not any('GITHUB_ENV' in st.get('run','') for j in jobs.values() for st in j['steps']),'no step sets the environment of later steps ($GITHUB_ENV)'
    for (job,sname),want in REL_PINNED.items():
        got=[{k:st[k] for k in ('id','env','run') if k in st} for st in jobs[job]['steps'] if st.get('name')==sname]
        assert got==[want],(job,'the reviewed step, whole (id, environment, script)',sname)
    assert d['env']=={'RELEASE_ENV':'heltec_v2_sx1276_control','PYTHONDONTWRITEBYTECODE':'1','PIP_DISABLE_PIP_VERSION_CHECK':'1'},('workflow env: exactly as reviewed',d['env'])
    assert all('run' in st or st['uses'].split('@')[0] in ('actions/checkout','actions/upload-artifact','actions/download-artifact') for st in jobs['sign']['steps']),'signing job: no setup actions'
WORKFLOWS={'heltec-v2-rx-only.yml':'fbf3af64a87be3822246c0adc17ba3cb5ec42c1ddb725c455153f8fd4ea7a563',   # file -> sha256 of the reviewed content
           'release-control.yml':'d4bf5a8e81fbdf4f134f4c8843532af9f121c6eb1e446782a6cf2c0454f83955',
           # Matter checks (pull requests): contents read, no secrets, no artifact; the placeholder identity only
           'matter.yml':'bf4ede81add9bbe08432924a7f4c1c4b71f930870768531e11e7c8f2d499a613'}
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
  'secret-in-history': rel_mut('          GH_TOKEN: ${{ github.token }}\n','          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}\n'),
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
  'expression-in-script': rel_mut('-- "$RELEASE_TYPE" >> "$GITHUB_OUTPUT"','-- "${{ inputs.release_type }}" >> "$GITHUB_OUTPUT"'),
  'continue-on-error': rel_mut('      - name: Host tests\n','      - name: Host tests\n        continue-on-error: true\n'),
  'dry-run-image': rel_mut('pio run -e "$RELEASE_ENV"','pio run -e heltec_v2_sx1276_tx_poc_dryrun'),
  'rf-test-image': rel_mut('  RELEASE_ENV: heltec_v2_sx1276_control\n','  RELEASE_ENV: heltec_v2_sx1276_rftest\n'),
  'rx-only-image': rel_mut('  RELEASE_ENV: heltec_v2_sx1276_control\n','  RELEASE_ENV: heltec_v2_sx1276\n'),
  'second-build': rel_mut('          pio run -e "$RELEASE_ENV"\n','          pio run -e "$RELEASE_ENV"\n          pio run -e heltec_v2_sx1276\n'),
  'skip-build-checks': rel_mut('          python -I tools/release_ci.py verify-build','          true'),
  'skip-old-workflow-guards': rel_mut('          python tools/test_tx_poc_safety.py\n',''),
  'skip-package-verify': rel_mut('          python3 -I tools/release_ci.py verify-package --dist dist --harness harness/release_accept --release "$TAG" --official --notes="$NOTES"\n',''),
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
  'other-expression': rel_mut('          RELEASE_TYPE: ${{ inputs.release_type }}\n','          RELEASE_TYPE: ${{ github.event.inputs.release_type }}\n'),
  'no-asan': rel_mut('          CXXFLAGS: -fsanitize=address,undefined -fno-sanitize-recover=all\n','          CXXFLAGS: -fsanitize=undefined -fno-sanitize-recover=all\n'),
  'no-pip-check': rel_mut('          "$RUNNER_TEMP/pio-venv/bin/python" -m pip check\n',''),
  'bash-template-dropped': rel_mut('    shell: bash\n','    shell: bash {0}\n'),
  'step-shell': rel_mut('      - name: Host tests\n        env:','      - name: Host tests\n        shell: sh\n        env:'),
  'pip-check-help': rel_mut('          "$RUNNER_TEMP/pio-venv/bin/python" -m pip check\n','          "$RUNNER_TEMP/pio-venv/bin/python" -m pip check --help\n'),
  'if-false-wrapper': rel_mut('          python3 -I tools/release_ci.py verify-package','          if false; then\n          python3 -I tools/release_ci.py verify-package').replace('--official --notes="$NOTES"\n','--official --notes="$NOTES"\n          fi\n',1),
  'verify-help': rel_mut('--official --notes="$NOTES"\n','--official --notes="$NOTES" --help\n'),
  'function-redefine': rel_mut('          chmod 0755 harness/release_accept\n','          python3() { :; }\n          chmod 0755 harness/release_accept\n'),
  'bare-cr': rel_mut('      - name: Host tests\n','      - name: Host tests\n        # Host step configuration\r        if: false\n'),
  'no-regeneration': rel_mut(' --out dist/control --esptool "$HOME/.platformio/packages/tool-esptoolpy/esptool.py"\n',' --out dist/control\n'),
  'no-force-reinstall': rel_mut(' --force-reinstall -r tools/release',' -r tools/release'),
  'set-plus-e-after-pipe': rel_mut('          python tools/test_orcon_cli.py\n','          true | set +e\n          python tools/test_orcon_cli.py\n'),
  'source-script': rel_mut('          python tools/test_orcon_cli.py\n','          . ./evil.sh\n          python tools/test_orcon_cli.py\n'),
  'promote-current-draft': rel_mut('          gh api -X PATCH "repos/${REPO}/releases/${id}" -F draft=false -F "prerelease=$PRERELEASE" -f "make_latest=$LATEST" -F "body=@RELEASE_NOTES.md" > /dev/null\n','          gh release edit "$TAG" --repo "$REPO" --draft=false --latest\n'),
  'draft-not-compared': rel_mut('          for f in "${files[@]}" SHA256SUMS; do cmp -- "$f" "$RUNNER_TEMP/check/$f"; done\n',''),
  'no-version-floor': rel_mut('          if [ "$floor" -ge "$NUMBER" ]; then\n','          if [ "$floor" -gt "999999999" ]; then\n'),
  'publish-before-compare': rel_mut('          gh release download "$TAG" --repo "$REPO" --dir "$RUNNER_TEMP/check"\n','          gh release edit "$TAG" --repo "$REPO" --draft=false --latest\n          gh release download "$TAG" --repo "$REPO" --dir "$RUNNER_TEMP/check"\n'),
  'unchecked-sums': rel_mut('          test "$(sha256sum SHA256SUMS | cut -d\' \' -f1)" = "$SUMS"\n',''),
  'quoted-apostrophe': rel_mut("        description: 'Soort release (de versie volgt uit de bestaande tags en releases): patch, minor of major'","        description: 'Soort release''s: patch, minor of major'"),
  'floor-zero': rel_mut('END { print m + 0 }','END { print 0 }'),
  'no-asset-inventory': rel_mut('          cmp <(ls "$RUNNER_TEMP/check" | sort) <( (printf \'%s\\n\' "${files[@]}"; echo SHA256SUMS) | sort)\n',''),
  'no-break-space': rel_mut('    environment: orcon-production-release\n','    environment: orcon-production-release\u00a0\n'),
  'secret-as-action-input': rel_mut('        with:\n          name: release-signed\n','        with:\n          name: release-signed\n          token: ${{ secrets.ORCON_OTA_SIGNING_KEY }}\n').replace('          ORCON_OTA_SIGNING_KEY: ${{ secrets.ORCON_OTA_SIGNING_KEY }}\n','          ORCON_OTA_SIGNING_KEY: x\n'),
  'nested-tag-name': rel_mut("| cut -f2 | sed -n 's#^refs/tags/##p'","| sed -n 's#^.*refs/tags/##p'"),
  'duplicate-key': rel_mut('  PYTHONDONTWRITEBYTECODE: \'1\'\n','  PYTHONDONTWRITEBYTECODE: \'1\'\n  RELEASE_ENV: heltec_v2_sx1276_rftest\n'),
  # The version is never typed in again, and only the three steps exist, patch by default.
  'version-input-back': rel_mut('      notes:\n','      version:\n        description: \'Versie\'\n        required: false\n        type: string\n      notes:\n'),
  'release-type-free-text': rel_mut('        type: choice\n        default: patch\n','        type: string\n        default: patch\n'),
  'release-type-extra-option': rel_mut('          - major\n','          - major\n          - 1.0.0\n'),
  'release-type-default-major': rel_mut('        default: patch\n','        default: major\n'),
  'release-type-optional': rel_mut('        required: true\n        type: choice\n','        required: false\n        type: choice\n'),
  'bash-env': rel_mut("  PIP_DISABLE_PIP_VERSION_CHECK: '1'\n","  PIP_DISABLE_PIP_VERSION_CHECK: '1'\n  BASH_ENV: /tmp/probe.sh\n"),
  'step-bash-env': rel_mut('          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n','          HOST_TEST_OUTPUT_DIR: ${{ runner.temp }}/host-plain\n          BASH_ENV: /tmp/probe.sh\n'),
  'draft-id-reassigned': rel_mut("          printf '%s' \"$id\" | grep -qxE '[0-9]+'\n","          id=12345\n          printf '%s' \"$id\" | grep -qxE '[0-9]+'\n"),
  'draft-assets-reduced': rel_mut("          mapfile -t files < <(cut -d' ' -f3- SHA256SUMS)\n","          files=(firmware.bin)\n"),
  'tag-array-assign': rel_mut('          python3 -I tools/release_ci.py verify-package','          TAG[0]=v9.9.9\n          python3 -I tools/release_ci.py verify-package'),
  'printf-v-indirect': rel_mut('          PLATFORMIO_BUILD_FLAGS=','          t=NUMBER\n          printf -v "$t" %s 1000001\n          PLATFORMIO_BUILD_FLAGS='),
  'quoted-gh': rel_mut('          python tools/test_orcon_cli.py\n',"          python tools/test_orcon_cli.py\n          'gh' --version\n"),
  'publish-extra-step': rel_mut('      - name: Exactly the verified files\n','      - name: Extra\n        env:\n          GH_TOKEN: ${{ github.token }}\n        run: gh --version\n      - name: Exactly the verified files\n'),
  'github-env-bash-env': rel_mut('          python tools/test_orcon_cli.py\n','          python tools/test_orcon_cli.py\n          echo "BASH_ENV=/tmp/probe.sh" >> "$GITHUB_ENV"\n'),
  'sign-tool-swapped': rel_mut('      - name: Sign the release manifest (the only step that sees the key)\n','      - name: Tools\n        run: cp /tmp/other.py tools/release_ci.py\n      - name: Sign the release manifest (the only step that sees the key)\n'),
  'sign-hash-check-removed': rel_mut('          test -n "$WANT"\n          test "$(sha256sum dist/control/firmware.bin | cut -d\' \' -f1)" = "$WANT"\n','          test -n "$WANT"\n'),
  'sums-truncated': rel_mut('      - name: Draft release, check its files, then publish (a beta as a pre-release, never "latest")\n','      - name: Sums\n        run: head -n 1 publish/SHA256SUMS > /tmp/s\n      - name: Draft release, check its files, then publish (a beta as a pre-release, never "latest")\n'),
  'prepare-helper-edited': rel_mut('      - name: The next version after every tag and release\n','      - name: Helper\n        run: sed -i s/c + 1/c + 2/ tools/release_ci.py\n      - name: The next version after every tag and release\n'),
  'version-not-from-history': rel_mut('next --newest "$NEWEST"','next --newest 1000000'),
  'history-without-drafts': rel_mut('    timeout-minutes: 5\n    permissions:\n      contents: write\n','    timeout-minutes: 5\n    permissions:\n      contents: read\n'),
  'history-fetch-removed': rel_mut('            gh api --paginate "repos/${REPO}/releases" --jq \'.[].tag_name\'\n          } > "$RUNNER_TEMP/tags.txt"\n          newest=','          } > "$RUNNER_TEMP/tags.txt"\n          newest='),
  'history-runs-code': rel_mut('      - name: Every tag and every release, drafts included (no repository code)\n','      - name: Code\n        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1\n        with:\n          ref: ${{ github.sha }}\n          persist-credentials: false\n      - name: Every tag and every release, drafts included (no repository code)\n'),
  'history-unused': rel_mut('          NEWEST: ${{ needs.history.outputs.newest }}\n','          NEWEST: 1000000\n'),
  'history-writes-tag': rel_mut('          newest="$(awk','          gh api -X POST "repos/${REPO}/git/refs" -f "ref=refs/tags/${TAG}" -f "sha=${SHA}" > /dev/null\n          newest="$(awk'),
  'history-output-hardcoded': rel_mut('      newest: ${{ steps.list.outputs.newest }}\n',"      newest: '1000000'\n"),
  'history-list-overwritten': rel_mut('          newest="$(awk','          echo v1.0.0 > "$RUNNER_TEMP/tags.txt"\n          newest="$(awk'),
  'history-other-rule': rel_mut('          newest="$(awk \'/^v?','          newest="$(awk \'/^v'),
  'type-overridden-in-script': rel_mut('          python3 -I tools/release_ci.py next --newest','          RELEASE_TYPE=major\n          python3 -I tools/release_ci.py next --newest'),
  'publish-tag-overridden': rel_mut('          REPO: ${{ github.repository }}\n          TAG: ${{ needs.prepare.outputs.tag }}\n          NUMBER:','          REPO: ${{ github.repository }}\n          TAG: v9.9.9\n          NUMBER:'),
  'publish-tag-in-script': rel_mut('          gh release create "$TAG"','          TAG=v9.9.9\n          gh release create "$TAG"'),
  'publish-release-lookup-removed': rel_mut('            gh api --paginate "repos/${REPO}/releases" --jq \'.[].tag_name\'\n          } > "$RUNNER_TEMP/tags.txt"\n          floor=','          } > "$RUNNER_TEMP/tags.txt"\n          floor='),
  'verify-tag-in-script': rel_mut('          python3 -I tools/release_ci.py verify-package','          TAG="v$(python3 -c \'import json;print(json.load(open("dist/BUILD_INFO.json"))["name"])\')"\n          python3 -I tools/release_ci.py verify-package'),
  'build-number-in-script': rel_mut('          PLATFORMIO_BUILD_FLAGS=','          NUMBER=1000001\n          PLATFORMIO_BUILD_FLAGS='),
  'export-tag': rel_mut('          gh release create "$TAG"','          export TAG=v9.9.9\n          gh release create "$TAG"'),
  'read-number': rel_mut('          PLATFORMIO_BUILD_FLAGS=','          read -r NUMBER < /dev/null\n          PLATFORMIO_BUILD_FLAGS='),
  'release-type-hardcoded': rel_mut('          RELEASE_TYPE: ${{ inputs.release_type }}\n','          RELEASE_TYPE: major\n'),
  'verify-tag-rebound': rel_mut('          FW: ${{ needs.build.outputs.firmware_sha256 }}\n          TAG: ${{ needs.prepare.outputs.tag }}\n','          FW: ${{ needs.build.outputs.firmware_sha256 }}\n          TAG: v9.9.9\n'),
  'number-hardcoded': rel_mut('          NUMBER: ${{ needs.prepare.outputs.number }}\n','          NUMBER: 1000001\n'),
  'tag-at-other-commit': rel_mut('          PRE: ${{ needs.prepare.outputs.pre }}\n          SHA: ${{ github.sha }}\n        run: |\n          {','          PRE: ${{ needs.prepare.outputs.pre }}\n          SHA: ${{ needs.build.outputs.firmware_sha256 }}\n        run: |\n          {'),
  'type-as-option': rel_mut('--notes="$NOTES" -- "$RELEASE_TYPE"','--notes="$NOTES" "$RELEASE_TYPE"'),
  'history-other-repo': rel_mut('          REPO: ${{ github.repository }}\n          GH_TOKEN: ${{ github.token }}\n        run: |\n          {','          REPO: someone/orcon-15rf-controller\n          GH_TOKEN: ${{ github.token }}\n        run: |\n          {'),
  'package-tag-unchecked': rel_mut(' --release "$TAG" --official',' --official'),
  # the beta channel and the Matter variant
  'beta-as-latest': rel_mut('          LATEST: ${{ needs.prepare.outputs.latest }}\n',"          LATEST: 'true'\n"),
  'beta-not-prerelease': rel_mut('-F "prerelease=$PRERELEASE" ','-F prerelease=false '),
  'channel-default-beta': rel_mut('        default: stable\n','        default: beta\n'),
  'matter-with-identity': rel_mut('bash tools/build_idf.sh --release "$NUMBER" --pre "$PRE"','bash tools/build_idf.sh --identity "$HOME/.orcon-matter" --release "$NUMBER" --pre "$PRE"'),
  'matter-identity-env': rel_mut('          PRE: ${{ needs.prepare.outputs.pre }}\n          SHA: ${{ github.sha }}\n        run: |\n          if [ "$PRE"','          PRE: ${{ needs.prepare.outputs.pre }}\n          SHA: ${{ github.sha }}\n          ORCON_MATTER_IDENTITY: /tmp/id\n        run: |\n          if [ "$PRE"'),
  'matter-unchecked': rel_mut('            bash tools/release_matter_check.sh "$NUMBER" "$PRE" "$SHA"\n',''),
  'matter-in-stable': rel_mut('          if [ "$PRE" -gt 0 ]; then\n            bash tools/fetch_idf_deps.sh','          if [ "$PRE" -ge 0 ]; then\n            bash tools/fetch_idf_deps.sh'),
  'own-container': rel_mut('            bash tools/release_matter_check.sh "$NUMBER" "$PRE" "$SHA"\n','            bash tools/release_matter_check.sh "$NUMBER" "$PRE" "$SHA"\n            docker run --rm espressif/esp-matter:latest true\n'),
  'channel-without-rank': rel_mut('          if [ "$RANK" -gt "$TOP" ]; then\n','          if [ "$RANK" -gt 0 ]; then\n'),
  'beta-floor-skipped': rel_mut('              echo "::error::$TAG: beta $pfloor of this version exists already: a beta number is published once"; exit 1\n','              echo "beta $pfloor exists"\n'),
  'builds-unchecked': rel_mut('          test "$(cat dist/*/BUILD_INFO.json | sha256sum | cut -d\' \' -f1)" = "$BUILDS"\n',''),
  'top-hardcoded': rel_mut('          TOP: ${{ steps.floor.outputs.top }}\n','          TOP: 0\n'),
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
