// Run the actual emitted setup-page script in a small browser/fetch harness.
// Usage: dump-pages 5 | node test/test_oled_setup.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync(0, 'utf8');
const script = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]).join('\n;\n');
const flush = () => new Promise(resolve => setImmediate(resolve));

async function page(state, result = 'ok') {
    const elements = {};
    for (const id of [...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]))
        elements['#' + id] = {value: '', textContent: '', disabled: false, hidden: id === 'oledcard',
                              appendChild() {}, style: {}};
    let banner = '';
    const writes = [];
    const document = {
        querySelector: s => { assert.ok(elements[s], s); return elements[s]; },
        querySelectorAll: () => [],
        createElement: () => ({}),
        body: {insertAdjacentHTML: (_, text) => {banner += text;}},
    };
    const fetch = async (url, options = {}) => {
        if (options.method === 'POST') {
            writes.push({url, body: JSON.parse(options.body)});
            assert.equal(elements['#oledsave'].disabled, true);
            if (result === 'network') throw new Error('offline');
            return {ok: result === 'ok', json: async () => result === 'ok' ? {ok: 1} : {err: 'display setting not saved'}};
        }
        if (url === '/api/state') return {json: async () => state};
        if (url === '/api/netcfg') return {json: async () => ({mqtt_on: false})};
        if (url === '/api/scan') return {json: async () => []};
        throw new Error('Unexpected request: ' + url);
    };
    vm.runInNewContext(script, {document, fetch, window: {}});
    await flush();
    return {elements, writes, banner};
}

(async () => {
    for (const mode of ['on', 'off', 'auto']) {
        const {elements: e, writes, banner} = await page({oled_available: true, oled_mode: mode, rx_only: true});
        assert.equal(e['#oledcard'].hidden, false);
        assert.equal(e['#oledmode'].value, mode);
        assert.match(banner, /RX-ONLY TEST FIRMWARE/);
        assert.equal(e['#oledsave'].disabled, false);
        await e['#oledsave'].onclick();
        assert.deepEqual(writes, [{url: '/api/oled', body: {mode}}]);
        assert.equal(e['#oledsave'].disabled, false);
        assert.match(e['#oledmsg'].textContent, /^Saved\./);
        if (mode === 'auto') assert.match(e['#oledmsg'].textContent, /30 seconds/);
    }
    for (const state of [{oled_available: false}, {}]) {
        const {elements, writes} = await page(state);
        assert.equal(elements['#oledcard'].hidden, true, 'other builds/older firmware hide setting');
        assert.deepEqual(writes, []);
    }
    for (const result of ['server', 'network']) {
        const {elements: e} = await page({oled_available: true, oled_mode: 'off'}, result);
        await e['#oledsave'].onclick();
        assert.equal(e['#oledsave'].disabled, false);
        assert.doesNotMatch(e['#oledmsg'].textContent, /^Saved\./);
        assert.match(e['#oledmsg'].textContent, /not saved|failed/i);
    }
    console.log('PASS: OLED setup modes, RX-only access, other builds and save failures');
})().catch(error => { console.error(error); process.exitCode = 1; });
