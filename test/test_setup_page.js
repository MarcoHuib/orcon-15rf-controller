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
                  console, Promise, Set, String, JSON, Math, Object, Array };
    vm.runInNewContext(script, ctx);
    await flush();
    const $ = id => dom.byId[id];
    return { $, calls, dom, keys: dom.all.filter(e => 'data-key' in e.attrs),
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

(async () => {
    let n = 0; const ok = m => { n++; console.log('  [ ok ] ' + m); };

    // ---- start screen + RX-only gating ----------------------------------------------------------
    {
        const p = await page({ 'GET /api/setup/state': () => [200, state(CAPS_RX)] });
        assert.equal(p.$('choices').hidden, false); assert.equal(p.$('pairP').hidden, true); assert.equal(p.$('back').hidden, true);
        ok('start screen shows the three choices');
        assert.equal(p.$('conn').textContent, 'Verbonden · alleen ontvangen'); assert.ok(p.$('conn').classList.contains('ok'));
        ok('header shows the controller connection (RX-only)');
        assert.ok(p.keys.every(k => k.disabled)); ok('remote keys are not clickable on the start screen');
        p.dom.all.find(e => e.dataset.open === 'control').click(); await p.refresh();
        assert.equal(p.$('ctlP').hidden, false); assert.match(p.$('notice').textContent, /niet mogelijk/);
        assert.ok(p.keys.every(k => k.disabled)); p.keys[3].click(); await flush();
        assert.equal(p.posts().length, 0); assert.ok(!p.calls.some(c => c.key.includes('/api/remote/')));
        ok('control screen on RX-only: notice, keys disabled, no remote API call');
        p.$('back').click(); await flush();
        p.dom.all.find(e => e.dataset.open === 'pair').click(); await p.refresh();
        assert.equal(p.$('pairA').disabled, true); assert.match(p.$('notice').textContent, /Koppelen is met deze firmware niet beschikbaar/);
        p.$('pairA').click(); await flush(); assert.equal(p.posts().length, 0);
        ok('pair screen on RX-only: visibly unavailable, nothing sent');
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
        let f={storage_ok:true,active:0,limit:4,items:[{id:3,name:'Woonkamer',control_mapping:true,paired:false},{id:4,name:'Zolder',control_mapping:false,paired:false}]};
        const cs={supported:true,id:0,key:'',confirm:'none',phase:'idle',next_request:1};
        const p=await page({
            'GET /api/setup/state':()=>[200,state(caps,{profiles:f})],
            'GET /api/remote/state':()=>[200,cs],
            'POST /api/profiles/select':c=>{f={...f,active:c.body.id};return [200,f];},
        },'control');
        assert.ok(p.keys.every(k=>k.disabled));assert.match(p.$('profileNote').textContent,/Kies een profiel/);ok('web control without an active profile: keys disabled');
        p.$('profileSel').value='4';p.$('profileSel').handlers.change[0]();await flush();
        assert.ok(p.keys.every(k=>k.disabled));ok('profile without a complete mapping: keys stay disabled');
        p.$('profileSel').value='3';p.$('profileSel').handlers.change[0]();await flush();
        assert.ok(!p.keys[4].disabled);ok('verified active profile enables control');
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

    {
        let rs=read(0,'idle',0),f={storage_ok:true,active:0,limit:4,items:[]},fail=true;
        const p=await page({
            'GET /api/setup/state':()=>[200,state(CAPS_RX,{profiles:f})],
            'GET /api/setup/read/state':()=>[200,rs],
            'POST /api/setup/read/start':()=>{rs=read(41,'done',6);return [201,rs];},
            'POST /api/profiles/save':c=>{assert.deepEqual(c.body,{sid:41,name:'Ventilatie woonkamer'});if(fail)return [503,{error:'profile-write-failed'}];f={...f,items:[{id:1,name:'Ventilatie woonkamer',control_mapping:true,paired:false}]};return [201,f];},
        },'read');
        p.$('remotes').click({target:{closest:()=>({dataset:{id:'29:000101'}})}});await flush();
        assert.equal(p.$('doneP').hidden,false);assert.equal(p.$('saveProfile').textContent,'Profiel opslaan');ok('completed read offers a primary save-profile action');
        p.$('saveName').value='Ventilatie woonkamer';p.$('saveProfile').click();await flush();
        assert.match(p.$('err').textContent,/Opslaan is mislukt/);assert.equal(p.$('saveProfile').disabled,false);ok('storage failure remains retryable without showing success');
        fail=false;p.$('saveProfile').click();await flush();
        assert.match(p.$('profileSaveM').textContent,/Profiel opgeslagen/);assert.equal(p.$('saveProfile').disabled,true);ok('successful profile save is shown after server persistence');
        assert.ok(!p.calls.some(x=>x.key.includes('/api/remote/send')));ok('profile save never sends a ventilation command');
    }
    {
        let f={storage_ok:true,active:0,limit:4,items:[{id:1,name:'Woonkamer',control_mapping:true,paired:false},{id:2,name:'Slaapkamer',control_mapping:false,paired:false}]};
        const p=await page({
            'GET /api/setup/state':()=>[200,state(CAPS_RX,{profiles:f})],
            'POST /api/profiles/select':c=>{f={...f,active:c.body.id};return [200,f];},
            'POST /api/profiles/rename':c=>{assert.deepEqual(c.body,{id:1,name:'Keuken'});f={...f,items:f.items.map(x=>x.id===1?{...x,name:c.body.name}:x)};return [200,f];},
            'POST /api/profiles/delete':c=>{assert.deepEqual(c.body,{id:1});f={...f,active:0,items:f.items.filter(x=>x.id!==1)};return [200,f];},
        },'control');
        assert.match(p.$('profileSel').innerHTML,/Woonkamer/);assert.match(p.$('profileSel').innerHTML,/Slaapkamer/);ok('compact selector lists multiple saved profile names');
        p.$('profileSel').value='1';p.$('profileSel').handlers.change[0]();await flush();
        assert.equal(p.$('profileSel').value,'1');assert.match(p.$('profileNote').textContent,/afzonderlijk nodig/);ok('profile selection follows persisted active ID without claiming pairing');
        p.$('renameProfile').click();p.$('editProfileName').value='Keuken';p.$('renameProfileSave').click();await flush();assert.match(p.$('profileSel').innerHTML,/Keuken/);ok('profile can be renamed in place');
        p.$('deleteProfile').click();await flush();assert.doesNotMatch(p.$('profileSel').innerHTML,/Keuken/);assert.match(p.$('profileSel').innerHTML,/Slaapkamer/);ok('deleting active profile preserves the others');
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

    console.log(`setup page: ${n} checks, 0 failures`);
})().catch(e => { console.error(e); process.exit(1); });
