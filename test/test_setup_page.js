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
        const h = routes[key];
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
        assert.equal(p.$('pairA').disabled, true); assert.match(p.$('notice').textContent, /Koppelen is met deze firmware niet mogelijk/);
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
        assert.doesNotMatch(p.$('remotes').innerHTML, /29:000101<\/b>/); ok('heard remotes listed with masked IDs');
        assert.equal(p.$('readS').disabled, true); ok('start disabled until a remote is chosen explicitly');
        p.$('remotes').click({ target: { closest: () => ({ dataset: { id: '29:000101' } }) } });
        assert.equal(p.$('readS').disabled, false);
        p.$('readS').click(); await flush();
        const st = p.posts()[0];
        assert.equal(st.key, 'POST /api/setup/read/start'); assert.deepEqual(st.body, { remote: '29:000101' });
        assert.equal(st.headers['X-Orcon-Token'], 'tok123'); assert.equal(st.headers['Content-Type'], 'application/json');
        ok('start posts the chosen remote with token + JSON content type');
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
        p.$('readS').click(); await flush();
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
        p.$('readS').click(); await flush();
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
        assert.match(p.$('c2').lastChild.textContent, /geen melding/); assert.ok(p.$('c2').classList.contains('n'));
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

    console.log(`setup page: ${n} checks, 0 failures`);
})().catch(e => { console.error(e); process.exit(1); });
