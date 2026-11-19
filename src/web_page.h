// web_page.h — the browser upload page of the old TX builds (/update, RX_ONLY=0 only), served
// straight from flash as a raw PROGMEM string.
#pragma once
#include <Arduino.h>

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

