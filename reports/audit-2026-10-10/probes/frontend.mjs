// Reuse existing test scaffolding but execute production frontend functions.
import fs from 'node:fs';
import vm from 'node:vm';
const root = new URL('../../../', import.meta.url);
const fixture = new URL('tools/frontend.test.mjs', root);
let helpers = fs.readFileSync(fixture, 'utf8').split("test('actual WASM:")[0];
helpers = helpers.replaceAll('import.meta.url', JSON.stringify(fixture.href));
const { environment, fake, turn } = await import('data:text/javascript;base64,' + Buffer.from(helpers + '\nexport { environment, fake, turn };').toString('base64'));
const observations=[];
{
 const e=environment(true);await e.api.loadRom(fake('RESET',1),'reset.z64');
 const b=e.api.battery();b[0]=0xA5;e.api.setBattery(b);e.api.setDirty(7);
 await e.api.resetGame();await e.api.flushSaves();
 observations.push({case:'reset-before-save',dirty:e.api.dirty(),memory:e.api.battery()[0],persisted:e.records.has('save:'+e.api.getRom().key)});
}
{
 const e=environment(true);await e.api.loadRom(fake('STATE',2),'state.z64');
 const b=e.api.battery();b[0]=0x11;e.api.setBattery(b);e.api.setDirty(1);await e.api.flushSaves();await e.api.saveState();
 b[0]=0x22;e.api.setBattery(b);e.api.setDirty(1);await e.api.flushSaves();
 const restored=await e.api.loadState();await e.api.flushSaves();
 observations.push({case:'state-battery-rewind',restored,memory:e.api.battery()[0],persisted:new Uint8Array(e.records.get('save:'+e.api.getRom().key))[0],dirty:e.api.dirty()});
}
{
 const e=environment(true),a=fake('SAME',3),b=a.slice();b[4096]=0xAB;
 await e.api.loadRom(a,'original.z64');const key=e.api.getRom().key;
 await e.api.loadRom(b,'patched.z64');
 observations.push({case:'rom-content-collision',sameKey:key===e.api.getRom().key,libraryEntries:e.records.get('lib').length,activeByte:new Uint8Array(e.ex.memory.buffer)[e.api.pointer()+4099],cachedByte:new Uint8Array(e.records.get('rom:'+key))[4096]});
}
{
 const e=environment(true);let resolveA;
 const slow=new Promise(r=>resolveA=r);
 const a=e.api.openFile({name:'first.z64',size:8192,arrayBuffer:()=>slow});
 await e.api.openFile({name:'second.z64',size:8192,arrayBuffer:async()=>fake('SECOND',5).buffer});
 resolveA(fake('FIRST',4).buffer);await a;
 observations.push({case:'file-selection-order',lastSelected:'second',finalActive:e.api.getRom().name});
}
{
 const context=vm.createContext({});
 vm.runInContext(fs.readFileSync(new URL('src/web/gpu.js',root),'utf8')+'\nthis.Gpu=N64Gpu;',context);
 const g=new context.Gpu(),chunks=[];g.p={syncMax:1024};g.syncChunk=(i,n)=>chunks.push([i,n]);g.touched=new Map();
 g.syncRange(0x400000-2,4);g.touch(0x400000-2,4);
 observations.push({case:'gpu-wrapped-range',chunks,touched:[...g.touched],expected:[[0x400000-2,2],[0,2]]});
}
{
 const e=environment(), sel={options:[{value:'0'},{value:'1'},{value:'2'}],value:'2'};
 e.context.document.getElementById=()=>sel;
 let rejectFirst;const calls=[];
 const g={maxScaleLog2:2,setScale(n){calls.push(n);return calls.length===1?new Promise((_,reject)=>rejectFirst=reject):Promise.resolve(n);}};
 e.api.setGpu(g);
 vm.runInContext('useGpu=true; settings.scale=1; this.firstScale=applyScale(); settings.scale=2; this.secondScale=applyScale();',e.context);
 await e.context.secondScale;rejectFirst(new Error('old 2x build failed'));await e.context.firstScale;
 observations.push({case:'obsolete-scale-failure',calls,finalDesired:g.scaleWant,setting:vm.runInContext('settings.scale',e.context)});
}
{
 const context=vm.createContext({GPUTextureUsage:{},GPUBufferUsage:{}});
 vm.runInContext(fs.readFileSync(new URL('src/web/gpu.js',root),'utf8')+'\nthis.Gpu=N64Gpu;',context);
 const g=new context.Gpu(),allocated=[];
 function resource(kind){const r={kind,destroyed:false,destroy(){this.destroyed=true;},createView(){return {};}};allocated.push(r);return r;}
 g.module=async()=>({});g.samplers=[];g.hdWords=0x400000;
 g.device={
  createComputePipelineAsync:async d=>{if(d.compute.entryPoint==='ordered_main')throw Error('pipeline compilation failed');return {};},
  createRenderPipelineAsync:async()=>({}),createBuffer:()=>resource('buffer'),createTexture:()=>resource('texture')
 };
 try{await g.buildSet(true,2);}catch{}
 observations.push({case:'partial-gpu-build-cleanup',allocated:allocated.length,destroyed:allocated.filter(r=>r.destroyed).length});
}
console.log(JSON.stringify(observations,null,2));
