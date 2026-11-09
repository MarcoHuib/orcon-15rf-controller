// Dedicated dry-run UI: no legacy controls, external resources or persisted secrets.
#pragma once
static const char TX_POC_HTML[] PROGMEM=R"POC(<!doctype html>
<html lang="nl"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Orcon TX-PoC dry-run</title>
<style>body{font:17px system-ui;max-width:640px;margin:2em auto;padding:1em;background:#101820;color:#eee}button,input{font:inherit;padding:.6em;margin:.3em}pre{white-space:pre-wrap}strong{color:#ffd080}</style>
<h1>TX-PoC: uitsluitend dry-run</h1><p><strong>Geen RF-uitzending. De radio blijft in RX.</strong></p>
<p>Alleen op een vertrouwd, afgeschermd lokaal netwerk. Dit portaal gebruikt HTTP.</p>
<label>Bootsleutel van het OLED <input id="token" type="password" maxlength="32" autocomplete="off"></label>
<button id="login">Open lokale sessie</button><p id="access">Geen sessie. Sleutel: maximaal 60 seconden op het wakkere OLED; sessie maximaal 10 minuten.</p>
<p>Bron en unit komen uit bestaande NVS-configuratie. Geen pairing, cloning of identiteiten wijzigen.</p>
<div><button data-mode="1" disabled>Stand 1 simuleren</button><button data-mode="2" disabled>Stand 2 simuleren</button><button data-mode="3" disabled>Stand 3 simuleren</button></div>
<pre id="status">Nog geen opdracht.</pre><button id="check">Opnieuw controleren</button><button id="retry">Zelfde aanvraag opnieuw aanbieden</button>
<script>
let token='',csrf='',next=0,busy=true,pending=null,refreshing=false;
const $=id=>document.getElementById(id),buttons=[...document.querySelectorAll('[data-mode]')];
function lock(){buttons.forEach(b=>b.disabled=busy||!csrf||!next||pending!==null)}
async function state(){
 if(refreshing||!token)return;refreshing=true;
 try{const r=await fetch('/api/tx-poc/state',{headers:{Authorization:'Bearer '+token},cache:'no-store'});
 if(!r.ok)throw Error('Toegang geweigerd ('+r.status+')');
 const s=await r.json();csrf=s.csrf;next=s.next_request;busy=s.busy||s.locked||!s.ready;
 $('access').textContent='Dry-run-sessie. Bron: '+s.source_id+'; vergrendelde unit: '+s.unit_id+'. RF-capable: nee; RF-vrijgave: nee.';
 $('status').textContent='Transactie '+s.request+': '+s.phase+'\nFout: '+s.error+'\nRX-controle: '+s.rx_restored+'\nFramebytes: '+s.bytes+'\nRF verzonden: nee\nUnitbevestiging: geen (dry-run)';
 }catch(e){csrf='';busy=true;$('access').textContent=e.message}finally{refreshing=false;lock()}
}
$('login').onclick=async()=>{token=$('token').value;$('token').value='';pending=null;await state()};
async function send(mode){
 if(!pending)pending={mode:mode,request:next};lock();
 try{const r=await fetch('/api/tx-poc/mode',{method:'POST',headers:{Authorization:'Bearer '+token,'X-CSRF-Token':csrf,'Content-Type':'application/json'},body:JSON.stringify(pending)});
 const j=await r.json();if(!r.ok)throw Error(j.error||('HTTP '+r.status));
 pending=null;await state();
 }catch(e){$('status').textContent='Geen acceptatie bewezen: '+e.message+'. Geen automatische herhaling.';}
 finally{lock()}
}
buttons.forEach(b=>b.onclick=()=>send(Number(b.dataset.mode)));
$('check').onclick=state;$('retry').onclick=()=>{if(pending)send(pending.mode)};
setInterval(state,1000);
</script></html>)POC";
