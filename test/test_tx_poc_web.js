// Execute the actual emitted PoC page. Test browser actions, not a duplicate UI.
const assert=require('node:assert/strict'),vm=require('node:vm'),fs=require('node:fs');
const script=[...fs.readFileSync(0,'utf8').matchAll(/<script>([\s\S]*?)<\/script>/g)].map(x=>x[1]).join('\n');
const token='0123456789abcdef0123456789abcdef';
async function page(){
 const elements=Object.fromEntries(['token','login','access','status','check','retry'].map(x=>[x,{value:'',textContent:'',disabled:false}]));
 const buttons=[1,2,3].map(mode=>({dataset:{mode:String(mode)},disabled:true}));
 const requests=[];let fail=false;
 const document={getElementById:id=>elements[id],querySelectorAll:()=>buttons};
 const fetch=async(url,options={})=>{
  requests.push({url,...options});
  assert.equal(options.headers.Authorization,'Bearer '+token);
  if(options.method==='POST'){
   assert.equal(options.headers['X-CSRF-Token'],'csrf-test');
   assert.equal(options.headers['Content-Type'],'application/json');
   if(fail)throw Error('synthetic disconnect');
   return {ok:true,json:async()=>({})};
  }
  return {ok:true,json:async()=>({csrf:'csrf-test',next_request:1,busy:false,ready:true,phase:'idle',error:'none',rx_restored:false,bytes:0,request:0,source_id:'29:000101',unit_id:'29:000202'})};
 };
 vm.runInNewContext(script,{document,fetch,setInterval:()=>0});
 return {elements,buttons,requests,setFail:value=>{fail=value}};
}
(async()=>{
 let p=await page();assert(p.buttons.every(b=>b.disabled));assert.equal(p.requests.length,0,'no request/command on load');
 p.elements.token.value=token;await p.elements.login.onclick();assert.equal(p.elements.token.value,'');
 assert(p.buttons.every(b=>!b.disabled));assert.equal(p.requests.filter(r=>r.method==='POST').length,0);
 await p.buttons[1].onclick();let posts=p.requests.filter(r=>r.method==='POST');
 assert.equal(posts.length,1);assert.deepEqual(JSON.parse(posts[0].body),{mode:2,request:1});
 assert.match(p.elements.status.textContent,/RF verzonden: nee/);assert.match(p.elements.status.textContent,/Unitbevestiging: geen/);
 p=await page();p.elements.token.value=token;await p.elements.login.onclick();p.setFail(true);
 await p.buttons[2].onclick();assert(p.buttons.every(b=>b.disabled));
 await p.elements.check.onclick();assert.equal(p.requests.filter(r=>r.method==='POST').length,1,'poll cannot resend');
 p.setFail(false);await p.elements.retry.onclick();posts=p.requests.filter(r=>r.method==='POST');
 assert.equal(posts.length,2);assert.equal(posts[0].body,posts[1].body,'explicit retry retains idempotency request');
 assert(!script.includes('localStorage')&&!script.includes('sessionStorage'));
 console.log('PASS PoC page: no boot command, auth/CSRF, mode action, honest dry-run status, no automatic retry');
})().catch(e=>{console.error(e);process.exitCode=1});
