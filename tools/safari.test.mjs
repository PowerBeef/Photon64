// Execute the production journaling/request function without macOS WebDriver.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
test('Safari command journal accepts numeric timeout bodies and retains first errors', async () => {
  const s=fs.readFileSync(new URL('./safaricheck.mjs',import.meta.url),'utf8'), a=s.indexOf('async function command('), b=s.indexOf('\nconst cmd =',a);
  const requests=[],report={journal:[]},c=vm.createContext({report,phase:'setup',port:1,Date,JSON,Error,AbortSignal,diagnostic:false,deadline:Date.now()+10000,checkpoint(){},fetch:async(url,opts)=>{requests.push(JSON.parse(opts.body));return {ok:true,json:async()=>({value:{}})};}});
  vm.runInContext(s.slice(a,b)+'\nthis.run=command',c);
  await c.run('POST','/timeouts',{script:30000,pageLoad:60000,implicit:0});
  await c.run('POST','/execute/sync',{script:'return true;',args:[]});
  assert.equal(requests[0].script,30000);assert.equal(report.journal[0].script,undefined);
  assert.equal(report.journal[1].script,'return true;');assert.ok(report.journal.every(x=>x.started&&x.finished));
  c.fetch=async()=>{throw new Error('driver unavailable')};
  await assert.rejects(c.run('POST','/actions',{}),/driver unavailable/);
  assert.match(report.journal[2].error,/driver unavailable/);
});
