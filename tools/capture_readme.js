#!/usr/bin/env node
'use strict';

// Local documentation capture only. Serves src/setup_page.html unchanged and mocks
// the documented HTTP contracts. It never opens a serial port or contacts a device.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFileSync } = require('node:child_process');
const { chromium } = require('playwright');

const root = path.resolve(__dirname, '..');
const out = path.join(root, 'docs', 'images', 'screenshots');
const work = path.join(root, 'build', 'readme-screenshots');
const password = process.env.ORCON_SCREENSHOT_PASSWORD;
const chrome = process.env.CHROME_PATH || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
if (!password) throw new Error('Set ORCON_SCREENSHOT_PASSWORD to a disposable local-only test password.');
fs.mkdirSync(out, { recursive: true });
fs.mkdirSync(work, { recursive: true });

const FW = '6a4b4c61b8daff5d1c6c74d93b8110c45caf2000';
const PEM_PLACEHOLDER = '-----BEGIN ENCRYPTED PRIVATE KEY-----\nLOKALE-PLAATSHOUDER-GEEN-SLEUTEL\n-----END ENCRYPTED PRIVATE KEY-----\n';
const caps = {
  rx_supported: true, tx_supported: true, read_supported: true, pair_supported: true,
  control_supported: true, simulated: false, timer_supported: false,
  profile_control_supported: true, settings_pages: true, tx_max_per_boot: 0,
  tx_per_minute: 10, tx_per_hour: 60, dashboard: false, mqtt_supported: false,
};
const profileItems = [
  { id: 1, name: 'Woonkamer', remote: '29:010101', unit: '29:020202', control_mapping: true, paired: false },
  { id: 2, name: 'Zolder', remote: '29:010303', unit: '29:020202', control_mapping: true, paired: false },
  { id: 3, name: 'Washok', remote: '29:010505', unit: '29:020202', control_mapping: true, paired: false },
];
const profiles = () => ({ storage_ok: true, active: scenario === 'wizard' ? 0 : 1, limit: 4, items: scenario === 'wizard' ? [] : profileItems });
const readSteps = n => ['away', 'auto', 'timer', '1', '2', '3'].map((key, i) => i < n
  ? { key, ok: true, cmd: key === 'timer' ? '22F3' : '22F1', pl: key === 'timer' ? '00012C' : '000007', rssi: -58, ev: i + 1 }
  : { key, ok: false });

let scenario = 'login';
let sid = '';
let csrf = '';
let read = { sid: 0, status: 'idle', step: 0 };
let pair = { supported: true, sid: 0, status: 'idle', bind_sent: false, rf_confirmed: false, ack_sent: false, paired: false, left_s: 0, simulated: false };
let provision;
let setup;

function reset(name) {
  scenario = name;
  sid = '';
  csrf = '';
  read = { sid: 0, status: 'idle', step: 0 };
  pair = { supported: true, sid: 0, status: 'idle', bind_sent: false, rf_confirmed: false, ack_sent: false, paired: false, left_s: 0, simulated: false };
  setup = { state: 'done', via_ap: false, physical: false, key: true };
  provision = {
    state: 'done', physical: false, physical_left_s: 0, password_set: true, logged_in: false,
    crypto_ok: true, sta_up: false, ap_name: 'Orcon15RF-Setup',
    key: { registered: true, fingerprint: 'ab12-cd34-ef56', pending: false, pending_fingerprint: '' },
    wifi: { test: 'idle', ip: '', ssid: 'Demo Thuisnetwerk' },
  };
  if (name === 'wizard') {
    setup = { state: 'new', via_ap: true, physical: false };
    provision = { ...provision, state: 'new', password_set: false, key: { registered: false, fingerprint: '', pending: false, pending_fingerprint: '' }, wifi: { test: 'idle', ip: '', ssid: '' } };
  } else if (name === 'recovery') {
    setup = { state: 'done', via_ap: true, physical: true };
    provision = { ...provision, physical: true, physical_left_s: 870 };
  }
}
reset('login');

