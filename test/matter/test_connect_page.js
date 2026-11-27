// The owner's connect page (components/orcon_matter/orcon_connect_page.html) against a fake DOM, fetch and clock:
// CSRF (the session token from /api/setup/state only as the X-Orcon-Token header of a same-origin JSON POST), pairing
// codes only while this controller's own window is open and gone the moment it closes or runs out, controller labels
// never as HTML, a removal only after an explicit confirmation, and a distinct message for every refusal.
//   node test/matter/test_connect_page.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const page = fs.readFileSync(path.join(__dirname, '..', '..', 'idf/components/orcon_matter/orcon_connect_page.html'), 'utf8');
const script = page.slice(page.indexOf('<script>') + 8, page.lastIndexOf('</script>'));
let fails = 0, checks = 0;
const check = (ok, what) => { checks++; if (!ok) { fails++; console.log('FAIL ' + what); } };

const TOKEN = 'a'.repeat(32);
function res(status, body) { return { ok: status >= 200 && status < 300, status, json: async () => body }; }
const base = { state: 'ready', fabrics: 0, window: false, window_code: false, window_s: 0, error: '', commissioning: false,
               fabric_revision: 27, controllers: [], diag: { state: 'ready', step: '-', err: '0x0', server_ready: true, confirm_refused: 0, uptime_ms: 600000 } };
const st = o => Object.assign({}, base, o);
const OPEN = st({ window: true, window_code: true, window_s: 900, code: '12345678901', qr: 'MT:Y.K9042C00KA0648G00' });

async function run(routes, { loggedIn = true } = {}) {
  const el = {};
  const get = id => el[id] || (el[id] = { id, textContent: '', innerHTML: '', hidden: false, disabled: false, className: '',
    onclick: null, attrs: {}, setAttribute(k, v) { this.attrs[k] = v; }, getAttribute(k) { return this.attrs[k]; }, focus() {} });
  const calls = [], timers = [], intervals = [];
  const fetch = async (url, opt = {}) => {
    calls.push({ url, opt });
    const key = (opt.method || 'GET') + ' ' + url;
    const r = routes[key];
    if (!r) throw new Error('unexpected request ' + key);
    return typeof r === 'function' ? r(opt) : r;
  };
  routes['GET /api/setup/state'] = routes['GET /api/setup/state'] ||
    res(200, loggedIn ? { auth: { enabled: true, logged_in: true }, token: TOKEN } : { auth: { enabled: true, logged_in: false } });
  const document = { getElementById: get, hidden: false, addEventListener() {}, body: { appendChild() {} },
    createElement: () => ({ style: {}, setAttribute() {}, select() { selected = this.value; }, setSelectionRange() {}, remove() {} }),
    execCommand: () => true };
  const copied = [];
  let selected = '';
  const ctx = { document, fetch, window: { isSecureContext: false }, navigator: {}, console, Math, JSON, String, Array, Number,
    setTimeout: (f, ms) => { timers.push({ f, ms }); return timers.length; }, clearTimeout() {},
    setInterval: (f) => { intervals.push(f); return intervals.length; }, clearInterval: (i) => { if (i) intervals[i - 1] = null; },
    encodeURIComponent, unescape, parseInt, Infinity, Error };
  document.execCommand = () => { copied.push(selected); return true; };
  vm.runInNewContext(script, ctx);
  await flush();
  return { el, calls, timers, intervals, copied };
}
const flush = async () => { for (let i = 0; i < 8; i++) await new Promise(r => setImmediate(r)); };
const posts = calls => calls.filter(c => c.opt.method === 'POST');
const hasSvg = el => /<svg[^>]*viewBox="0 0 33 33"/.test(el.qr.innerHTML);   // a version-2 code plus quiet zone

