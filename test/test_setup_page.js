// Run the setup wizard's real script (src/setup_page.html) against a scripted fake API.
// Usage: node test/test_setup_page.js            (no dependencies; DOM is a small stub)
// Checks: start screen, navigation, key states, RX-only gating, API errors, lost
// connection, cancel/restart, and that nothing turns green without a server result.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '..', 'src', 'setup_page.html'), 'utf8');
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise(r => setImmediate(r)); };

function el(attrs) {
    const cls = new Set((attrs.class || '').split(/\s+/).filter(Boolean));
    const e = {
        attrs, hidden: 'hidden' in attrs, disabled: 'disabled' in attrs, textContent: '', innerHTML: '', style: {},
        dataset: {}, handlers: {}, children: {},
        get className() { return [...cls].join(' '); }, set className(v) { cls.clear(); v.split(/\s+/).filter(Boolean).forEach(c => cls.add(c)); },
        classList: { toggle: (c, f) => { if (f === undefined) f = !cls.has(c); f ? cls.add(c) : cls.delete(c); }, contains: c => cls.has(c) },
        setAttribute(k, v) { attrs[k] = String(v); },
        addEventListener(t, f) { (e.handlers[t] = e.handlers[t] || []).push(f); },
        click(ev) { (e.handlers.click || []).forEach(f => f(ev || {})); },
        querySelector(s) { return e.children[s] || (e.children[s] = el({})); },
    };
    e.lastChild = { textContent: '' };
    for (const k in attrs) if (k.startsWith('data-')) e.dataset[k.slice(5)] = attrs[k];
    return e;
}

