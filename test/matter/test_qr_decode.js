// QR encoder of the connect page (orcon_connect_page.html, const QR) against an independent decoder: Chrome's
// BarcodeDetector (on macOS: Apple's Vision framework, the scanner family of Apple Home). Synthetic payloads only
// (the Matter spec's example and random base38 strings), never this device's own codes.
//   node test/matter/test_qr_decode.js [<chrome binary>]   (needs Google Chrome; macOS: Apple's Vision decoder)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const comp = path.join(__dirname, '..', '..', 'idf/components/orcon_matter');
const chrome = process.argv[2] || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const page = fs.readFileSync(path.join(comp, 'orcon_connect_page.html'), 'utf8');
const qr = page.slice(page.indexOf('const QR = (() => {'), page.indexOf('return { matrix, svg };\n})();') + 'return { matrix, svg };\n})();'.length);
if (qr.length < 1000) { console.log('QR block not found'); process.exit(1); }

const B38 = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.';
let seed = 12345;
const rnd = n => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
const b38 = n => Array.from({ length: n }, () => B38[rnd(38)]).join('');
const cases = ['MT:Y.K9042C00KA0648G00', 'MT:-24J0AFN00KA0648G00'];   // Matter spec / CHIP examples
for (let i = 0; i < 20; i++) cases.push('MT:' + b38(19));                     // random onboarding-like payloads
for (const n of [1, 10, 25, 40, 60, 90, 118, 140, 150]) cases.push(b38(n).replace(/[^0-9A-Z.-]/g, 'A'));   // versions 1-6
cases.push('Orcon ventilator, ü');                                              // byte mode

const html = `<!doctype html><html><body><pre id="out">running</pre><script>
${qr}
const cases = ${JSON.stringify(cases)};
window.__done = (async () => {
  if (!('BarcodeDetector' in window)) return 'NO-DETECTOR';
  const det = new BarcodeDetector({ formats: ['qr_code'] }), out = [];
  for (const t of cases) {
    const m = QR.matrix(t), n = m.length, s = 8, q = 4;
    const c = document.createElement('canvas'); c.width = c.height = (n + 2 * q) * s;
    const g = c.getContext('2d'); g.fillStyle = '#fff'; g.fillRect(0, 0, c.width, c.height); g.fillStyle = '#000';
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) if (m[y][x]) g.fillRect((x + q) * s, (y + q) * s, s, s);
    let got = '';
    try { const r = await det.detect(c); got = r.length ? r[0].rawValue : ''; } catch (e) { got = 'ERR ' + e; }
    out.push((got === t ? 'PASS' : 'FAIL') + ' v' + ((n - 17) / 4) + ' ' + JSON.stringify(t) + (got === t ? '' : ' got ' + JSON.stringify(got)));
  }
  return out.join('\\n');
})();
window.onerror = (m) => { window.__err = String(m); };
</script></body></html>`;
// DevTools answers carry the id of one of our own requests. Anything else (an event, a binary or malformed frame, an
// id that is not a pending request's) is dropped, and each request's callback runs once (CodeQL alert 3).
function devtools(send) {
  const pending = new Map();
  let next = 0;
  return {
    call: (method, params) => new Promise(resolve => {
      const id = ++next;
      pending.set(id, resolve);
      send(JSON.stringify({ id, method, params }));
    }),
    onMessage(data) {
      if (typeof data !== 'string') return false;
      let m;
      try { m = JSON.parse(data); } catch (e) { return false; }
      const id = m !== null && typeof m === 'object' ? m.id : undefined;
      if (!Number.isSafeInteger(id) || id <= 0) return false;
      const done = pending.get(id);
      if (typeof done !== 'function') return false;
      pending.delete(id);
      done(m);
      return true;
    },
    pending: () => pending.size,
  };
}
// Chrome over the DevTools protocol (Node's built-in WebSocket): wait for the real decoder, not a virtual clock.
(async () => {
  {   // the dispatcher first, on its own: malformed and foreign answers do nothing, a request is answered once
    const sent = [], t = devtools(s => sent.push(JSON.parse(s)));
    const first = t.call('Self.test', {});
    const junk = [undefined, null, 7, '', 'not json', 'null', '[]', '"1"', '{"id":"1"}', '{"id":0}', '{"id":-1}',
                  '{"id":1.5}', '{"id":"__proto__"}', '{"id":"constructor"}', '{"id":2}', '{"method":"Page.loadEventFired"}'];
    const dropped = junk.every(d => t.onMessage(d) === false);
    const once = t.onMessage('{"id":1,"result":"a"}') === true && t.onMessage('{"id":1,"result":"b"}') === false;
    if (!dropped || !once || t.pending() !== 0 || sent[0].id !== 1 || (await first).result !== 'a') {
      console.log('FAIL devtools dispatcher');
      process.exit(1);
    }
  }
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'qrtest-'));
  const file = path.join(dir, 'qr.html');
  fs.writeFileSync(file, html);
  const port = 9300 + Math.floor(Math.random() * 500);
  const { spawn } = require('child_process');
  const ch = spawn(chrome, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    '--remote-debugging-port=' + port, '--user-data-dir=' + path.join(dir, 'profile'), 'about:blank'], { stdio: 'ignore' });
  let text = 'no output';
  try {
    let target = null;
    for (let i = 0; i < 100 && !target; i++) {
      await new Promise(r => setTimeout(r, 200));
      try { target = await (await fetch('http://127.0.0.1:' + port + '/json/new?' + encodeURI('file://' + file), { method: 'PUT' })).json(); } catch (e) {}
    }
    const ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((ok, bad) => { ws.onopen = ok; ws.onerror = bad; });
    const dt = devtools(text => ws.send(text));
    ws.onmessage = e => { dt.onMessage(e.data); };
    const call = dt.call;
    for (let i = 0; i < 50; i++) {   // the page's script defines __done once it has loaded
      const r = await call('Runtime.evaluate', { expression: 'typeof window.__done', returnByValue: true });
      if (r.result && r.result.result && r.result.result.value === 'object') break;
      await new Promise(r => setTimeout(r, 200));
    }
    const r = await call('Runtime.evaluate', { expression: 'window.__done || Promise.resolve("PAGE ERROR " + (window.__err || document.title))', awaitPromise: true, returnByValue: true });
    text = r.result && r.result.result ? String(r.result.result.value) : JSON.stringify(r);
    ws.close();
  } finally { ch.kill(); }
  console.log(text);
  const fails = (text.match(/^FAIL/gm) || []).length, passes = (text.match(/^PASS/gm) || []).length;
  console.log(`qr decode: ${passes} passed, ${fails} failed of ${cases.length}`);
  process.exit(fails === 0 && passes === cases.length ? 0 : 1);
})();
