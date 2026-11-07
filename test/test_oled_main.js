// Run the actual emitted index-page script in a small browser/fetch harness and
// exercise the OLED screen button (Heltec V2 only).
// Usage: dump-pages 0 | node test/test_oled_main.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync(0, 'utf8');
const script = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]).join('\n;\n');
const flush = () => new Promise(resolve => setImmediate(resolve));

async function page(state, behavior = {}) {
    const mk = () => ({value: '', textContent: '', disabled: false, hidden: false,
                       checked: false, title: '', appendChild() {}, style: {},
                       classList: (() => { const s = new Set();
                           return {toggle: (c, f) => { if (f === undefined) f = !s.has(c);
                                                      f ? s.add(c) : s.delete(c); },
                                   contains: c => s.has(c),
                                   add: c => s.add(c), remove: c => s.delete(c)}; })()});
    const elements = {};   // every selector gets one cached stub (id or attribute)
    const el = s => elements[s] ?? (elements[s] = mk());
    let banner = '';
    const writes = [];
    const document = {
        querySelector: el,
        querySelectorAll: () => [],
        getElementById: () => undefined,   // room pins are guarded with if(!el)return
        createElement: () => ({value: '', textContent: ''}),
        body: {insertAdjacentHTML: (_, text) => {banner += text;}},
    };
    const fetch = async (url, options = {}) => {
        if (options.method === 'POST') {
            writes.push({url, body: JSON.parse(options.body)});
            assert.equal(elements['#oledbtn'].disabled, true, 'icon disables while posting');
            if (behavior.onToggle) behavior.onToggle(state);
            return {ok: behavior.fail ? false : true, json: async () => ({ok: 1})};
        }
        if (url === '/api/state') return {json: async () => state};
        throw new Error('Unexpected request: ' + url);
    };
    vm.runInNewContext(script, {document, fetch, window: {}, setInterval: () => 0, setTimeout: () => 0});
    await flush();
    return {elements, writes, banner};
}

(async () => {
    // V2 build, RX-only: the icon is a local display control, so it must be
    // visible AND stay enabled on an RX-only board.
    let s = {oled_available: true, oled_on: true, rx_only: true, fw: 'test'};
    let r = await page(s, {onToggle: st => { st.oled_on = false; }});
    const b = r.elements['#oledbtn'];
    assert.equal(r.banner.match(/RX-ONLY TEST FIRMWARE/g).length, 1);
    assert.equal(b.hidden, false, 'icon visible when the OLED is available');
    assert.equal(b.classList.contains('on'), true, 'lit state while the screen is on');
    assert.equal(b.disabled, false, 'icon stays enabled on RX-only (local display setting)');
    await b.onclick();
    assert.deepEqual(r.writes, [{url: '/api/oled', body: {toggle: 1}}]);
    await flush();
    assert.equal(b.disabled, false);
    assert.equal(b.classList.contains('on'), false, 'icon dims after the toggle-off refresh');
    assert.match(b.title, /off/);

    // Wake via the icon while the screen is off (Auto window restarts server-side).
    s = {oled_available: true, oled_on: false, rx_only: true, fw: 'test'};
    r = await page(s, {onToggle: st => { st.oled_on = true; }});
    assert.equal(r.elements['#oledbtn'].classList.contains('on'), false);
    await r.elements['#oledbtn'].onclick();
    assert.deepEqual(r.writes, [{url: '/api/oled', body: {toggle: 1}}]);
    await flush();
    assert.equal(r.elements['#oledbtn'].classList.contains('on'), true, 'icon lights after toggle-on');

    // Other builds / older firmware without oled_available: icon stays hidden,
    // no writes.
    for (const state of [{oled_available: false}, {}]) {
        r = await page(state);
        assert.equal(r.elements['#oledbtn'].hidden, true);
        assert.deepEqual(r.writes, []);
    }

    // A failed toggle still re-syncs from /api/state and leaves no stuck UI.
    s = {oled_available: true, oled_on: true, rx_only: false, fw: 'test'};
    r = await page(s, {fail: true});
    await r.elements['#oledbtn'].onclick();
    await flush();
    assert.equal(r.elements['#oledbtn'].disabled, false);
    assert.equal(r.elements['#oledbtn'].classList.contains('on'), true, 'state re-read after a failed toggle');

    console.log('PASS: main-page OLED icon visibility, toggle, RX-only and failure paths');
})().catch(error => { console.error(error); process.exitCode = 1; });