(async () => {
  // 1. Logged in, window closed: the open button, and nothing of a code anywhere.
  {
    const { el, calls } = await run({ 'GET /api/connect': res(200, st({})), 'POST /api/connect/window': res(200, OPEN) });
    check(!el.open.hidden, 'closed: the open button is shown to the logged-in owner');
    check(el.pOpen.hidden && el.qr.innerHTML === '' && el.code.textContent === '', 'closed: no QR code and no pairing code');
    check(el.cWv.textContent === 'Dicht' && el.cMv.textContent === 'Actief', 'status chips: Matter active, window closed');
    await el.open.onclick(); await flush();
    const p = posts(calls)[0];
    check(p && p.url === '/api/connect/window', 'open: POST to the same origin, relative /api/connect/window');
    check(p && p.opt.headers['X-Orcon-Token'] === TOKEN, 'open: the session token as X-Orcon-Token');
    check(p && p.opt.headers['Content-Type'] === 'application/json', 'open: JSON (Orcon requestAllowed)');
    check(p && p.opt.body === '{"open":true}', 'open: body {"open":true}, the token not in it');
    check(p && !p.url.includes(TOKEN.slice(0, 8)), 'open: the token not in the URL');
    check(!el.pOpen.hidden && hasSvg(el), 'open: the QR code is drawn');
    check(el.code.textContent === '1234-567-8901', 'open: the manual code, grouped 4-3-4');
    check(el.left.textContent === '15:00' && !el.timer.hidden, 'open: the countdown starts at 15:00');
    check(el.open.hidden && !el.close.hidden, 'open: no second open button, a close button');
  }
  // 2. The countdown hides the codes at zero; a poll that finds the window closed clears them too.
  {
    let n = 0;
    const { el, intervals } = await run({ 'GET /api/connect': () => res(200, n++ === 0 ? st({ window: true, window_code: true, window_s: 3, code: '12345678901', qr: 'MT:Y.K9042C00KA0648G00' }) : st({})) });
    check(hasSvg(el) && el.left.textContent === '0:03', 'a short window: code shown, 0:03 left');
    const tick = intervals.filter(Boolean).pop();
    tick(); check(el.left.textContent === '0:02', 'countdown: one second later 0:02');
    tick(); tick(); await flush();
    check(el.qr.innerHTML === '' && el.code.textContent === '' && el.pOpen.hidden, 'countdown at zero: QR and code removed');
  }
  {
    let n = 0;
    const { el, timers } = await run({ 'GET /api/connect': () => res(200, n++ === 0 ? OPEN : st({ fabrics: 1 })) });
    check(hasSvg(el), 'before the poll: the code is shown');
    timers.pop().f(); await flush();
    check(el.qr.innerHTML === '' && el.code.textContent === '', 'window closed (commissioned): codes cleared on the next poll');
    check(el.cFv.textContent === '1', 'after commissioning: one controller');
  }
  // 3. A window another app opened (enhanced, its own code): never this controller's codes.
  {
    const { el } = await run({ 'GET /api/connect': res(200, st({ window: true, window_code: false, fabrics: 1 })) });
    check(el.pOpen.hidden && el.qr.innerHTML === '' && !el.pOther.hidden, 'other app\'s window: no QR, the explanation instead');
  }
  // 4. Codes in a reply without an open own window (must not happen) are still not shown.
  {
    const { el } = await run({ 'GET /api/connect': res(200, st({ code: '12345678901', qr: 'MT:Y.K9042C00KA0648G00' })) });
    check(el.qr.innerHTML === '' && el.code.textContent === '', 'window closed: codes in the reply are ignored');
  }
  // 5. Not logged in: no token, no buttons, the login hint.
  {
    const { el, calls } = await run({ 'GET /api/connect': res(401, { error: 'login-required' }) }, { loggedIn: false });
    check(!el.login.hidden, 'not logged in: the login hint is shown');
    check(el.qr.innerHTML === '' && el.code.textContent === '', 'not logged in: no code');
    check(posts(calls).length === 0, 'not logged in: nothing is posted');
  }
  {
    const { el } = await run({ 'GET /api/connect': res(200, st({ fabrics: 1, controllers: [{ index: 1, vendor: 4937, label: '' }] })) }, { loggedIn: false });
    check(el.open.hidden && el.add.hidden && !/data-rm/.test(el.ctl.innerHTML), 'without a token: no open, add or remove buttons');
  }
  // 6. Controllers: names from the vendor, a controller's label only as text.
  {
    const evil = '<img src=x onerror=alert(1)>';
    const { el } = await run({ 'GET /api/connect': res(200, st({ fabrics: 2, controllers: [{ index: 1, vendor: 4937, label: '' }, { index: 2, vendor: 65521, label: evil }] })) });
    check(/Apple Woning/.test(el.ctl.innerHTML), 'vendor 0x1349 shows as Apple Woning');
    check(!/<img/i.test(el.ctl.innerHTML) && /&lt;img src=x onerror=alert\(1\)&gt;/.test(el.ctl.innerHTML), 'a label never becomes HTML');
    check(!el.add.hidden, 'paired and closed: "Extra controller koppelen" is offered');
    check(el.ctlEmpty.hidden, 'controllers listed: no "Nog niet gekoppeld"');
  }
  // 7. Removing: the first click only asks; "Annuleren" posts nothing; "Ja" posts index + confirm with the token.
  {
    const two = st({ fabrics: 2, controllers: [{ index: 1, vendor: 4937, label: '' }, { index: 2, vendor: 24582, label: '' }] });
    const { el, calls } = await run({ 'GET /api/connect': res(200, two), 'POST /api/connect/remove': res(200, st({ fabrics: 1, controllers: [{ index: 1, vendor: 4937, label: '' }] })) });
    const target = { closest: () => ({ getAttribute: () => '2' }) };
    el.ctl.onclick({ target });
    check(!el.rmBox.hidden && /Google Home/.test(el.rmTxt.textContent) && posts(calls).length === 0, 'remove: a confirmation first, nothing posted');
    el.rmNo.onclick();
    check(el.rmBox.hidden && posts(calls).length === 0, 'remove cancelled: nothing posted');
    el.ctl.onclick({ target }); await el.rmYes.onclick(); await flush();
    const p = posts(calls)[0];
    check(p && p.url === '/api/connect/remove' && p.opt.body === '{"index":2,"revision":27,"confirm":true}', 'remove confirmed: index and confirm');
    check(p && p.opt.headers['X-Orcon-Token'] === TOKEN, 'remove: the session token as X-Orcon-Token');
    check(el.rmBox.hidden && /verwijderd/.test(el.cMsg.textContent) && el.cFv.textContent === '1', 'removed: message and one controller left');
  }
  {
    const fabric = { index: 7, vendor: 4996, label: '', added: true };
    const other = { index: 8, vendor: 65521, label: '', added: false };
    const { el } = await run({ 'GET /api/connect': res(200, st({ fabrics: 2, controllers: [fabric, other] })) });
    check(/Apple Keychain/.test(el.ctl.innerHTML) && !el.appleKeychain.hidden, 'Apple Keychain is identified by its registered VID');
    check(/Onbekende Matter-koppeling/.test(el.ctl.innerHTML) && /65521 \(0xfff1\)/.test(el.ctl.innerHTML), 'unknown VID is displayed accurately');
    check(/toegevoegd sinds start/.test(el.diag.innerHTML) && /al aanwezig bij start/.test(el.diag.innerHTML), 'technical status distinguishes uptime additions');
  }
  {
    const one = st({ fabrics: 1, controllers: [{ index: 2, vendor: 4937, label: '' }] });
    let resolveStatus, resolveRemove, callsToStatus = 0;
    const { el, calls, timers } = await run({
      'GET /api/connect': () => callsToStatus++ ? new Promise(resolve => { resolveStatus = resolve; }) : res(200, one),
      'POST /api/connect/remove': () => new Promise(resolve => { resolveRemove = resolve; }),
    });
    const target = { closest: () => ({ getAttribute: () => '2' }) };
    el.ctl.onclick({ target });
    timers.pop().f();
    const pending = el.rmYes.onclick();
    await el.rmYes.onclick();
    check(posts(calls).length === 1, 'double-click sends a single revocation');
    resolveRemove(res(200, st({ fabric_revision: 28 })));
    await pending;
    resolveStatus(res(200, one));
    await flush();
    check(el.ctlEmpty.hidden === false && el.rmBox.hidden, 'an older refresh cannot restore a revoked fabric');
  }
  {
    const one = st({ fabrics: 1, controllers: [{ index: 2, vendor: 4937, label: '' }] });
    let reads = 0;
    const { el, calls, timers } = await run({
      'GET /api/connect': () => res(200, reads++ ? st({ fabric_revision: 28 }) : one),
    });
    el.ctl.onclick({ target: { closest: () => ({ getAttribute: () => '2' }) } });
    timers.pop().f(); await flush(); await el.rmYes.onclick();
    check(el.rmBox.hidden && /intussen gewijzigd/.test(el.cMsg.textContent) && !posts(calls).length,
      'a changed revision cancels the stale confirmation');
  }
  for (const [status, error, message] of [[404, 'no-such-controller', /bestaat niet/],
                                          [500, 'remove-failed', /niet bevestigen/],
                                          [409, 'fabric-list-changed', /intussen gewijzigd/]]) {
    const { el } = await run({
      'GET /api/connect': res(200, st({ fabrics: 1, controllers: [{ index: 2, vendor: 4937, label: '' }] })),
      'POST /api/connect/remove': res(status, { error }),
    });
    el.ctl.onclick({ target: { closest: () => ({ getAttribute: () => '2' }) } });
    await el.rmYes.onclick();
    check(message.test(el.cMsg.textContent), error + ': the page reports failure');
  }
  {
    const bundle = { schema: 1, snapshot: { count: 1, events: [[77, 'fabric-commit', 3, 4937, 0]] } };
    const { el, calls, copied } = await run({
      'GET /api/connect': res(200, OPEN),
      'GET /api/connect/diagnostics': res(200, bundle),
    });
    await el.diagCopy.onclick();
    check(posts(calls).length === 0 && copied[0] === JSON.stringify(bundle),
      'export copies only the separate read-only diagnostics response, even during an open window');
    check(!copied[0].includes(OPEN.code) && !copied[0].includes(OPEN.qr) && !copied[0].includes(TOKEN),
      'the display-only pairing data and the CSRF token are absent from the export');
  }
  // 8. Closing posts {"open":false}; every refusal gets its own message.
  {
    const { el, calls } = await run({ 'GET /api/connect': res(200, OPEN), 'POST /api/connect/window': res(403, { error: 'bad-token' }) });
    await el.close.onclick(); await flush();
    const p = posts(calls)[0];
    check(p && p.opt.body === '{"open":false}', 'close: body {"open":false}');
    check(/beveiligingstoken/.test(el.pMsg.textContent), 'bad-token: its own message');
  }
  {
    const { el } = await run({ 'GET /api/connect': res(200, st({})), 'POST /api/connect/window': res(403, { error: 'forbidden-request' }) });
    await el.open.onclick(); await flush();
    check(/herkomstcontrole/.test(el.pMsg.textContent), 'forbidden-request: its own message');
  }
  {
    const { el } = await run({ 'GET /api/connect': res(200, st({ fabrics: 1, controllers: [{ index: 1, vendor: 4937, label: '' }] })), 'POST /api/connect/remove': res(409, { error: 'commissioning-busy' }) });
    el.ctl.onclick({ target: { closest: () => ({ getAttribute: () => '1' }) } }); await el.rmYes.onclick(); await flush();
    check(/Er wordt nu gekoppeld/.test(el.cMsg.textContent), 'commissioning-busy: its own message');
  }
  // 9. Copy: the digits only; the hw5 update restart keeps its CSRF rules.
  {
    const { el, copied } = await run({ 'GET /api/connect': res(200, OPEN), 'POST /api/update/restart': res(202, { restart: 'update-check' }) });
    await el.copy.onclick();
    check(copied.length === 1 && /gekopieerd/.test(el.pMsg.textContent), 'copy: the fallback copies on http');
    await el.upd.onclick(); await flush();
    check(/herstart nu zonder Matter/.test(el.uMsg.textContent), 'update restart: accepted message');
  }
  // 10. Matter not running: the reason, no actions.
  {
    const { el } = await run({ 'GET /api/connect': res(200, st({ state: 'failed', diag: { state: 'failed', step: 'update-boot', err: '0x0' } })) });
    check(el.cMv.textContent === 'Niet beschikbaar' && el.open.hidden, 'failed: "Niet beschikbaar", no open button');
    check(/update-boot/.test(el.diag.innerHTML), 'failed: the step is in the technical status');
  }
  // 11. The universal firmware without an identity yet: "Matter activeren" for the logged-in owner only; one POST with
  //     the CSRF rules, then it waits for the restart. A damaged identity: a new one only after an explicit confirmation.
  {
    const NOT = st({ state: 'failed', identity: 'absent', diag: { state: 'failed', step: 'not-activated', err: '0x0' } });
    const { el, calls } = await run({ 'GET /api/connect': res(200, NOT),
      'POST /api/connect/activate': res(202, Object.assign({}, NOT, { activating: true })) });
    check(!el.act.hidden && el.activate.textContent === 'Matter activeren', 'not activated: the activate button for the owner');
    check(el.cMv.textContent === 'Niet geactiveerd' && /nog niet geactiveerd/.test(el.stText.textContent), 'not activated: chip and status say so');
    check(el.open.hidden && el.pOpen.hidden, 'not activated: no pairing window, no code');
    await el.activate.onclick(); await flush();
    const p = posts(calls)[0];
    check(p && p.url === '/api/connect/activate' && p.opt.body === '{"action":"activate"}', 'activate: POST {"action":"activate"}');
    check(p && p.opt.headers['X-Orcon-Token'] === TOKEN && p.opt.headers['Content-Type'] === 'application/json', 'activate: CSRF token and JSON');
    check(el.activate.disabled && /herstart/.test(el.actMsg.textContent), 'activate: waits for the restart, button stays off');
  }
  {
    const NOT = st({ state: 'failed', identity: 'absent', diag: { state: 'failed', step: 'not-activated', err: '0x0' } });
    const { el } = await run({ 'GET /api/connect': res(200, NOT) }, { loggedIn: false });
    check(el.act.hidden, 'not logged in: no activate button');
  }
  {
    const NOT = st({ state: 'failed', identity: 'absent', diag: { state: 'failed', step: 'not-activated', err: '0x0' } });
    const { el } = await run({ 'GET /api/connect': res(200, NOT), 'POST /api/connect/activate': res(409, { error: 'trial-boot' }) });
    await el.activate.onclick(); await flush();
    check(/proefstart/.test(el.actMsg.textContent) && !el.activate.disabled, 'trial-boot: its own message, the button again');
  }
  {
    const BAD = st({ state: 'failed', identity: 'damaged', diag: { state: 'failed', step: 'identity-damaged', err: '0x109' } });
    const { el, calls } = await run({ 'GET /api/connect': res(200, BAD), 'POST /api/connect/activate': res(202, Object.assign({}, BAD, { activating: true })) });
    check(el.cMv.textContent === 'Beschadigd' && el.activate.textContent === 'Nieuwe identiteit maken', 'damaged: chip and the replace button');
    await el.activate.onclick(); await flush();
    check(posts(calls).length === 0 && !el.actBox.hidden, 'damaged: the first click only asks');
    el.actNo.onclick(); await flush();
    check(el.actBox.hidden && posts(calls).length === 0, 'damaged: cancel sends nothing');
    await el.activate.onclick(); el.actYes.onclick(); await flush();
    const p = posts(calls)[0];
    check(p && p.opt.body === '{"action":"replace","confirm":true}', 'damaged: confirmed, POST {"action":"replace","confirm":true}');
  }
  {
    const { el } = await run({ 'GET /api/connect': res(200, st({ state: 'failed', identity: 'absent', activating: true })) });
    check(el.activate.disabled && /herstart zo/.test(el.actMsg.textContent), 'activating: the button off and why');
  }
  {
    const { el } = await run({ 'GET /api/connect': res(200, st({ identity: 'valid' })) });
    check(el.act.hidden && el.cMv.textContent === 'Actief', 'activated: no activation block');
  }
  console.log(`connect page: ${checks} checks, ${fails} failures`);
  process.exit(fails ? 1 : 0);
})();