function newSession(res) {
  sid = crypto.randomBytes(16).toString('hex');
  csrf = crypto.randomBytes(16).toString('hex');
  res.setHeader('Set-Cookie', `orcon_sid=${sid}; Path=/; HttpOnly; SameSite=Strict; Max-Age=3600`);
}
function loggedIn(req) {
  const cookie = req.headers.cookie || '';
  return !!sid && cookie.split(/;\s*/).includes(`orcon_sid=${sid}`);
}
function state(req) {
  if (!loggedIn(req)) return {
    fw: FW,
    auth: { enabled: true, logged_in: false, password_set: provision.password_set, login_wait_s: 0 },
    wifi: { ap: setup.via_ap }, setup: { state: setup.state, via_ap: setup.via_ap, physical: setup.physical },
  };
  return {
    fw: FW, rx_only: false, caps, rf: 'receiving', token: csrf, mask_ids: true, active: 'none',
    auth: { enabled: true, logged_in: true },
    unit: { known: true, id: '29:020202', mode: scenario === 'dashboard' ? 'medium' : 'auto', age_s: 4 },
    remotes: [
      { id: '29:010101', age_s: 3, rssi: -58, cmd: '22F1', n: 4 },
      { id: '29:010303', age_s: 8, rssi: -71, cmd: '22F1', n: 2 },
    ],
    wifi: setup.via_ap
      ? { connected: false, ap: true, ap_name: 'Orcon15RF-Setup', ip: '192.168.4.1', ssid: '' }
      : { connected: true, ap: false, ap_name: 'Orcon15RF-Setup', ip: '192.0.2.24', ssid: 'Demo Thuisnetwerk' },
    profiles: profiles(), setup,
  };
}
function json(res, code, value) {
  const body = JSON.stringify(value);
  res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Content-Length': Buffer.byteLength(body) });
  res.end(body);
}
async function body(req) {
  let text = '';
  for await (const chunk of req) {
    text += chunk;
    if (text.length > 20000) throw new Error('mock body too large');
  }
  return text ? JSON.parse(text) : {};
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, 'http://127.0.0.1');
    if (req.method === 'GET' && url.pathname === '/') {
      const html = fs.readFileSync(path.join(root, 'src', 'setup_page.html'));
      res.writeHead(200, {
        'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-cache',
        'X-Content-Type-Options': 'nosniff', 'X-Frame-Options': 'DENY',
        'Content-Security-Policy': "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'",
      });
      res.end(html);
      return;
    }
    if (req.method === 'POST' && url.pathname === '/__mock/scenario') {
      const b = await body(req);
      if (b.read === 'done') read = { ...read, status: 'done', step: 6, left_s: 0, steps: readSteps(6) };
      else reset(b.name);
      json(res, 200, { ok: true });
      return;
    }
    if (req.method === 'GET' && url.pathname === '/api/setup/state') return json(res, 200, state(req));
    if (req.method === 'GET' && url.pathname === '/api/provision/state') {
      provision.logged_in = loggedIn(req);
      return json(res, 200, provision);
    }
    if (req.method === 'GET' && url.pathname === '/api/scan') return json(res, 200, [
      { ssid: 'Open voorbeeld', rssi: -80, lock: false },
      { ssid: 'Demo Thuisnetwerk', rssi: -48, lock: true },
      { ssid: 'Zolder-demo', rssi: -67, lock: true },
    ]);
    if (req.method === 'GET' && url.pathname === '/api/state') return json(res, 200, {
      uptime_ms: 90061000, heap_min: 171008, rf_frames: 42, oled_available: true,
      oled_mode: 'auto', oled_on: false, mqtt_on: false, mqtt_connected: false,
    });
    if (req.method === 'GET' && url.pathname === '/api/netcfg') return json(res, 200, { ssid: 'Demo Thuisnetwerk', mqtt: '', mqtt_on: false });
    if (req.method === 'GET' && url.pathname === '/api/remote/state') return json(res, 200, scenario === 'dashboard'
      ? { supported: true, id: 1, key: '2', minutes: 0, accepted: true, sent: true, confirm: 'confirmed', age_s: 1, phase: 'unit-reported-mode', unit_mode: 'medium', unit_age_s: 0, next_request: 2, budget: { minute: 1, minute_max: 10, hour: 1, hour_max: 60, air_ms_hour: 11, retry_s: 0 } }
      : { supported: true, id: 0, key: '', minutes: 0, accepted: false, sent: false, confirm: 'none', age_s: 0, phase: 'idle', next_request: 1, budget: { minute: 0, minute_max: 10, hour: 0, hour_max: 60, air_ms_hour: 0, retry_s: 0 } });
    if (req.method === 'GET' && url.pathname === '/api/setup/read/state') {
      return json(res, 200, read.sid ? read : { ...read, remote: '29:010101', left_s: 0, ev: 0, rf: 'receiving', others: 0, why: '', why_ev: 0, why_cmd: '0000', why_mode: -1, steps: readSteps(0) });
    }
    if (req.method === 'GET' && url.pathname === '/api/setup/pair/state') return json(res, 200, pair);
    if (req.method === 'GET' && url.pathname === '/api/setup/console') return json(res, 200, { now: 50000, next: 1, oldest: 1, lost: 0, more: false, ev: [] });
    if (req.method === 'GET' && url.pathname === '/api/profiles') return json(res, 200, profiles());
    if (req.method === 'GET' && url.pathname === '/api/provision/key/file') return json(res, 200, { filename: 'orcon-recovery-key.pem', fingerprint: 'ab12-cd34-ef56', pem: PEM_PLACEHOLDER });
    if (req.method !== 'POST') return json(res, 404, { error: 'not-found' });

    const b = await body(req);
    if (url.pathname === '/api/auth/login') {
      if (b.password !== password) return json(res, 401, { error: 'bad-password', retry_s: 0 });
      newSession(res);
      return json(res, 200, { ok: true, token: csrf });
    }
    if (url.pathname === '/api/auth/logout') {
      sid = ''; csrf = '';
      res.setHeader('Set-Cookie', 'orcon_sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0');
      return json(res, 200, { ok: true });
    }
    if (url.pathname !== '/api/provision/password' && !loggedIn(req)) return json(res, 401, { error: 'login-required' });
    if (url.pathname === '/api/provision/password') {
      if (b.password !== password) return json(res, 400, { error: 'password-weak' });
      newSession(res);
      setup.state = 'setup';
      provision.state = 'setup';
      provision.password_set = true;
      provision.logged_in = true;
      return json(res, 200, { ok: true, token: csrf });
    }
    if (url.pathname === '/api/provision/wifi') {
      provision.wifi = { test: 'connected', ip: '192.0.2.24', ssid: b.ssid };
      return json(res, 202, { ok: true, test: 'testing' });
    }
    if (url.pathname === '/api/provision/key') {
      provision.key.pending = true;
      provision.key.pending_fingerprint = 'ab12-cd34-ef56';
      return json(res, 200, { ok: true, filename: 'orcon-recovery-key.pem', fingerprint: 'ab12-cd34-ef56' });
    }
    if (url.pathname === '/api/provision/key/verify') {
      if (b.pem !== PEM_PLACEHOLDER) return json(res, 400, { error: 'bad-file' });
      return json(res, 200, { ok: true, installed: true, fingerprint: 'ab12-cd34-ef56' });
    }
    if (url.pathname === '/api/provision/recover') {
      newSession(res);
      return json(res, 200, { ok: true, token: csrf });
    }
    if (url.pathname === '/api/setup/read/start') {
      read = { sid: 41, status: 'active', step: 2, remote: b.remote, left_s: 48, ev: 2, rf: 'receiving', others: 0, why: '', why_ev: 0, why_cmd: '0000', why_mode: -1, steps: readSteps(2) };
      return json(res, 201, read);
    }
    if (url.pathname === '/api/setup/read/cancel') {
      read.status = 'cancelled';
      return json(res, 200, read);
    }
    if (url.pathname === '/api/setup/pair/start') {
      pair = { ...pair, sid: 7, status: 'ready', left_s: 0 };
      return json(res, 201, pair);
    }
    if (url.pathname === '/api/setup/pair/press') {
      pair = { ...pair, status: 'bind_sent', bind_sent: true, left_s: 170 };
      return json(res, 200, pair);
    }
    if (url.pathname === '/api/setup/pair/cancel') {
      pair = { ...pair, status: 'cancelled' };
      return json(res, 200, pair);
    }
    if (url.pathname === '/api/remote/send') return json(res, 202, {
      supported: true, id: 1, key: b.key, minutes: 0, accepted: true, sent: true,
      confirm: 'confirmed', age_s: 0, phase: 'unit-reported-mode', unit_mode: b.key === '2' ? 'medium' : 'auto', unit_age_s: 0,
      next_request: 2, budget: { minute: 1, minute_max: 10, hour: 1, hour_max: 60, air_ms_hour: 11, retry_s: 0 },
    });
    if (url.pathname === '/api/profiles/select' || url.pathname === '/api/profiles/rename' || url.pathname === '/api/profiles/delete') return json(res, 200, profiles());
    if (url.pathname === '/api/wifi' || url.pathname === '/api/oled') return json(res, 200, { ok: 1 });
    if (url.pathname === '/api/setup/restart' || url.pathname === '/api/setup/wifi/forget') return json(res, 200, { ok: true, restarting: true });
    return json(res, 404, { error: 'not-found' });
  } catch (error) {
    json(res, 500, { error: 'mock-error' });
  }
});

