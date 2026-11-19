// web_page.h — the legacy RF log page (/log) and the browser upload page of the old TX builds
// (/update, RX_ONLY=0 only), served straight from flash as raw PROGMEM strings.
#pragma once
#include <Arduino.h>

// RX-only builds: banner + disabled controls on every page (the API refuses them anyway).
#define RXONLY_JS R"JS(function rxOnly(s){if(!s||!s.rx_only||window.rxo)return;window.rxo=1;document.body.insertAdjacentHTML('afterbegin','<div id="rxonly" style="background:#7a2e00;color:#fff;padding:8px 12px;font:600 14px system-ui">RX-ONLY TEST FIRMWARE (fw '+String(s.fw||'?').slice(0,7)+') &mdash; receive only, no 868 MHz transmission. Controls, pairing and firmware upload are disabled.</div>');document.querySelectorAll('[data-m],[data-t],[data-b],[data-bypass],[data-ac],[data-accustom],[data-pair],[data-reapply],[data-forget],[data-selftest],[data-connect],[data-status],[data-raw],.ctl,#ar,#pl').forEach(function(e){e.disabled=true;});document.querySelectorAll('a[href="/update"]').forEach(function(a){a.style.display='none';});})JS"

// --- RF log + device-ID / clone page (served at /log) ------------------------
static const char LOG_HTML[] PROGMEM = R"LOG(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Orcon 15RF — RF log</title>
<style>
:root{--bg:#10141a;--card:#1a212b;--fg:#e7edf3;--mut:#8b97a7;--acc:#3da9fc;--rx:#2ecc71;--tx:#f5b041}
*{box-sizing:border-box}body{margin:0;font:14px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
.wrap{max-width:760px;margin:0 auto;padding:16px}h1{font-size:18px;margin:6px 0 12px}
.card{background:var(--card);border-radius:14px;padding:14px;margin-bottom:14px}
label{display:block;color:var(--mut);font-size:12px;margin:8px 0 4px}
input,button{font:inherit;color:var(--fg);background:#26303c;border:1px solid #313d4c;border-radius:9px;padding:9px}
button{cursor:pointer}button.go{background:var(--acc);border-color:var(--acc);color:#04121f;font-weight:600}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:end}.small{font-size:12px;color:var(--mut)}
pre{background:#0c1016;border-radius:10px;padding:10px;overflow:auto;max-height:60vh;font:12px/1.45 ui-monospace,Menlo,Consolas,monospace;margin:0;white-space:pre-wrap;overflow-wrap:anywhere}
.rx{color:var(--rx)}.tx{color:var(--tx)}.tag{display:inline-block;width:14px}.entry{display:block;padding:5px 0;border-bottom:1px solid #232c38}
b{color:var(--fg)}
</style></head><body><div class="wrap">
<h1>RF log &amp; device identity</h1>

<div class="card">
<div class="small">Our ID <b id="usid">–</b> &nbsp;·&nbsp; selected unit <b id="fanid">–</b></div>
<div id="seen" class="small" style="margin-top:8px"></div>
<div id="observed" class="small" style="margin-top:8px">Waiting for decoded RF frames…</div>
<div id="rfdiag" class="small" style="margin-top:8px">Waiting for raw radio diagnostics…</div>
<div class="row" style="margin-top:10px">
<div><label>This device ID (cc:serial)</label><input id="us" placeholder="29:012345" style="width:150px"></div>
<div><label>Our unit (cc:serial)</label><input id="fan" placeholder="29:000202" style="width:150px"></div>
<button class="go" id="apply" style="margin-top:18px">Apply &amp; save</button>
<button id="clone" style="margin-top:18px">Copy heard remote as our ID</button>
</div>
<div id="idmsg" class="small" style="margin-top:8px"></div>
<p class="small">Choose <b>our unit</b> yourself from the heard units: neighbours share 868 MHz, so a heard
unit is not necessarily ours (check that it answers your own remote). Nothing is selected automatically.
Cloning (TX builds): press your remote, copy its ID as our ID, Apply.</p>
</div>

<div class="card">
<div class="row" style="justify-content:space-between">
<div><span class="rx">■</span> received &nbsp; <span class="tx">■</span> sent</div>
<label class="small"><input type="checkbox" id="auto" checked style="width:auto"> auto-refresh</label>
</div>
<pre id="log">loading…</pre>
</div>
<p class="small"><a href="/" style="color:var(--acc)">&larr; back to controls</a></p>
</div>
<script>)LOG" RXONLY_JS R"LOG(
const $=s=>document.querySelector(s);
let seenR='',selFan='';
function st(){fetch('/api/state',{cache:'no-store'}).then(r=>r.json()).then(s=>{rxOnly(s);
  selFan=s.fan_known?s.fan_id:'';
  $('#usid').textContent=s.us_id;$('#fanid').textContent=selFan||'none selected';
  if(s.rx_only)$('#clone').style.display='none';   // receive-only: no identity to clone
  $('#observed').textContent='Decoded RX: '+s.rx_n+' · Last mode from RF: '+(s.mode||'unknown')+' (not verified fan speed)';
  const age=ms=>ms==null||ms<0?'never':Math.round(ms/1000)+'s ago';
  $('#rfdiag').textContent='Raw UART bytes: '+(s.rf_uart_bytes??'-')+
    ' · Sync markers: '+(s.rf_sync_hits??'-')+
    ' · Last raw data: '+age(s.rf_last_uart_age_ms)+
    ' · Last sync: '+age(s.rf_last_sync_age_ms)+
    ' · Buffer resets: '+(s.rf_buffer_resets??'-')+
    ' · Pending bytes: '+(s.rf_pending_bytes??'-');
  if(!$('#us').value)$('#us').value=s.us_id;
  if(!$('#fan').value&&s.fan_id&&!s.fan_id.startsWith('63'))$('#fan').value=s.fan_id;
})}
function fmtAge(t,now){let a=Math.max(0,(now-t)/1000);return a<99?a.toFixed(0)+'s':(a/60).toFixed(0)+'m'}
function load(){fetch('/api/log',{cache:'no-store'}).then(r=>r.json()).then(j=>{
  seenR=j.seen_remote||'';
  const devs=j.devices||[], units=devs.filter(d=>d.role.indexOf('unit')>=0), rems=devs.filter(d=>d.role.indexOf('remote')>=0);
  $('#seen').innerHTML='Heard units ('+units.length+', discovery only — pick ours explicitly):<br>'+
    (units.map(d=>'<button data-pick="'+d.id+'" style="width:auto;margin:2px 6px 2px 0">'+d.id+(d.id===selFan?' ✓ selected':' select')+'</button>×'+d.n+' · '+d.age_s+'s ago · '+d.rssi+' dBm').join('<br>')||'none yet')+
    '<br>Heard remotes: '+(rems.map(d=>d.id+' ×'+d.n+' '+d.rssi+' dBm').join(' · ')||'none yet');
  document.querySelectorAll('[data-pick]').forEach(b=>b.onclick=()=>{$('#fan').value=b.dataset.pick;
    $('#idmsg').textContent='Unit '+b.dataset.pick+' entered. Check it is YOUR unit (it answers your own remote), then Apply & save.';});
  const now=j.uptime_ms||Math.max(...j.log.map(e=>e.t),0);
  $('#log').innerHTML=j.log.slice().reverse().map(e=>{
    const c=e.d==='R'?'rx':'tx';const r=e.d==='R'?(' '+e.r+'dBm'):'';
    return '<span class="entry '+c+'"><span class="tag">'+(e.d==='R'?'←':'→')+'</span>'+
           fmtAge(e.t,now).padStart(4)+r.padStart(7)+'  '+e.s+'</span>';
  }).join('\n')||'(nothing yet)';
})}
$('#clone').onclick=()=>{if(seenR)$('#us').value=seenR;};   // never fills the unit
$('#apply').onclick=()=>{
  const pa=s=>{const m=(s||'').split(':');return m.length===2?[parseInt(m[0],10),parseInt(m[1],10)]:null};
  const u=pa($('#us').value),f=pa($('#fan').value);
  if(!u){$('#idmsg').textContent='Enter device ID as cc:serial, e.g. 29:012345';return;}
  const body={us_class:u[0],us_serial:u[1]};
  if(f){body.fan_class=f[0];body.fan_serial=f[1];}
  fetch('/api/ids',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
   .then(r=>r.json().then(j=>{$('#idmsg').textContent=r.ok?'Saved. Selected unit: '+($('#fan').value||'none')+'.':'Not saved: '+(j.err||r.status);st();}));
};
st();load();setInterval(()=>{if($('#auto').checked)load();},1500);setInterval(st,4000);
</script></body></html>)LOG";

// --- /update : browser firmware upload (Update.h, no CLI needed) ------------
static const char UPDATE_HTML[] PROGMEM = R"UPD(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Orcon firmware update</title>
<style>
:root{--bg:#10141a;--card:#1a212b;--fg:#e7edf3;--mut:#8b97a7;--acc:#3da9fc;--ok:#2ecc71;--warn:#e74c3c}
*{box-sizing:border-box}body{margin:0;font:15px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
.wrap{max-width:520px;margin:0 auto;padding:16px}h1{font-size:18px;margin:6px 0 14px}
.card{background:var(--card);border-radius:14px;padding:16px;margin-bottom:14px}
input[type=file]{width:100%;color:var(--mut);margin-bottom:12px}
button{width:100%;font:inherit;color:#04121f;background:var(--acc);border:0;border-radius:10px;padding:13px;font-weight:600;cursor:pointer}
.barwrap{height:10px;background:#26303c;border-radius:5px;overflow:hidden;margin-top:14px}
#bar{height:100%;width:0;background:var(--acc);transition:width .2s}
.small{font-size:12.5px;color:var(--mut)}a{color:var(--acc);text-decoration:none}
</style></head><body><div class="wrap">
<h1>Firmware update (OTA)</h1>
<div class="card">
<input type="file" id="fw" accept=".bin">
<button id="go">Flash firmware</button>
<div class="barwrap"><div id="bar"></div></div>
<div id="msg" class="small" style="margin-top:10px">Pick the compiled <span style="color:var(--fg)">firmware.bin</span> and flash. The device reboots into it automatically.</div>
</div>
<div class="card small">
The build output is at<br><span style="color:var(--fg)">.pio/build/heltec_v3_cc1101_ota/firmware.bin</span><br><br>
Settings, WiFi and saved fan presets are kept across an update.<br><br>
<a href="/dashboard">&larr; back to control</a>
</div>
<script>
const $=s=>document.getElementById(s);
$('go').onclick=()=>{
 const f=$('fw'); if(!f.files.length){alert('Pick a firmware .bin first');return;}
 const file=f.files[0];
 if(!confirm('Flash '+file.name+' ('+((file.size/1024)|0)+' KB)? The device will reboot.'))return;
 const fd=new FormData(); fd.append('update',file);
 const x=new XMLHttpRequest(); x.open('POST','/update');
 x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);$('bar').style.width=p+'%';$('msg').textContent='Uploading '+p+'%';}};
 x.onload=()=>{let ok=false;try{ok=JSON.parse(x.responseText).ok==1;}catch(e){}
   $('msg').textContent=ok?'Flashed — rebooting, this page reloads in a few seconds…':'Update failed (bad file or out of space).';
   $('bar').style.background=ok?'var(--ok)':'var(--warn)';
   if(ok)setTimeout(()=>location.href='/dashboard',9000);};
 x.onerror=()=>{$('msg').textContent='Upload error — connection dropped.';};
 $('msg').textContent='Starting upload…';
 x.send(fd);
};
</script></body></html>)UPD";