function load() {
    const byId = {}, all = [];
    for (const m of html.matchAll(/<(\w+)((?:\s+[\w-]+(?:="[^"]*")?)*)\s*>/g)) {
        if (m[1] === 'html' || m[1] === 'meta') continue;
        const attrs = {};
        for (const a of m[2].matchAll(/([\w-]+)(?:="([^"]*)")?/g)) attrs[a[1]] = a[2] ?? '';
        const e = el(attrs); all.push(e);
        if (attrs.id) byId[attrs.id] = e;
    }
    return { byId, all };
}

// Browser dialogs: answers set per test, every question recorded.
const dlg = { asked: [], confirm: true, prompt: null };
// Page clock: tests move it forward explicitly (automatic save retries wait for it).
let clock = 1e12;
class PageDate extends Date { static now() { return clock; } }
// routes: { 'GET /api/x': () => [status, body] | throws }
async function page(routes, hash = '') {
    const dom = load(), calls = [], win = {};
    const document = {
        hidden: false, handlers: {},
        getElementById: id => dom.byId[id],
        querySelectorAll: sel => { const a = sel.slice(1, -1); return dom.all.filter(e => a in e.attrs); },
        addEventListener(t, f) { this.handlers[t] = f; },
    };
    const location = { _h: hash, get hash() { return this._h ? '#' + this._h : ''; }, set hash(v) { this._h = String(v).replace(/^#/, ''); } };
    const fetch = async (url, o = {}) => {
        const key = (o.method || 'GET') + ' ' + url;
        calls.push({ key, headers: o.headers || {}, body: o.body ? JSON.parse(o.body) : undefined });
        const h = routes[key] || routes[key.split('?')[0]];
        if (!h) return { ok: false, status: 404, json: async () => ({ error: 'no-route' }) };
        const [status, body] = h(calls[calls.length - 1]);
        return { ok: status < 300, status, json: async () => body };
    };
    const ctx = { document, location, fetch, setTimeout: () => 0, clearTimeout() {}, addEventListener: (t, f) => { win[t] = f; },
                  console, Promise, Set, String, JSON, Math, Object, Array, Date: PageDate,
                  confirm: m => { dlg.asked.push(m); return dlg.confirm; }, prompt: m => { dlg.asked.push(m); return dlg.prompt; } };
    vm.runInNewContext(script, ctx);
    await flush();
    const $ = id => dom.byId[id];
    return { $, calls, dom, location, keys: dom.all.filter(e => 'data-key' in e.attrs),
             refresh: async () => { document.handlers.visibilitychange(); await flush(); },
             posts: () => calls.filter(c => c.key.startsWith('POST')) };
}

const CAPS_RX = { rx_supported: true, tx_supported: false, read_supported: true, pair_supported: false, control_supported: false };
const CAPS_TX = { rx_supported: true, tx_supported: true, read_supported: true, pair_supported: true, control_supported: true };
const state = (caps, extra = {}) => ({ fw: 'test', rx_only: !caps.tx_supported, caps, rf: 'receiving', token: 'tok123', mask_ids: true,
    active: 'none', unit: { known: true, id: '29:000202', mode: 'low', age_s: 4 },
    remotes: [{ id: '29:000101', age_s: 3, rssi: -61, cmd: '22F1', n: 4 }], ...extra });
const steps = (n, extra = {}) => ['away', 'auto', 'timer', '1', '2', '3'].map((k, i) => i < n
    ? { key: k, ok: true, cmd: k === 'timer' ? '22F3' : '22F1', pl: '0001', rssi: -60, ev: i + 1 } : { key: k, ok: false });
const read = (sid, status, step, extra = {}) => ({ sid, status, step, remote: '29:000101', left_s: 50, ev: 9, rf: 'receiving', others: 0,
    why: '', why_ev: 0, why_cmd: '0000', why_mode: -1, steps: steps(step), ...extra });

const PF=(items,active)=>({storage_ok:true,active,limit:4,items});
const PI=(id,name,remote,cm=true)=>({id,name,remote,unit:'29:000202',control_mapping:cm,paired:false});
const pickRow = (p, d) => p.$('plist').click({ target: { closest: () => ({ dataset: d }) } });
(async () => {
    let n = 0; const ok = m => { n++; console.log('  [ ok ] ' + m); };

    // ---- start: no profiles -> welcome; existing procedures stay reachable; RX-only gating --------------
    {
        const p = await page({ 'GET /api/setup/state': () => [200, state(CAPS_RX, { profiles: PF([], 0) })] });
        assert.equal(p.location.hash, '#welcome'); assert.equal(p.$('wel').hidden, false); assert.equal(p.$('dash').hidden, true);
        assert.equal(p.$('back').hidden, true); assert.equal(p.$('title').textContent, 'Welkom bij Orcon 15RF'); assert.ok(p.$('lay').classList.contains('solo'));
        ok('no profiles: welcome with two choices, no back button, remote hidden');
        assert.equal(p.$('conn').textContent, 'Verbonden · alleen ontvangen'); assert.ok(p.$('conn').classList.contains('ok'));
        ok('header shows the controller connection (RX-only)');
        assert.match(p.$('simSub').textContent, /Niet beschikbaar met deze firmware/); ok('simulation marked unavailable on RX-only');
        { const q = await page({ 'GET /api/setup/state': () => [200, state(CAPS_TX, { profiles: PF([], 0) })] }, 'welcome');
          assert.match(q.$('simSub').textContent, /Experimenteel: maakt nog geen profiel aan/); assert.doesNotMatch(q.$('simSub').textContent, /Niet beschikbaar/); }
        ok('simulation where pairing exists: marked experimental, says it creates no profile');
        p.dom.all.find(e => e.dataset.open === 'pair').click(); await p.refresh();
        assert.equal(p.$('pairA').disabled, true); assert.match(p.$('notice').textContent, /Koppelen is met deze firmware niet beschikbaar/);
        p.$('pairA').click(); await flush(); assert.equal(p.posts().length, 0);
        ok('"Simulatie instellen" opens the existing pair procedure: visibly unavailable, nothing sent');
        assert.equal(p.$('back').hidden, false); p.$('back').click(); await flush(); assert.equal(p.location.hash, '#welcome');
        p.dom.all.find(e => e.dataset.open === 'read').click(); await p.refresh();
        assert.equal(p.location.hash, '#read'); assert.equal(p.$('readP').hidden, false); assert.equal(p.$('title').textContent, 'Afstandsbediening koppelen');
        ok('"Afstandsbediening koppelen" opens the existing read procedure; back returns to welcome');
    }
    {
        const p = await page({ 'GET /api/setup/state': () => [200, state(CAPS_RX, { profiles: PF([PI(2, 'Washok', '29:000101')], 2) })] });
        assert.equal(p.location.hash, '#control'); assert.equal(p.$('dash').hidden, false); assert.equal(p.$('wel').hidden, true);
        assert.equal(p.$('profName').textContent, 'Washok'); assert.equal(p.$('profSub').textContent, 'Actief profiel'); assert.ok(p.$('cProf').classList.contains('on'));
        ok('profiles present: straight to the dashboard with the active profile name');
        assert.equal(p.$('ctlP').hidden, false); assert.match(p.$('notice').textContent, /niet mogelijk/);
        assert.ok(p.keys.every(k => k.disabled)); p.keys[3].click(); await flush();
        assert.equal(p.posts().length, 0); assert.ok(!p.calls.some(c => c.key.includes('/api/remote/')));
        ok('dashboard on RX-only: notice, keys disabled, no remote API call');
        const p2 = await page({ 'GET /api/setup/state': () => [200, state(CAPS_RX, { profiles: PF([PI(2, 'Washok', '29:000101')], 7) })] });
        assert.equal(p2.location.hash, '#profiles'); assert.match(p2.$('profM').textContent, /Geen actief profiel/);
        ok('stored active reference invalid: the user chooses on the profile screen (no wizard)');
    }
    {   // dashboard cards: WiFi status, Siri placeholder, system page; classic pages absent on web-control builds
        let wifi = { connected: true, ssid: 'Thuis <5G>' }, caps = { ...CAPS_RX, settings_pages: true };
        const p = await page({ 'GET /api/setup/state': () => [200, state(caps, { wifi, profiles: PF([], 0) })] }, 'control');
        assert.equal(p.$('wifiSub').textContent, 'Verbonden met Thuis <5G>'); assert.ok(!p.$('cWifi').classList.contains('off'));
        assert.equal(p.$('profName').textContent, 'Geen profiel geselecteerd'); assert.ok(!p.$('cProf').classList.contains('on'));
        ok('WiFi card shows the network as text; no profile -> "Geen profiel geselecteerd"');
        assert.ok(p.dom.all.some(e => e.attrs.class === 'choice card off' && e.attrs['aria-disabled'] === 'true')); ok('Siri is a disabled placeholder');
        assert.equal(p.$('sysSub').textContent, 'Firmware, herstarten en informatie'); assert.equal(p.$('cSys').dataset.open, 'system'); ok('Systeem card opens the in-app Systeem screen');
        assert.equal(p.$('tech').hidden, false); assert.ok(!('open' in p.$('tech').attrs)); assert.match(html, /<details class="tech" id="tech" hidden><summary><span>Technische status tonen<\/span>/);
        assert.ok(html.indexOf('id="mins"') < html.indexOf('id="tech"') && html.indexOf('id="tech"') < html.indexOf('id="ctlP"') && html.indexOf('id="ctlP"') < html.indexOf('</details>', html.indexOf('id="tech"')) && html.indexOf('id="ctlP"') < html.indexOf('id="notice"'));
        ok('technical status: collapsed by default under the remote, no longer a dashboard card');
        wifi = { connected: false, ssid: '' }; caps = { ...CAPS_RX, settings_pages: false }; await p.refresh();
        assert.equal(p.$('wifiSub').textContent, 'Niet verbonden'); assert.ok(p.$('cWifi').classList.contains('off') && p.$('cSys').classList.contains('off'));
        assert.match(p.$('sysSub').textContent, /Niet beschikbaar/); ok('no WiFi -> "Niet verbonden"; no classic pages -> cards disabled');
        p.$('cProf').click(); await flush(); assert.equal(p.location.hash, '#profiles'); assert.equal(p.$('back').hidden, false);
        p.$('back').click(); await flush(); assert.equal(p.location.hash, '#control'); ok('profile card opens Profielen; back returns to the dashboard');
        const q = await page({ 'GET /api/setup/state': () => [200, state(CAPS_RX, { profiles: PF([PI(2, 'Washok', '29:000101')], 2), caps: { ...CAPS_RX, settings_pages: false } })] }, 'system');
        assert.equal(q.location.hash, '#control'); assert.ok(!q.calls.some(c => c.key === 'GET /api/state' || c.key === 'GET /api/netcfg'));
        ok('settings screens absent on builds without the classic settings APIs (opened by URL -> dashboard)');
    }
    {   // WiFi + Systeem screens: in-app, back on every sub-screen, existing APIs only, confirmation before a reboot
        let x = { uptime_ms: 90061000, heap_min: 171008, rf_frames: 42, oled_available: true, oled_mode: 'auto', oled_on: false, mqtt_on: false, mqtt_connected: false };
        const nc = { ssid: 'Thuis <5G>', mqtt: '', mqtt_on: false }, wifiPosts = [], oledPosts = [];
        const wifi = { connected: true, ap: false, ap_name: 'Orcon15RF-Setup', ip: '192.0.2.5', ssid: 'Thuis <5G>' };
        const p = await page({
            'GET /api/setup/state': () => [200, state({ ...CAPS_RX, settings_pages: true }, { wifi, fw: '0123456789abcdef0123456789abcdef01234567', profiles: PF([PI(2, 'Washok', '29:000101')], 2) })],
            'GET /api/state': () => [200, x], 'GET /api/netcfg': () => [200, nc],
            'GET /api/scan': () => [200, [{ ssid: 'Buren', rssi: -80, lock: true }, { ssid: 'Thuis <5G>', rssi: -50, lock: true }, { ssid: 'Buren', rssi: -60, lock: false }, { ssid: '', rssi: -40, lock: false }]],
            'POST /api/wifi': c => { wifiPosts.push(c.body); return [200, { ok: 1 }]; },
            'POST /api/oled': c => { oledPosts.push(c.body); if (c.body.mode) x = { ...x, oled_mode: c.body.mode }; return [200, { ok: 1 }]; },
            'POST /api/setup/restart': () => [200, { ok: true, restarting: true }],
            'POST /api/setup/wifi/forget': () => [200, { ok: true, restarting: true }],
        }, 'control');
        const open = async k => { p.dom.all.find(e => e.dataset.open === k).click(); await flush(); };
        const back = async () => { p.$('back').click(); await flush(); };
        await open('wifi');
        assert.equal(p.location.hash, '#wifi'); assert.equal(p.$('title').textContent, 'WiFi'); assert.equal(p.$('back').hidden, false);
        assert.ok(p.$('dash').hidden && p.$('lay').classList.contains('solo') && p.$('tech').hidden);
        assert.equal(p.$('wName').textContent, 'Thuis <5G>'); assert.equal(p.$('wSub').textContent, 'Verbonden · IP-adres 192.0.2.5');
        assert.match(p.$('wfSub').textContent, /accesspoint Orcon15RF-Setup/); assert.equal(p.posts().length, 0);
        ok('WiFi card opens the WiFi screen in place of the dashboard: active network, status, IP address');
        await open('wifinew'); assert.equal(p.location.hash, '#wifinew');
        p.$('wScan').click(); await flush(); const sl = p.$('wSel').innerHTML;
        assert.ok(sl.indexOf('Thuis &#60;5G&#62;') < sl.indexOf('Buren') && (sl.match(/Buren/g) || []).length === 2 && /Buren · -60 dBm · open/.test(sl) && !/value=""[^>]*>[^<]*-40/.test(sl));
        ok('scan: networks sorted by signal, de-duplicated, hidden (empty) names skipped, escaped');
        p.$('wSave').click(); await flush(); assert.match(p.$('err').textContent, /netwerknaam/);
        p.$('wSsid').value = 'Thuis <5G>'; p.$('wPass').value = 'kort'; p.$('wSave').click(); await flush(); assert.match(p.$('err').textContent, /8 tot 63/);
        assert.equal(wifiPosts.length, 0); p.$('wShow').checked = true; p.$('wShow').handlers.change[0](); assert.equal(p.$('wPass').type, 'text');
        p.$('wPass').value = ''; p.$('wSave').click(); await flush();
        assert.deepEqual(wifiPosts, [{ ssid: 'Thuis <5G>', pass: '' }]); assert.match(p.$('setM').textContent, /herstart en verbindt met “Thuis <5G>”.*Orcon15RF-Setup/);
        ok('new network: validated, show-password toggle, empty password keeps the stored one, no MQTT fields sent');
        await back(); assert.equal(p.location.hash, '#wifi'); await back(); assert.equal(p.location.hash, '#control'); ok('back: new network -> WiFi -> dashboard');
        await open('wifi'); dlg.confirm = false; dlg.asked = []; p.$('wForget').click(); await flush();
        assert.ok(!p.posts().some(c => c.key.endsWith('/wifi/forget')) && /accesspoint Orcon15RF-Setup/.test(dlg.asked[0]));
        dlg.confirm = true; p.$('wForget').click(); await flush();
        const fg = p.posts().find(c => c.key === 'POST /api/setup/wifi/forget'); assert.ok(fg && fg.headers['X-Orcon-Token'] === 'tok123');
        assert.match(p.$('setM').textContent, /Orcon15RF-Setup.*192\.168\.4\.1/); ok('forget WiFi: only after confirmation, guarded request, recovery via the setup AP explained');
        await back(); await open('system');
        assert.equal(p.location.hash, '#system'); assert.equal(p.$('title').textContent, 'Systeem'); assert.equal(p.$('fwSub').textContent, 'Versie 0123456');
        assert.equal(p.$('oledSub').textContent, 'Automatisch: uit na 30 seconden'); assert.equal(p.$('intSub').textContent, 'MQTT uit');
        assert.ok(p.dom.all.some(e => e.attrs.href === '/dashboard' && /card/.test(e.attrs.class || '')));
        ok('Systeem: firmware, restart, device info, OLED, integrations and the legacy dashboard');
        dlg.confirm = false; p.$('sRestart').click(); await flush(); assert.ok(!p.posts().some(c => c.key.endsWith('/restart')));
        dlg.confirm = true; p.$('sRestart').click(); await flush(); assert.ok(p.posts().some(c => c.key === 'POST /api/setup/restart')); assert.match(p.$('setM').textContent, /herstart/);
        ok('restart only after confirmation');
        await open('firmware'); assert.match(p.$('fwKv').innerHTML, /0123456789abcdef0123456789abcdef01234567/); assert.match(p.$('fwKv').innerHTML, /Niet vast te stellen/);
        assert.ok(!/beschikbaar: v/.test(html)); assert.equal(p.$('fwUp').hidden, true); await back(); assert.equal(p.location.hash, '#system');
        ok('firmware: real version, no invented update; no install button without the ownership model (signed releases); back -> Systeem');
        await open('info'); const ik = p.$('infoKv').innerHTML;
        assert.ok(/1 d 1 u 1 min/.test(ik) && /167 KB/.test(ik) && /<dd>42<\/dd>/.test(ik) && /Thuis &#60;5G&#62;/.test(ik)); await back();
        ok('device info from the existing state API, escaped');
        await open('oled'); assert.match(p.$('oMode').innerHTML, /aria-checked="true" data-om="auto"/);
        p.$('oMode').click({ target: { closest: () => ({ dataset: { om: 'on' }, disabled: false }) } }); await flush();
        p.$('oWake').click(); await flush(); assert.deepEqual(oledPosts, [{ mode: 'on' }, { toggle: 1 }]); assert.match(p.$('oMode').innerHTML, /aria-checked="true" data-om="on"/);
        await back(); ok('OLED moved to Systeem: existing modes and wake toggle, same /api/oled');
        await open('integr'); assert.equal(p.$('mqOn').checked, false); assert.equal(p.$('mqHost').value, '');
        p.$('mqSave').click(); await flush(); assert.match(p.$('err').textContent, /niets gewijzigd/);
        p.$('mqOn').checked = true; p.$('mqSave').click(); await flush(); assert.match(p.$('err').textContent, /broker/);
        p.$('mqHost').value = '192.0.2.9'; p.$('mqSave').click(); await flush();
        assert.deepEqual(wifiPosts[1], { mqtt_on: true, mqtt: '192.0.2.9' }); assert.equal(wifiPosts.length, 2);
        ok('MQTT under Systeem > Integraties: never switched on implicitly, only MQTT fields sent (WiFi kept)');
        await back(); await back(); assert.equal(p.location.hash, '#control');
    }

    // ---- read: pick remote, progress only from server state, wrong button, done ---------------------
    {
        let rs = read(0, 'idle', 0);
        const p = await page({
            'GET /api/setup/state': () => [200, state(CAPS_RX)],
            'GET /api/setup/read/state': () => [200, rs],
            'POST /api/setup/read/start': c => { rs = read(3, 'active', 0); return [201, rs]; },
        }, 'read');
        assert.equal(p.$('readP').hidden, false); assert.match(p.$('remotes').innerHTML, /29:00••01/);
        assert.doesNotMatch(p.$('remotes').innerHTML, /29:000101<\/b>/); assert.match(p.$('remotes').innerHTML, /<b>Afstandsbediening 1<\/b>/);ok('heard remotes named with secondary masked IDs');
        assert.equal(p.$('readS').disabled, true); ok('start disabled until a remote is chosen explicitly');
        p.$('remotes').click({ target: { closest: () => ({ dataset: { id: '29:000101' } }) } });
        await flush();
        const st = p.posts()[0];
        assert.equal(st.key, 'POST /api/setup/read/start'); assert.deepEqual(st.body, { remote: '29:000101' });
        assert.equal(st.headers['X-Orcon-Token'], 'tok123'); assert.equal(st.headers['Content-Type'], 'application/json');
        ok('selection immediately posts the chosen remote with token + JSON content type');
        assert.ok(p.keys[0].classList.contains('cur') && !p.keys[0].classList.contains('done')); assert.ok(p.keys.every(k => k.disabled));
        ok('step 1: first key orange, all keys not clickable');
        rs = read(3, 'active', 2, { why: 'wrong-button', why_cmd: '22F1', why_mode: 3, why_ev: 8 }); await p.refresh();
        assert.ok(p.keys[0].classList.contains('done') && p.keys[1].classList.contains('done') && p.keys[2].classList.contains('cur'));
        assert.ok(!p.keys[3].classList.contains('done') && !p.keys[3].classList.contains('cur'));
        assert.match(p.$('readM').textContent, /Andere knop ontvangen \(Stand 3\)\. Druk op Timer/);
        ok('progress follows server steps; wrong button explained, not counted');
        rs = read(3, 'active', 2, { rf: 'failed' }); await p.refresh();
        assert.match(p.$('readM').textContent, /Radio-ontvanger niet actief/); ok('RF interruption shown');
        rs = read(3, 'done', 6); await p.refresh();
        assert.equal(p.$('doneP').hidden, false); assert.equal(p.$('readP').hidden, true);
        assert.match(p.$('results').innerHTML, /22F3/); assert.ok(p.keys.every(k => k.classList.contains('done')));
        ok('completion screen only after server reports done, with results');
        p.$('again').click(); await flush(); assert.equal(p.$('doneP').hidden, true); assert.equal(p.$('pick').hidden, false);
        ok('restart returns to remote selection');
    }

    // ---- read: an old finished session is not shown as current; cancel; timeout ------------------------
    {
        let rs = read(5, 'done', 6);
        const p = await page({
            'GET /api/setup/state': () => [200, state(CAPS_RX)],
            'GET /api/setup/read/state': () => [200, rs],
            'POST /api/setup/read/start': () => { rs = read(6, 'active', 1); return [201, rs]; },
            'POST /api/setup/read/cancel': c => { assert.deepEqual(c.body, { sid: 6 }); rs = read(6, 'cancelled', 1); return [200, rs]; },
        }, 'read');
        assert.equal(p.$('doneP').hidden, true); assert.ok(!p.keys.some(k => k.classList.contains('done')));
        ok('previous finished session is not presented as this result');
        p.$('remotes').click({ target: { closest: () => ({ dataset: { id: '29:000101' } }) } });
        await flush();
        p.$('readX').click(); await flush();
        assert.equal(p.posts()[1].key, 'POST /api/setup/read/cancel'); assert.match(p.$('readM').textContent, /geannuleerd/);
        assert.equal(p.$('pick').hidden, false); ok('cancel posts the session id and returns to selection');
        rs = read(6, 'timeout', 1); await p.refresh();
        assert.match(p.$('readM').textContent, /Tijd verstreken/); ok('timeout explained');
    }

    // ---- API errors and lost connection -------------------------------------------------------
    {
        let up = true;
        const p = await page({
            'GET /api/setup/state': () => { if (!up) throw new Error('down'); return [200, state(CAPS_RX)]; },
            'GET /api/setup/read/state': () => [200, read(0, 'idle', 0)],
            'POST /api/setup/read/start': () => [409, { error: 'session-busy' }],
        }, 'read');
        p.$('remotes').click({ target: { closest: () => ({ dataset: { id: '29:000101' } }) } });
        await flush();
        assert.equal(p.$('err').hidden, false); assert.match(p.$('err').textContent, /andere sessie/);
        assert.equal(p.$('run').hidden, true); assert.ok(!p.keys.some(k => k.classList.contains('cur')));
        ok('API error shown; no session presented as started');
        const p2 = await page({ 'GET /api/setup/state': () => { throw new Error('net'); } });
        assert.equal(p2.$('conn').textContent, 'Geen verbinding'); assert.match(p2.$('err').textContent, /Geen verbinding/);
        ok('unreachable controller: header and error say so');
    }

    // ---- TX build: virtual remote with three separate statuses -------------------------------------------
    {
        let cs = { supported: true, id: 0, key: '', minutes: 0, accepted: false, sent: false, confirm: 'none', age_s: 0 }, next = null;
        const p = await page({
            'GET /api/setup/state': () => [200, state(CAPS_TX)],
            'GET /api/remote/state': () => [200, cs],
            'POST /api/remote/send': c => next(c),
        }, 'control');
        assert.ok(p.keys.every(k => !k.disabled)); assert.equal(p.$('notice').hidden, true);
        assert.ok(p.keys[3].classList.contains('done')); ok('TX: keys clickable; unit-reported mode (Stand 1) marked');
        next = c => { assert.deepEqual(c.body, { key: '2' }); cs = { ...cs, id: 1, key: '2', accepted: true, sent: true, confirm: 'pending' }; return [202, cs]; };
        p.keys[4].click(); await flush();
        assert.match(p.$('c0').lastChild.textContent, /Stand 2/); assert.match(p.$('c1').lastChild.textContent, /ja/);
        assert.equal(p.$('c2').lastChild.textContent, 'wacht…'); ok('accepted / sent / confirmation shown separately');
        cs = { ...cs, confirm: 'not_confirmed' }; await p.refresh();
        assert.match(p.$('c2').lastChild.textContent, /geen tijdige standmelding/); assert.ok(p.$('c2').classList.contains('n'));
        ok('no unit report -> not confirmed (never assumed)');
        p.keys[2].click(); await flush(); assert.equal(p.$('mins').hidden, false); assert.equal(p.posts().length, 1);
        ok('Timer asks for a duration before anything is sent');
        next = c => { assert.deepEqual(c.body, { key: 'timer', minutes: 30 }); return [502, { error: 'tx-failed' }]; };
        p.dom.all.find(e => e.dataset.min === '30').click(); await flush();
        assert.match(p.$('err').textContent, /Verzenden mislukt/); ok('TX failure is shown as failure');
        next = () => [429, { error: 'too-fast' }];
        p.keys[0].click(); await flush(); assert.match(p.$('err').textContent, /Wacht even/); ok('rate limit explained');
    }

    // ---- TX build: pairing needs the combination and a firmware confirmation ------------------------------
    {
        let ps = { supported: true, sid: 0, status: 'idle', bind_sent: false, rf_confirmed: false, paired: false, left_s: 0 };
        const p = await page({
            'GET /api/setup/state': () => [200, state(CAPS_TX)],
            'GET /api/setup/pair/state': () => [200, ps],
            'POST /api/setup/pair/start': () => { ps = { ...ps, sid: 4, status: 'ready' }; return [201, ps]; },
            'POST /api/setup/pair/press': c => { assert.deepEqual(c.body.keys.sort(), ['1', 'auto']); assert.equal(c.body.sid, 4);
                                                 ps = { ...ps, status: 'bind_sent', bind_sent: true, left_s: 170 }; return [200, ps]; },
        }, 'pair');
        p.$('pairA').click(); await flush();
        assert.ok(p.keys[1].disabled === false); assert.equal(p.$('pairA').disabled, true);
        p.keys[1].click(); assert.equal(p.$('pairA').disabled, true); ok('Auto alone is not enough');
        p.keys[0].click(); assert.match(p.$('pairM').textContent, /Auto \+ 1/); ok('other keys refused for the bind');
        p.keys[3].click(); assert.equal(p.$('pairA').disabled, false);
        p.$('pairA').click(); await flush();
        assert.equal(p.posts().filter(c => c.key.endsWith('/press')).length, 1); ok('Auto + 1 sent as one combination request');
        assert.match(p.$('pairM').textContent, /nog geen bevestiging/); assert.doesNotMatch(p.$('pairM').textContent, /Gekoppeld/);
        ok('bind request sent is not shown as paired');
        ps = { ...ps, status: 'rf_confirmed', rf_confirmed: true }; await p.refresh();
        assert.doesNotMatch(p.$('pairM').textContent, /Gekoppeld:/); ok('RF confirmation alone is not "paired"');
        ps = { ...ps, status: 'paired', paired: true }; await p.refresh();
        assert.match(p.$('pairM').textContent, /Gekoppeld/); ok('paired only when the firmware says so');
    }

    // ---- web-control build: dry-run badge, no timer, phase line, ack step -----------------------------------
    {
        const CAPS_WC = { rx_supported: true, tx_supported: true, read_supported: true, pair_supported: true,
                          control_supported: true, simulated: true, timer_supported: false };
        let cs = { supported: true, id: 0, key: '', minutes: 0, accepted: false, sent: false, confirm: 'none',
                   age_s: 0, phase: 'idle', error: 'none', rf_sent: false, simulated: true, unit_mode: 'low', unit_age_s: 4 };
        const p = await page({
            'GET /api/setup/state': () => [200, state(CAPS_WC)],
            'GET /api/remote/state': () => [200, cs],
            'POST /api/remote/send': () => { cs = { ...cs, id: 1, key: '3', accepted: true, sent: false, confirm: 'not_available', phase: 'dry-run-complete' }; return [202, cs]; },
        }, 'control');
        assert.match(p.$('notice').textContent, /DRY-RUN/); ok('webctl dry-run: prominent notice, keys still work');
        { const q = await page({ 'GET /api/setup/state': () => [200, state({ ...CAPS_WC, simulated: false, tx_max_per_boot: 50 })], 'GET /api/remote/state': () => [200, { ...cs, simulated: false }],
                                 'POST /api/remote/send': () => [409, { error: 'tx-limit-reached' }] }, 'control');
          assert.match(q.$('notice').textContent, /RF-TESTFIRMWARE: echte RF-uitzendingen, maximaal 50 per opstart/);
          q.keys[5].click(); await flush(); assert.match(q.$('err').textContent, /Testlimiet per opstart bereikt/); }
        ok('RF test image: warning with the per-boot cap on the dashboard, refusal at the cap explained');
        assert.equal(p.keys[2].style.visibility, 'hidden'); ok('webctl: timer button hidden (five functions only)');
        p.keys[5].click(); await flush();
        assert.equal(p.posts().length, 1); assert.deepEqual(p.posts()[0].body, { key: '3' });
        assert.match(p.$('c1').lastChild.textContent, /dry-run: nee/);
        assert.match(p.$('c2').lastChild.textContent, /geen RF \(dry-run\)/);
        assert.match(p.$('ctlM').textContent, /Dry-run voltooid/); ok('dry-run statuses: not sent, not confirmed, phase shown');
        let ps = { supported: true, sid: 4, status: 'ack_sent', bind_sent: true, rf_confirmed: true, ack_sent: true, paired: false, left_s: 120, simulated: true };
        const p2 = await page({
            'GET /api/setup/state': () => [200, state(CAPS_WC)],
            'GET /api/setup/pair/state': () => [200, ps],
        }, 'pair');
        assert.match(p2.$('pairM').textContent, /Slotbevestiging verzonden/);
        assert.equal(p2.$('pairX').hidden, false); ok('pairing: closing-ack step shown, cancel still possible');
        ps = { ...ps, status: 'paired', paired: true }; await p2.refresh();
        assert.match(p2.$('pairM').textContent, /Dry-run voltooid/);assert.doesNotMatch(p2.$('pairM').textContent, /Gekoppeld:/); ok('dry-run never displays a real pairing success');
    }

    {   // web-control firmware: no active profile -> nothing to send, keys stay disabled
        const caps={...CAPS_TX,simulated:true,timer_supported:false,profile_control_supported:true};
        let f=PF([PI(3,'Woonkamer','29:000101'),PI(4,'Zolder','29:000505',false)],0);
        const cs={supported:true,id:0,key:'',confirm:'none',phase:'idle',next_request:1};
        const p=await page({
            'GET /api/setup/state':()=>[200,state(caps,{profiles:f})],
            'GET /api/remote/state':()=>[200,cs],
            'POST /api/profiles/select':c=>{f={...f,active:c.body.id};return [200,f];},
        },'control');
        assert.ok(p.keys.every(k=>k.disabled));assert.match(p.$('profileNote').textContent,/Kies een profiel/);ok('web control without an active profile: keys disabled');
        p.$('cProf').click();await flush();pickRow(p,{sel:'4'});await flush();
        assert.equal(p.location.hash,'#control');assert.equal(p.$('profName').textContent,'Zolder');
        assert.ok(p.keys.every(k=>k.disabled));ok('profile without a complete mapping: keys stay disabled');
        p.$('cProf').click();await flush();pickRow(p,{sel:'3'});await flush();
        assert.equal(p.$('profName').textContent,'Woonkamer');assert.ok(!p.keys[4].disabled);ok('verified active profile enables control without a restart');
        assert.ok(!p.posts().some(x=>x.key.includes('/api/remote/send')));ok('profile selection itself sends nothing');
    }
    {
        const caps={...CAPS_TX,simulated:false,timer_supported:false};
        let cs={id:1,key:'3',sent:true,confirm:'observed',phase:'unit-reported-mode',age_s:1,next_request:2};
        const p=await page({
            'GET /api/setup/state':()=>[200,state(caps,{remote_request:{mode:'high',age_s:0}})],
            'GET /api/remote/state':()=>[200,cs],
            'POST /api/remote/send':c=>{assert.deepEqual(c.body,{key:'2',request:2});cs={id:2,key:'2',sent:false,confirm:'pending',next_request:3};return [202,cs];},
        },'control');
        assert.match(p.$('unit').textContent,/Afstandsbediening vraagt: Stand 3/);ok('physical remote request visible without a unit answer');
        assert.match(p.$('c2').lastChild.textContent,/geen ontvangstbewijs/);assert.ok(!p.$('c2').classList.contains('y'));ok('matching mode observation does not claim command confirmation');
        p.keys[4].click();await flush();assert.equal(p.posts().length,1);ok('web commands include the API sequence');
        assert.ok(p.keys.every(k=>k.disabled));ok('pending transaction disables further commands');
    }

    {   // save = store + activate on the server; the page moves to control only when the reply shows it active
        let rs=read(0,'idle',0),f={storage_ok:true,active:0,limit:4,items:[]},mode='fail';
        const item={id:1,name:'Ventilatie woonkamer',remote:'29:000101',control_mapping:true,paired:false};
        const p=await page({
            'GET /api/setup/state':()=>[200,state(CAPS_RX,{profiles:f})],
            'GET /api/setup/read/state':()=>[200,rs],
            'POST /api/setup/read/start':()=>{rs=read(41,'done',6);return [201,rs];},
            'POST /api/profiles/save':c=>{assert.deepEqual(c.body,{sid:41,name:'Ventilatie woonkamer'});
                if(mode==='fail')return [503,{error:'profile-write-failed'}];
                f={...f,items:[item],active:mode==='inactive'?0:1};return [201,f];},
        },'read');
        p.$('remotes').click({target:{closest:()=>({dataset:{id:'29:000101'}})}});await flush();
        assert.equal(p.$('doneP').hidden,false);assert.equal(p.$('saveProfile').textContent,'Profiel opslaan');ok('completed read offers a primary save-profile action');
        p.$('saveName').value='Ventilatie woonkamer';p.$('saveProfile').click();await flush();
        assert.match(p.$('err').textContent,/Opslaan is mislukt/);assert.equal(p.$('saveProfile').disabled,false);
        assert.equal(p.location.hash,'#read');assert.equal(p.$('doneP').hidden,false);ok('storage failure: stays on the read page, retryable, no success shown');
        mode='inactive';p.$('saveProfile').click();await flush();
        assert.equal(p.location.hash,'#read');assert.equal(p.$('ctlP').hidden,true);assert.match(p.$('err').textContent,/niet actief gemaakt/);
        ok('saved but not reported active: stays on the read page with an honest error');
        assert.equal(p.posts().filter(c=>c.key.endsWith('/profiles/save')).length,2);
        p.$('saveProfile').click();await flush();assert.equal(p.posts().filter(c=>c.key.endsWith('/profiles/save')).length,2);
        ok('no duplicate save after the profile was stored');
        assert.ok(!p.calls.some(x=>x.key.includes('/api/remote/send')));ok('profile save never sends a ventilation command');
    }
    {   // first use: read -> name -> save -> completion screen -> dashboard
        let rs=read(0,'idle',0),f=PF([PI(2,'Zolder','29:000505')],0);
        const p=await page({
            'GET /api/setup/state':()=>[200,state(CAPS_RX,{profiles:f})],
            'GET /api/setup/read/state':()=>[200,rs],
            'POST /api/setup/read/start':()=>{rs=read(41,'done',6);return [201,rs];},
            'POST /api/profiles/save':()=>{f={...f,active:3,items:[...f.items,PI(3,'Washok','29:000101')]};return [201,f];},
            'POST /api/profiles/select':c=>{f={...f,active:c.body.id};return [200,f];},
        },'read');
        p.$('remotes').click({target:{closest:()=>({dataset:{id:'29:000101'}})}});await flush();assert.equal(p.$('doneP').hidden,false);
        p.$('saveName').value='Washok';p.$('saveProfile').click();await flush();
        assert.equal(p.location.hash,'#done');assert.equal(p.$('finP').hidden,false);assert.equal(p.$('tick').hidden,false);
        assert.equal(p.$('title').textContent,'Koppeling voltooid');assert.equal(p.$('finName').textContent,'Washok');
        ok('successful save + activation shows the completion screen with the profile name');
        p.$('finGo').click();await flush();
        assert.equal(p.location.hash,'#control');assert.equal(p.$('profName').textContent,'Washok');assert.equal(p.$('profSub').textContent,'Actief profiel');
        ok('"Naar bediening" opens the dashboard with the new profile active');
        assert.ok(p.keys.every(k=>k.disabled));assert.match(p.$('notice').textContent,/niet mogelijk/);
        ok('RX-only firmware: keys stay disabled and the screen says control is not possible');
        p.$('cProf').click();await flush();pickRow(p,{sel:'2'});await flush();
        assert.equal(p.location.hash,'#control');assert.equal(p.$('profName').textContent,'Zolder');ok('multiple profiles: switch via Profielen, back on the dashboard');
        assert.ok(!p.calls.some(x=>x.key.includes('/api/remote/send')));ok('save, activate and switch send nothing');
    }
    {   // Nieuw profiel: name first, kept through the wizard, saved automatically after the six buttons
        let rs=read(0,'idle',0),f=PF([PI(2,'Washok','29:000101')],2),saves=[];
        const p=await page({
            'GET /api/setup/state':()=>[200,state(CAPS_RX,{profiles:f,remotes:[{id:'29:000101',age_s:3,rssi:-61,cmd:'22F1',n:4},{id:'29:000303',age_s:2,rssi:-50,cmd:'22F1',n:2}]})],
            'GET /api/setup/read/state':()=>[200,rs],
            'POST /api/setup/read/start':c=>{rs=read(50,'active',2,{remote:c.body.remote});return [201,rs];},
            'POST /api/setup/read/cancel':()=>{rs=read(50,'cancelled',2,{remote:rs.remote});return [200,rs];},
            'POST /api/profiles/save':c=>{saves.push(c.body);f={...f,active:3,items:[...f.items,PI(3,c.body.name,rs.remote)]};return [201,f];},
        },'profiles');
        assert.match(p.$('plist').innerHTML,/Washok/);p.$('pNew').click();await flush();
        assert.equal(p.location.hash,'#new');assert.equal(p.$('title').textContent,'Nieuw profiel');
        for(const [v,re] of [['','Vul een profielnaam'],['Washok','al een profiel'],['x'.repeat(49),'te lang'],['é'.repeat(25),'te lang']]){
            p.$('newName').value=v;p.$('newNext').click();await flush();assert.match(p.$('newM').textContent,new RegExp(re));assert.equal(p.location.hash,'#new');}
        ok('name validation: empty, duplicate, too long (UTF-8 bytes) refused with a message');
        p.$('newName').value='  Slaapkamer ';p.$('newNext').click();await flush();
        assert.equal(p.location.hash,'#welcome');assert.equal(p.$('back').hidden,false);
        p.$('back').click();await flush();assert.equal(p.location.hash,'#new');assert.equal(p.$('newName').value,'Slaapkamer');
        ok('Volgende opens the same welcome wizard; back returns to the name, kept');
        p.$('newNext').click();await flush();p.dom.all.find(e=>e.dataset.open==='read').click();await flush();
        p.$('remotes').click({target:{closest:()=>({dataset:{id:'29:000303'}})}});await flush();
        p.$('back').click();await flush();
        assert.equal(p.location.hash,'#welcome');assert.ok(p.posts().some(c=>c.key.endsWith('/read/cancel')));assert.equal(saves.length,0);assert.equal(f.active,2);
        ok('cancelling the procedure saves nothing; the old active profile stays');
        p.dom.all.find(e=>e.dataset.open==='read').click();await flush();
        p.$('remotes').click({target:{closest:()=>({dataset:{id:'29:000303'}})}});await flush();
        rs=read(50,'done',6,{remote:'29:000303'});await p.refresh();await flush();
        assert.deepEqual(saves,[{sid:50,name:'Slaapkamer'}]);assert.equal(p.location.hash,'#done');assert.equal(p.$('finName').textContent,'Slaapkamer');
        ok('six buttons in -> saved once under the chosen name, active, completion screen');
        await p.refresh();assert.equal(saves.length,1);ok('no second automatic save');
    }
    {   // automatic save: temporary failures retry on a later poll (max 3 tries); permanent ones leave the button
        const run=async answers=>{let rs=read(0,'idle',0),f=PF([PI(2,'Washok','29:000101')],2),tries=0;
            const p=await page({
                'GET /api/setup/state':()=>[200,state(CAPS_RX,{profiles:f,remotes:[{id:'29:000303',age_s:2,rssi:-50,cmd:'22F1',n:2}]})],
                'GET /api/setup/read/state':()=>[200,rs],
                'POST /api/setup/read/start':()=>{rs=read(70,'done',6,{remote:'29:000303'});return [201,rs];},
                'POST /api/profiles/save':c=>{const a=answers[tries++];if(a==='throw')throw new Error('net');if(a)return a;
                    f={...f,active:3,items:[...f.items,PI(3,c.body.name,rs.remote)]};return [201,f];},
            },'profiles');
            p.$('pNew').click();await flush();p.$('newName').value='Zolder';p.$('newNext').click();await flush();
            p.dom.all.find(e=>e.dataset.open==='read').click();await flush();
            p.$('remotes').click({target:{closest:()=>({dataset:{id:'29:000303'}})}});await flush();await p.refresh();
            return {p,n:()=>tries,f:()=>f};};
        let r=await run(['throw',[500,{}]]);
        assert.equal(r.n(),1);assert.equal(r.p.location.hash,'#read');assert.match(r.p.$('err').textContent,/Geen verbinding.*automatisch opnieuw/);
        assert.equal(r.p.$('saveProfile').disabled,false);assert.equal(r.p.$('saveName').value,'Zolder');
        await r.p.refresh();assert.equal(r.n(),1);ok('save failure while offline: explained, retry announced, button stays, no immediate hammering');
        clock+=2500;await r.p.refresh();assert.equal(r.n(),2);assert.match(r.p.$('err').textContent,/tijdelijke fout \(500\)\. Opslaan wordt automatisch opnieuw/);
        clock+=2500;await r.p.refresh();assert.equal(r.n(),3);assert.equal(r.p.location.hash,'#done');assert.equal(r.p.$('finName').textContent,'Zolder');
        ok('temporary failures retried automatically; third try saves and shows completion');
        r=await run(['throw','throw','throw']);clock+=2500;await r.p.refresh();clock+=2500;await r.p.refresh();clock+=2500;await r.p.refresh();
        assert.equal(r.n(),3);assert.match(r.p.$('err').textContent,/Klik op Profiel opslaan/);assert.equal(r.f().active,2);
        r.p.$('saveProfile').click();await flush();assert.equal(r.n(),4);assert.equal(r.p.location.hash,'#done');
        ok('after three failed tries: no more automatic retries, clear hint, manual save works');
        r=await run([[409,{error:'profile-limit'}]]);clock+=2500;await r.p.refresh();
        assert.equal(r.n(),1);assert.match(r.p.$('err').textContent,/vier profielen.*Klik op Profiel opslaan/);assert.equal(r.f().active,2);
        ok('permanent failure (profile limit): not retried, old active profile kept');
    }
    {   // a remote that already belongs to another profile is never silently renamed
        let rs=read(0,'idle',0),f=PF([PI(2,'Washok','29:000101')],2);
        const p=await page({
            'GET /api/setup/state':()=>[200,state(CAPS_RX,{profiles:f})],
            'GET /api/setup/read/state':()=>[200,rs],
            'POST /api/setup/read/start':()=>{rs=read(60,'done',6);return [201,rs];},
            'POST /api/profiles/save':()=>[201,f],
        },'read');
        p.$('remotes').click({target:{closest:()=>({dataset:{id:'29:000101'}})}});await flush();
        p.$('saveName').value='Keuken';p.$('saveProfile').click();await flush();
        assert.ok(!p.posts().some(c=>c.key.endsWith('/profiles/save')));assert.match(p.$('err').textContent,/al opgeslagen als profiel “Washok”/);
        ok('remote already stored as another profile: refused with an explanation, nothing saved');
    }
    {   // Profielen: list, active badge, menu, rename, delete with confirmation, last one -> welcome
        let f=PF([PI(1,'Woonkamer','29:000101'),PI(2,'Slaap<kamer>','29:000505',false)],1);
        const p=await page({
            'GET /api/setup/state':()=>[200,state(CAPS_RX,{profiles:f})],
            'POST /api/profiles/select':c=>{f={...f,active:c.body.id};return [200,f];},
            'POST /api/profiles/rename':c=>{assert.deepEqual(c.body,{id:1,name:'Keuken'});f={...f,items:f.items.map(x=>x.id===1?{...x,name:c.body.name}:x)};return [200,f];},
            'POST /api/profiles/delete':c=>{f={...f,active:f.active===c.body.id?0:f.active,items:f.items.filter(x=>x.id!==c.body.id)};return [200,f];},
        },'profiles');
        const h=p.$('plist').innerHTML;
        assert.match(h,/Woonkamer<\/b><span class="badge">Actief/);assert.match(h,/Slaap&#60;kamer&#62;/);assert.doesNotMatch(h,/Slaap<kamer>/);
        ok('list shows every stored profile, escaped, with the Actief badge on the active one');
        pickRow(p,{menu:'1'});assert.match(p.$('plist').innerHTML,/data-ren="1"/);assert.match(p.$('plist').innerHTML,/data-del="1"/);ok('three-dot menu offers rename and delete');
        dlg.prompt='Keuken';pickRow(p,{ren:'1'});await flush();assert.match(p.$('plist').innerHTML,/Keuken/);ok('rename via the menu');
        dlg.confirm=false;dlg.asked=[];pickRow(p,{del:'2'});await flush();
        assert.match(dlg.asked[0],/Weet je zeker dat je het profiel Slaap<kamer> wilt verwijderen\?/);assert.ok(!p.posts().some(c=>c.key.endsWith('/delete')));
        ok('delete asks for confirmation first; "no" keeps the profile');
        dlg.confirm=true;pickRow(p,{del:'1'});await flush();
        assert.equal(p.location.hash,'#profiles');assert.doesNotMatch(p.$('plist').innerHTML,/Keuken/);assert.match(p.$('plist').innerHTML,/Slaap/);
        assert.match(p.$('profM').textContent,/Geen actief profiel/);ok('deleting the active profile: others kept, the user chooses again');
        pickRow(p,{del:'2'});await flush();assert.equal(p.location.hash,'#welcome');assert.equal(p.$('back').hidden,true);
        ok('deleting the last profile opens the welcome wizard');
        assert.ok(!p.calls.some(x=>x.key.includes('/api/remote/send')));ok('profile management works in RX-only firmware without any TX action');
    }

    // ---- Live RF Console ------------------------------------------------------------------------
    {
        let cs = () => [200, { now: 50000, next: 4, oldest: 1, lost: 0, more: false, ev: [
            { s: 1, t: 49000, k: 'RAW', m: 'sync #12 -81 dBm pre=FF' },
            { s: 2, t: 49100, k: 'RX', m: '#13 -45 dBm I 22F1 29:****01>29:****02 pl=000404' },
            { s: 3, t: 49200, k: 'FILTER', m: '29:****01 telt niet: <b>x</b> herhaling' }] }];
        const p = await page({
            'GET /api/setup/state': () => [200, state(CAPS_RX)],
            'GET /api/setup/read/state': () => [200, read(0, 'idle', 0)],
            'GET /api/setup/console': c => cs(c),
        }, 'read');
        const ccalls = () => p.calls.filter(c => c.key.startsWith('GET /api/setup/console'));
        assert.ok(!('open' in p.$('con').attrs) && !p.$('con').open); assert.equal(ccalls().length, 0);
        await p.refresh(); assert.equal(ccalls().length, 0);
        ok('console collapsed by default and never polled while closed');
        p.$('con').open = true; p.$('con').handlers.toggle.forEach(f => f()); await flush();
        assert.equal(ccalls()[0].key, 'GET /api/setup/console?after=0');
        const html = p.$('cT').innerHTML;
        assert.match(html, /class="kRAW"><b>RAW<\/b>\d\d:\d\d:\d\d sync #12 -81 dBm pre=FF/);
        assert.match(html, /class="kRX">/); assert.match(html, /class="kFILTER">/);
        assert.match(html, /&#60;b&#62;x&#60;\/b&#62;/); assert.doesNotMatch(html, /<b>x<\/b>/);
        assert.equal(p.$('cS').textContent, 'live');
        ok('opening polls from the start; raw/decoded/filtered lines colour-coded and escaped');
        cs = () => [200, { now: 51000, next: 9, oldest: 6, lost: 2, more: false, ev: [{ s: 6, t: 50900, k: 'OK', m: 'knop Auto geaccepteerd: stap 2/6' }] }];
        await p.refresh();
        assert.equal(ccalls()[1].key, 'GET /api/setup/console?after=3'); ok('delta poll continues after the last sequence number');
        assert.match(p.$('cT').innerHTML, /class="kWARN"><b>WARN<\/b>.* 2 gebeurtenis\(sen\) gemist/); assert.match(p.$('cT').innerHTML, /class="kOK">/);
        ok('missed events reported, accepted press shown as OK');
        cs = () => [500, { error: 'response-too-large' }]; await p.refresh();
        assert.match(p.$('cT').innerHTML, /class="kERR"><b>ERR<\/b>.*API-fout: response-too-large/);
        cs = () => { throw new Error('net'); }; await p.refresh(); await p.refresh();
        assert.equal((p.$('cT').innerHTML.match(/geen verbinding/g) || []).length, 1); ok('API errors shown once each, not repeated');
        cs = () => [200, { now: 1000, next: 2, oldest: 1, lost: 0, more: false, ev: [{ s: 1, t: 900, k: 'INFO', m: 'console gestart' }] }];
        await p.refresh(); assert.match(p.$('cT').innerHTML, /opnieuw begonnen/); ok('controller restart detected from the sequence');
        p.$('cP').click(); await flush(); const before = ccalls().length; await p.refresh();
        assert.equal(ccalls().length, before); assert.equal(p.$('cP').textContent, 'Hervatten'); assert.equal(p.$('cS').textContent, 'gepauzeerd');
        p.$('cP').click(); await flush(); assert.equal(ccalls().length, before + 1); ok('pause stops polling, resume continues');
        p.$('cW').click(); assert.equal(p.$('cT').innerHTML, ''); ok('clear empties the console');
        p.$('cC').click(); await flush(); assert.match(p.$('cS').textContent, /kopiëren niet mogelijk|gekopieerd/); ok('copy reports its result');
        p.$('con').open = false; p.$('con').handlers.toggle.forEach(f => f()); const closed = ccalls().length; await p.refresh();
        assert.equal(ccalls().length, closed); assert.equal(p.$('cS').textContent, 'gesloten'); ok('closing stops polling');
        p.$('con').open = true; p.$('back').click(); await flush(); const away = ccalls().length; await p.refresh();
        assert.equal(ccalls().length, away); ok('leaving the read page stops polling even when open');
    }

    // ---- web login: no default password, recovery code, login, logout, account, RF budget messages -------
    {
        const st = { fw: 'test', auth: { enabled: true, logged_in: false, password_set: false, login_wait_s: 0 }, wifi: { ap: false } };
        const full = state({ ...CAPS_TX, profile_control_supported: true, settings_pages: true, tx_per_minute: 10, tx_per_hour: 60, dashboard: false, mqtt_supported: false },
                           { profiles: PF([PI(2, 'Thuis', '29:000101')], 2), auth: { enabled: true, logged_in: true }, token: 'csrf1' });
        let logged = false, rs = { id: 0, phase: 'idle', confirm: 'none', budget: { minute: 3, minute_max: 10, hour: 9, hour_max: 60, air_ms_hour: 120, retry_s: 0 } };
        const routes = {
            'GET /api/setup/state': () => [200, logged ? full : st],
            'POST /api/auth/setup': c => { if (c.body.code !== 'ABCD-EFGH') return [401, { error: 'bad-code' }]; logged = true; st.auth.password_set = true; return [200, { ok: true, token: 'csrf1' }]; },
            'POST /api/auth/login': c => { if (c.body.password !== 'juist-wachtwoord') return [401, { error: 'bad-password', retry_s: 30 }]; logged = true; return [200, { ok: true, token: 'csrf1' }]; },
            'POST /api/auth/logout': () => { logged = false; return [200, { ok: true }]; },
            'POST /api/auth/password': c => c.body.current === 'juist-wachtwoord' ? [200, { ok: true, token: 'csrf2' }] : [401, { error: 'bad-password', retry_s: 0 }],
            'GET /api/remote/state': () => logged ? [200, rs] : [401, { error: 'login-required' }],
            'POST /api/remote/send': () => [429, { error: 'tx-budget', retry_s: 42 }],
            'GET /api/profiles': () => [200, full.profiles],
        };
        const p = await page(routes, 'control');
        assert.equal(p.location.hash, '#login'); assert.equal(p.$('authP').hidden, false); assert.equal(p.$('ctlP').hidden, true);
        assert.equal(p.$('aSet').hidden, false); assert.equal(p.$('aLogin').hidden, true); assert.equal(p.$('aBack').hidden, true);
        assert.equal(p.$('logout').hidden, true); assert.equal(p.$('title').textContent, 'Aanmelden');
        assert.match(html, /id="aHint">[^]*?python3 tools\/orcon\.py auth-code/);
        ok('no password yet: only the set-password form, with how to get a recovery code');
        assert.ok(!p.calls.some(c => c.key.includes('/api/remote/') || c.key.includes('/api/profiles')));
        ok('no session data is requested before login');
        p.$('aCode').value = 'ABCD-EFGH'; p.$('aNew').value = 'nieuw-wachtwoord'; p.$('aNew2').value = 'anders';
        p.$('aSetGo').click(); await flush();
        assert.equal(p.posts().length, 0); assert.match(p.$('err').textContent, /verschillen/);
        ok('two different new passwords: refused in the page, nothing sent');
        p.$('aCode').value = 'ZZZZ-ZZZZ'; p.$('aNew').value = 'nieuw-wachtwoord'; p.$('aNew2').value = 'nieuw-wachtwoord';
        p.$('aSetGo').click(); await flush();
        assert.match(p.$('err').textContent, /Onjuiste herstelcode/); assert.equal(p.$('aNew').value, ''); assert.equal(p.$('aNew2').value, '');
        ok('wrong code: message; the password fields are emptied, never kept');
        p.$('aCode').value = 'ABCD-EFGH'; p.$('aNew').value = 'nieuw-wachtwoord'; p.$('aNew2').value = 'nieuw-wachtwoord';
        p.$('aSetGo').click(); await flush(); await p.refresh();
        const setCall = p.posts().find(c => c.key === 'POST /api/auth/setup' && c.body.code === 'ABCD-EFGH');
        assert.deepEqual(setCall.body, { code: 'ABCD-EFGH', password: 'nieuw-wachtwoord' });
        assert.equal(p.location.hash, '#control'); assert.equal(p.$('authP').hidden, true); assert.equal(p.$('logout').hidden, false);
        ok('code + password: logged in, on to the dashboard; "Afmelden" in the header');
        assert.equal(p.$('dashL').hidden, true); assert.equal(p.$('cInt').hidden, true); assert.equal(p.$('cAcct').hidden, false);
        ok('control image: no classic-dashboard link, no MQTT card, an Account card');
        assert.match(p.$('ctlM').textContent, /Zendbudget: nog 7 van 10 deze minuut, 51 van 60 dit uur/);
        ok('control screen shows the remaining RF budget');
        p.keys[4].click(); await flush();
        assert.match(p.$('err').textContent, /Zendlimiet bereikt: maximaal 10 per minuut en 60 per uur\. Er is niets verzonden\. Opnieuw mogelijk om \d\d:\d\d:\d\d \(over 42 s\)/);
        ok('budget used up: the page says nothing was sent and when it is possible again');
        rs = { ...rs, budget: { ...rs.budget, minute: 10, retry_s: 15 } }; await p.refresh();
        assert.match(p.$('ctlM').textContent, /Zendlimiet bereikt: opnieuw mogelijk om \d\d:\d\d:\d\d/);
        ok('control screen shows when sending is possible again');
        p.$('logout').click({ preventDefault() {} }); await flush(); await p.refresh();
        assert.equal(p.location.hash, '#login'); assert.equal(p.$('aLogin').hidden, false); assert.equal(p.$('aSet').hidden, true);
        assert.equal(p.$('logout').hidden, true);
        ok('logout: back to the login form');
        p.$('aPass').value = 'fout'; p.$('aGo').click(); await flush();
        assert.match(p.$('err').textContent, /Onjuist wachtwoord\. Opnieuw mogelijk om \d\d:\d\d:\d\d \(over 30 s\)/); assert.equal(p.$('aPass').value, '');
        ok('wrong password: message with the wait time; the field is emptied');
        p.$('aForgot').click(); await flush();
        assert.equal(p.$('aSet').hidden, false); assert.equal(p.$('aBack').hidden, false);
        p.$('aBack').click(); await flush(); assert.equal(p.$('aLogin').hidden, false);
        ok('"Wachtwoord vergeten" shows the recovery form and back');
        st.auth.login_wait_s = 30; await p.refresh();
        assert.equal(p.$('aGo').disabled, true); assert.match(p.$('err').textContent, /over 30 s/);
        st.auth.login_wait_s = 0; await p.refresh(); assert.equal(p.$('aGo').disabled, false);
        ok('locked by the controller: the login button waits');
        p.$('aPass').value = 'juist-wachtwoord'; p.$('aPass').handlers.keydown.forEach(f => f({ key: 'Enter' })); await flush(); await p.refresh();
        assert.equal(p.location.hash, '#control');
        assert.deepEqual(p.posts().filter(c => c.key === 'POST /api/auth/login').pop().body, { password: 'juist-wachtwoord' });
        ok('Enter logs in');
        logged = false; await p.refresh(); await p.refresh();
        assert.equal(p.location.hash, '#login'); ok('session ended on the controller: the page returns to the login form');
        logged = true; p.$('aPass').value = 'juist-wachtwoord'; p.$('aGo').click(); await flush(); await p.refresh();
        p.location.hash = '#account'; p.dom.all.find(e => e.dataset.open === 'account').click(); await flush(); await p.refresh();
        assert.equal(p.$('acctP').hidden, false);
        p.$('acCur').value = 'juist-wachtwoord'; p.$('acNew').value = 'ander-wachtwoord'; p.$('acNew2').value = 'ander-wachtwoord';
        p.$('acGo').click(); await flush();
        assert.match(p.$('acM').textContent, /Wachtwoord gewijzigd/); assert.equal(p.$('acCur').value, ''); assert.equal(p.$('acNew').value, '');
        assert.deepEqual(p.posts().filter(c => c.key === 'POST /api/auth/password').pop().body, { current: 'juist-wachtwoord', password: 'ander-wachtwoord' });
        ok('account: password change with the current password; fields emptied');
        assert.ok(p.posts().every(c => !c.key.startsWith('POST /api/auth/') || c.headers['Content-Type'] === 'application/json'));
        ok('every login request is JSON (the controller refuses simple cross-site posts)');
    }

    // ---- installation wizard (ownership model): a new device on the setup AP --------------------------------------
    {
        const st = { fw: 'test', auth: { enabled: true, logged_in: false, password_set: false, login_wait_s: 0 }, wifi: { ap: true },
                     setup: { state: 'new', via_ap: true, physical: false } };
        let W = { state: 'new', physical: false, password_set: false, logged_in: false, crypto_ok: true, sta_up: false, ap_name: 'Orcon15RF-Setup',
                  key: { registered: false, fingerprint: '', pending: false, pending_fingerprint: '' }, wifi: { test: 'idle', ip: '', ssid: '' } };
        const logged = () => state(CAPS_TX, { profiles: PF([], 0), auth: { enabled: true, logged_in: true },
            wifi: { connected: false, ap: true, ap_name: 'Orcon15RF-Setup', ip: '192.168.4.1', ssid: '' }, setup: { state: W.state, via_ap: true, physical: false, key: false } });
        const PEM = '-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n-----END ENCRYPTED PRIVATE KEY-----\n';
        const p = await page({
            'GET /api/setup/state': () => [200, W.logged_in ? logged() : st],
            'GET /api/provision/state': () => [200, W],
            'POST /api/provision/password': () => { W = { ...W, state: 'setup', password_set: true, logged_in: true }; return [200, { ok: true, token: 'tok123' }]; },
            'GET /api/scan': () => [200, [{ ssid: 'Buren', rssi: -80, lock: true }, { ssid: 'Thuis', rssi: -50, lock: true }]],
            'POST /api/provision/wifi': () => { W = { ...W, wifi: { test: 'testing', ip: '', ssid: '' } }; return [202, { ok: true, test: 'testing' }]; },
            'POST /api/provision/key': () => [200, { ok: true, fingerprint: 'ab12-cd34-ef56' }],
            'GET /api/provision/key/file': () => [200, { filename: 'orcon-recovery-key.pem', fingerprint: 'ab12-cd34-ef56', pem: PEM }],
            'POST /api/provision/key/verify': c => c.body.passphrase === 'goede sleutelzin' ? [200, { ok: true, installed: true, fingerprint: 'ab12-cd34-ef56' }] : [400, { error: 'bad-passphrase' }],
        });
        await p.refresh();
        assert.equal(p.location.hash, '#wizard'); assert.equal(p.$('wizP').hidden, false); assert.equal(p.$('w1').hidden, false);
        assert.equal(p.$('title').textContent, 'Welkom bij Orcon Controller'); assert.equal(p.$('ws1').className, 'on'); assert.equal(p.$('authP').hidden, true);
        ok('new device on the setup AP: the wizard opens at step 1 (password), not the login');
        p.$('w1a').value = 'Abc12345!x'; p.$('w1a').handlers.input.forEach(f => f()); assert.match(p.$('w1s').textContent, /^Sterkte: (goed|sterk)$/);
        p.$('w1b').value = 'anders'; p.$('w1Go').click(); await flush(); assert.ok(!p.posts().some(c => c.key === 'POST /api/provision/password'));
        assert.match(p.$('err').textContent, /verschillen/); ok('step 1: strength meter; a mismatch is refused in the page, nothing sent');
        p.$('w1a').value = 'Abc12345!x'; p.$('w1b').value = 'Abc12345!x'; p.$('w1Go').click(); await flush(); await p.refresh();
        const pw = p.posts().filter(c => c.key === 'POST /api/provision/password');
        assert.equal(pw.length, 1); assert.deepEqual(pw[0].body, { password: 'Abc12345!x' }); assert.equal(pw[0].headers['Content-Type'], 'application/json'); assert.equal(p.$('w1a').value, '');
        assert.equal(p.$('w2').hidden, false); assert.equal(p.$('wnewP').hidden, false); assert.equal(p.$('wSave').textContent, 'Verbinding testen'); assert.equal(p.$('ws1').className, 'fin');
        ok('step 1 sent once as JSON, fields emptied; step 2 shows the WiFi screen with "Verbinding testen"');
        p.$('wScan').click(); await flush(); assert.match(p.$('wSel').innerHTML, /Thuis.*Buren/);
        p.$('wSsid').value = 'Thuis'; p.$('wPass').value = 'wifi-wachtwoord'; p.$('wSave').click(); await flush();
        const wf = p.posts().find(c => c.key === 'POST /api/provision/wifi');
        assert.deepEqual(wf.body, { ssid: 'Thuis', password: 'wifi-wachtwoord' }); assert.equal(wf.headers['X-Orcon-Token'], 'tok123');
        assert.ok(!p.posts().some(c => c.key === 'POST /api/wifi')); assert.equal(p.$('wPass').value, '');
        ok('step 2: networks sorted by signal; the test (not the classic save-and-restart) with the session token');
        W = { ...W, wifi: { test: 'wrong-password', ip: '', ssid: '' } }; await p.refresh();
        assert.match(p.$('w2M').textContent, /Onjuist WiFi-wachtwoord/); assert.equal(p.$('w2').hidden, false); ok('step 2: a wrong password is explained; still on step 2');
        W = { ...W, wifi: { test: 'connected', ip: '192.0.2.23', ssid: 'Thuis' } }; await p.refresh();
        assert.equal(p.$('w3').hidden, false); assert.match(p.$('w3Kv').innerHTML, /Thuis \(getest\)/); assert.equal(p.$('w3Next').textContent, 'Later doen');
        assert.equal(p.$('w3Pair').hidden, false); ok('step 3: shows what is configured; reading and pairing offered; skippable');
        p.$('w3Next').click(); await flush(); assert.equal(p.$('w4').hidden, false); assert.equal(p.$('w4a').hidden, false);
        p.$('w4p1').value = 'kort'; p.$('w4p2').value = 'kort'; p.$('w4Make').click(); await flush(); assert.ok(!p.posts().some(c => c.key === 'POST /api/provision/key'));
        p.$('w4p1').value = 'goede sleutelzin'; p.$('w4p2').value = 'goede sleutelzin'; p.$('w4Make').click(); await flush();
        assert.deepEqual(p.posts().find(c => c.key === 'POST /api/provision/key').body, { passphrase: 'goede sleutelzin' });
        assert.equal(p.$('w4b').hidden, false); assert.equal(p.$('w4Fp').textContent, 'ab12-cd34-ef56'); assert.match(p.$('w4Pem').value, /BEGIN ENCRYPTED PRIVATE KEY/);
        assert.equal(p.$('w4p1').value, ''); ok('step 4: a short passphrase is refused in the page; the key is made, the passphrase cleared; download and copy offered');
        p.$('w4Dl').click(); await flush(); assert.match(p.$('w4M').textContent, /Downloaden lukt hier niet|Download gestart/); ok('download: a fallback is explained where the browser cannot save files');
        p.$('w4In').files = []; p.$('w4Paste').value = p.$('w4Pem').value; p.$('w4p3').value = 'foute zin!!'; p.$('w4Check').click(); await flush();
        assert.match(p.$('err').textContent, /wachtwoordzin klopt niet/); assert.equal(p.$('w4p3').value, ''); assert.equal(p.$('w5').hidden, true);
        ok('verify: a wrong passphrase is refused by the controller; not finished; the field is emptied');
        p.$('w4p3').value = 'goede sleutelzin'; p.$('w4Check').click(); await flush();
        assert.equal(p.$('w5').hidden, false); assert.match(p.$('w5T').textContent, /Thuis.*192\.0\.2\.23/); assert.equal(p.$('w4Pem').value, '');
        ok('verified: installation done; how to reconnect is shown; the key text is no longer kept');
    }
    {   // forgotten password on an installed device: instructions from the home network, the key page in the recovery mode
        const st = { fw: 'test', auth: { enabled: true, logged_in: false, password_set: true, login_wait_s: 0 }, wifi: { ap: false }, setup: { state: 'done', via_ap: false, physical: false } };
        const p = await page({ 'GET /api/setup/state': () => [200, st], 'POST /api/provision/recover': () => [200, { ok: true, token: 'tok9' }] });
        await p.refresh(); assert.equal(p.location.hash, '#login');
        p.$('aForgot').click(); await flush();
        assert.equal(p.$('aHintOwn').hidden, false); assert.equal(p.$('aCodeBox').hidden, true); assert.equal(p.$('aHint').hidden, true);
        assert.match(p.$('aHintOwn').textContent, /RST.*PRG 15 seconden.*Herstelmodus/);
        ok('installed device, forgotten password: the PRG recovery steps; no code entry (the release key grants no recovery)');
        st.setup = { state: 'done', via_ap: true, physical: true }; await p.refresh();
        p.$('aForgot').click(); await flush(); assert.equal(p.location.hash, '#recover'); assert.equal(p.$('recP').hidden, false);
        p.$('rIn').files = []; p.$('rPaste').value = '-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n-----END ENCRYPTED PRIVATE KEY-----\n'; p.$('rPass').value = 'goede sleutelzin';
        p.$('rNew').value = 'nieuw-wachtwoord'; p.$('rNew2').value = 'ander'; p.$('rGo').click(); await flush();
        assert.ok(!p.posts().some(c => c.key === 'POST /api/provision/recover')); ok('recovery: mismatching new passwords are refused in the page');
        p.$('rNew2').value = 'nieuw-wachtwoord'; p.$('rGo').click(); await flush();
        const rc = p.posts().find(c => c.key === 'POST /api/provision/recover');
        assert.match(rc.body.pem, /BEGIN ENCRYPTED/); assert.equal(rc.body.passphrase, 'goede sleutelzin'); assert.equal(rc.body.password, 'nieuw-wachtwoord');
        assert.equal(p.$('rPass').value, ''); assert.equal(p.$('rNew').value, ''); assert.equal(p.$('rPaste').value, '');
        ok('recovery mode on the AP: key file, passphrase and new password in one request; every field emptied');
        st.setup = { state: 'legacy', via_ap: false, physical: false };
        const q = await page({ 'GET /api/setup/state': () => [200, st] }); await q.refresh(); q.$('aForgot').click(); await flush();
        assert.equal(q.$('aCodeBox').hidden, false); assert.equal(q.$('aHint').hidden, false); assert.equal(q.$('aHintOwn').hidden, true);
        ok('migrated device without an owner key: the existing signed-code recovery stays');
        st.setup = { state: 'setup', via_ap: true, physical: false };
        const r = await page({ 'GET /api/setup/state': () => [200, st] }); await r.refresh();
        assert.equal(r.location.hash, '#login'); assert.match(r.$('sub').textContent, /Installatie hervatten/);
        r.$('aForgot').click(); await flush(); assert.match(r.$('aHintOwn').textContent, /fabrieksreset/);
        ok('interrupted installation: log in with the step-1 password; forgotten -> factory reset (no key exists yet)');
        st.setup = { state: 'locked', via_ap: true, physical: false }; st.auth.password_set = false;
        const lq = await page({ 'GET /api/setup/state': () => [200, st] }); await lq.refresh();
        assert.equal(lq.location.hash, '#login'); assert.equal(lq.$('aLogin').hidden, true); assert.equal(lq.$('aCodeBox').hidden, true);
        assert.equal(lq.$('aBack').hidden, true); assert.match(lq.$('aHintOwn').textContent, /kwijtgeraakt.*Fabrieksreset/);
        ok('locked (NVS lost its owner): no login, no setup, no code entry; only the PRG factory reset is explained');
    }
    console.log(`setup page: ${n} checks, 0 failures`);
})().catch(e => { console.error(e); process.exit(1); });