async function main() {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({ executablePath: chrome, headless: true });
  const desktop = { viewport: { width: 1440, height: 900 }, deviceScaleFactor: 1 };
  let count = 0;

  async function pageFor(name) {
    await pageRequest(name);
    const context = await browser.newContext({ ...desktop, locale: 'nl-NL' });
    const page = await context.newPage();
    page.on('pageerror', error => { throw error; });
    await page.goto(`${base}/`, { waitUntil: 'networkidle' });
    return { context, page };
  }
  async function pageRequest(name, extra = {}) {
    const response = await fetch(`${base}/__mock/scenario`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name, ...extra }) });
    if (!response.ok) throw new Error('Could not select mock scenario.');
  }
  async function login(page) {
    await page.locator('#aPass').fill(password);
    await page.locator('#aGo').click();
    await page.waitForFunction(() => !document.getElementById('dash').hidden);
  }
  async function convert(name) {
    execFileSync('cwebp', ['-quiet', '-q', '92', '-m', '6', '-sharp_yuv', path.join(work, `${name}.png`), '-o', path.join(out, `${name}.webp`)]);
    fs.unlinkSync(path.join(work, `${name}.png`));
    console.log(`captured ${name}.webp`);
    count++;
  }
  async function snap(page, name) {
    await page.evaluate(() => document.activeElement && document.activeElement.blur());
    await page.screenshot({ path: path.join(work, `${name}.png`), fullPage: true });
    await convert(name);
  }
  async function snapPart(page, selector, name) {
    await page.locator(selector).screenshot({ path: path.join(work, `${name}.png`) });
    await convert(name);
  }
  async function close(context) { await context.close(); }

  {
    const { context, page } = await pageFor('login');
    await page.locator('#aLogin').waitFor({ state: 'visible' });
    await snap(page, 'login-desktop');
    await close(context);
  }
  {
    const { context, page } = await pageFor('dashboard');
    await login(page);
    await page.locator('[data-key="2"]').click();
    await page.waitForFunction(() => document.getElementById('c2').textContent.includes('ja'));
    await page.locator('#tech summary').click();
    await snap(page, 'dashboard-desktop');
    await snapPart(page, '.rc', 'remote-control-detail-desktop');
    await close(context);
  }
  {
    const { context, page } = await pageFor('wizard');
    await page.locator('#w1').waitFor({ state: 'visible' });
    await snap(page, 'setup-password-desktop');
    await page.locator('#w1a').fill(password);
    await page.locator('#w1b').fill(password);
    await page.locator('#w1Go').click();
    await page.locator('#w2').waitFor({ state: 'visible' });
    await page.locator('#wScan').click();
    await page.locator('#wSel').selectOption({ label: 'Demo Thuisnetwerk · -48 dBm · beveiligd' });
    await page.locator('#wPass').fill('tijdelijk-fictief-wachtwoord');
    await snap(page, 'setup-wifi-desktop');
    await page.locator('#wSave').click();
    await page.locator('#w3').waitFor({ state: 'visible' });
    await snap(page, 'setup-profile-desktop');
    await page.locator('#w3Next').click();
    await page.locator('#w4').waitFor({ state: 'visible' });
    await snap(page, 'setup-recovery-key-desktop');
    await page.locator('#w4p1').fill('fictieve sleutelzin voor lokale opname');
    await page.locator('#w4p2').fill('fictieve sleutelzin voor lokale opname');
    await page.locator('#w4Make').click();
    await page.locator('#w4b').waitFor({ state: 'visible' });
    await page.locator('#w4Paste').fill(PEM_PLACEHOLDER);
    await page.locator('#w4p3').fill('fictieve sleutelzin voor lokale opname');
    await page.locator('#w4Check').click();
    await page.locator('#w5').waitFor({ state: 'visible' });
    await snap(page, 'setup-complete-desktop');
    await close(context);
  }
  {
    const { context, page } = await pageFor('installed');
    await login(page);
    await page.locator('#cProf').click();
    await page.locator('#profP').waitFor({ state: 'visible' });
    await snap(page, 'profiles-desktop');
    await page.locator('[data-menu="2"]').click();
    await snap(page, 'profile-actions-desktop');
    await page.locator('#pNew').click();
    await page.locator('#newName').fill('Slaapkamer');
    await snap(page, 'profile-create-desktop');
    await close(context);
  }
  {
    const { context, page } = await pageFor('installed');
    await login(page);
    await page.evaluate(() => { location.hash = 'read'; });
    await page.locator('#readP').waitFor({ state: 'visible' });
    await snap(page, 'remote-read-select-desktop');
    await page.locator('[data-id="29:010101"]').click();
    await page.locator('#run').waitFor({ state: 'visible' });
    await snap(page, 'remote-read-progress-desktop');
    await pageRequest('installed', { read: 'done' });
    await page.locator('#doneP').waitFor({ state: 'visible' });
    await snap(page, 'remote-read-complete-desktop');
    await close(context);
  }
  {
    const { context, page } = await pageFor('installed');
    await login(page);
    await page.evaluate(() => { location.hash = 'pair'; });
    await page.locator('#pairA').click();
    await page.locator('[data-key="auto"]').click();
    await page.locator('[data-key="1"]').click();
    await snap(page, 'pairing-desktop');
    await page.locator('#pairA').click();
    await page.waitForFunction(() => document.getElementById('pairM').textContent.includes('nog geen bevestiging'));
    await snap(page, 'pairing-confirmation-desktop');
    await close(context);
  }
  {
    const { context, page } = await pageFor('installed');
    await login(page);
    await page.locator('#cWifi').click();
    await page.locator('#wifiP').waitFor({ state: 'visible' });
    await snap(page, 'wifi-settings-desktop');
    await page.locator('[data-open="wifinew"]').click();
    await page.locator('#wScan').click();
    await page.locator('#wSel').selectOption({ label: 'Demo Thuisnetwerk · -48 dBm · beveiligd' });
    await page.locator('#wPass').fill('tijdelijk-fictief-wachtwoord');
    await snap(page, 'wifi-select-desktop');
    await close(context);
  }
  {
    const { context, page } = await pageFor('installed');
    await login(page);
    await page.locator('#cSys').click();
    await page.locator('#sysP').waitFor({ state: 'visible' });
    await snap(page, 'system-desktop');
    await page.locator('[data-open="firmware"]').click();
    await page.locator('#fwUp').waitFor({ state: 'visible' });
    await snap(page, 'firmware-update-desktop');
    await close(context);
  }
  {
    const { context, page } = await pageFor('installed');
    await login(page);
    await page.locator('#cSys').click();
    await page.locator('[data-open="oled"]').click();
    await page.locator('#oledP').waitFor({ state: 'visible' });
    await snap(page, 'oled-settings-desktop');
    await close(context);
  }
  {
    const { context, page } = await pageFor('recovery');
    await page.locator('#aForgot').click();
    await page.locator('#recP').waitFor({ state: 'visible' });
    await snap(page, 'password-recovery-desktop');
    await close(context);
  }

  await browser.close();
  console.log(`${count} screenshots written to ${path.relative(root, out)}`);
}

main().catch(error => {
  console.error(error.message);
  process.exitCode = 1;
  process.exit(1);
}).finally(() => server.close());
