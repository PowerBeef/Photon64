// Continuous real-Dawn execution and reset/state transitions against the software core.
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { create, globals } from 'webgpu';
import { gpuFixture } from './gpu_fixture.mjs';
import { seedSoftwareCore } from './core_checkpoint.mjs';
const native = create([]), ctx = vm.createContext({ ...globals, console, navigator: { gpu: native }, performance, Uint8Array, Uint16Array, Uint32Array, ArrayBuffer, Map, Set, Promise, Error, Math });
const gpuSource = fs.readFileSync('src/web/gpu.js', 'utf8');
vm.runInContext(gpuSource + '\nthis.N64Gpu=N64Gpu;', ctx);
const wasmBytes = fs.readFileSync('out/n64.wasm'), wasm = new WebAssembly.Module(wasmBytes), data = process.argv[2] ? fs.readFileSync(process.argv[2]) : gpuFixture();
let gpu;
const make = flush => { const ex = new WebAssembly.Instance(wasm, { env: { host_log() {}, host_gpu_flush: flush } }).exports, ptr = ex.n64_reserve_rom(data.length); new Uint8Array(ex.memory.buffer, ptr, data.length).set(data); assert.equal(ex.n64_load(ptr, data.length), 1); return { ex, ptr }; };
const a = make(() => gpu?.flush()), b = make(() => {}), shaders = { allowFallback: true };
const warmup = +(process.env.GPU_WARMUP || 0), inputPath = process.env.GPU_INPUTS;
const checkpointWarmup = process.env.GPU_WARMUP_SNAPSHOT === '1';
const inputs = inputPath ? fs.readFileSync(inputPath,'utf8').trim() : '';
const masks = { A:32768, B:16384, Z:8192, START:4096, DU:2048, DD:1024, DL:512, DR:256, L:32, R:16, CU:8, CD:4, CL:2, CR:1 };
const events = inputs ? inputs.split(',').map(s => { const [f,k,d] = s.split(':'); return { f:+f, keys:k.split('+'), d:+d }; }) : [];
function input(f) {
  let buttons=0,x=0,y=0;
  for(const e of events) if(f>=e.f && f<e.f+e.d) for(const k of e.keys) { if(k.startsWith('X='))x=+k.slice(2);else if(k.startsWith('Y='))y=+k.slice(2);else buttons|=masks[k]||0; }
  for(const c of [a,b])c.ex.n64_input(0,buttons,x,y);
}
assert.ok(Number.isSafeInteger(warmup) && warmup>=0);
for(let f=0;f<warmup;f++) {
  input(f); assert.equal(a.ex.n64_frame(),0);
  if(!checkpointWarmup) assert.equal(b.ex.n64_frame(),0);
  if((f+1)%2000===0) console.error(`Software warmup: ${f+1}/${warmup} fields`);
}
if(checkpointWarmup && warmup) seedSoftwareCore(a,b,data.length);
for (const n of ['rdp', 'vi', 'merge']) shaders[n] = fs.readFileSync(`src/web/${n}.wgsl`, 'utf8');
gpu = await ctx.N64Gpu.create(a, null, shaders); assert.ok(gpu, 'required adapter missing'); gpu.reset(); a.ex.n64_config(0, 1);
if(process.env.GPU_LIFECYCLE_HD === '1') await gpu.setScale(0,true);
let snapshots, fields = 0, bytes = 0;
const frames = +(process.env.GPU_FIELDS || 180);
assert.ok(Number.isSafeInteger(frames) && frames >= 91, 'At least 91 fields are required for every lifecycle transition');
for (let i = 0; i < frames; i++) {
  input(warmup+i);
  while (a.ex.n64_frame()) await gpu.syncNow(); b.ex.n64_frame(); await gpu.syncNow();
  const hi = new Uint32Array(a.ex.memory.buffer, a.ex.n64_host_info(), 24), bh = new Uint32Array(b.ex.memory.buffer, b.ex.n64_host_info(), 24);
  // Compare complete CPU-visible RAM and effective hidden bits, after every field.
  assert.deepEqual(new Uint8Array(a.ex.memory.buffer, hi[0], 0x800000), new Uint8Array(b.ex.memory.buffer, bh[0], 0x800000), `RDRAM field ${i}`);
  const ahidden = new Uint8Array(a.ex.memory.buffer, hi[19], 0x400000), bhidden = new Uint8Array(b.ex.memory.buffer, bh[19], 0x400000);
  const aram = new Uint16Array(a.ex.memory.buffer, hi[0], 0x400000), bram = new Uint16Array(b.ex.memory.buffer, bh[0], 0x400000);
  const ashadow = new Uint16Array(a.ex.memory.buffer, hi[20], 0x400000), bshadow = new Uint16Array(b.ex.memory.buffer, bh[20], 0x400000);
  for (let j = 0; j < ahidden.length; j++) {
    const av = aram[j ^ 1], bv = bram[j ^ 1];
    const ah = (ahidden[j] & 128) || ashadow[j] !== av ? (av & 1) * 3 : ahidden[j] & 3;
    const bh = (bhidden[j] & 128) || bshadow[j] !== bv ? (bv & 1) * 3 : bhidden[j] & 3;
    if (ah !== bh) throw new Error(`effective hidden ${i}/${j}: ${ah} != ${bh}`);
  }
  assert.deepEqual(new Int32Array(a.ex.memory.buffer, a.ex.n64_rdp_feedback(), 12), new Int32Array(b.ex.memory.buffer, b.ex.n64_rdp_feedback(), 12));
  fields++; bytes += 0xC00000;
  if (i === 30) snapshots = [a,b].map(c => new Uint8Array(c.ex.memory.buffer, 0, c.ex.n64_state_size()).slice());
  if (i === 60) { for (const [j,c] of [a,b].entries()) { new Uint8Array(c.ex.memory.buffer).set(snapshots[j]); c.ex.n64_set_rom(c.ptr,data.length); } gpu.reset(); a.ex.n64_config(0, 1); }
  if (i === 90) { a.ex.n64_reset(); b.ex.n64_reset(); gpu.reset(); a.ex.n64_config(0, 1); }
}
assert.ok(gpu.stats.batches > 0, 'No GPU drawing occurred; lifecycle coverage is incomplete');
console.log(JSON.stringify({ outcome: 'PASS', fields, warmup, warmupStrategy: checkpointWarmup ? 'one software core, cloned checkpoint' : 'two software cores', inputScript: inputPath || null, inputSha256: crypto.createHash('sha256').update(inputs).digest('hex'), romSha256: crypto.createHash('sha256').update(data).digest('hex'), wasmSha256: crypto.createHash('sha256').update(wasmBytes).digest('hex'), gpuSourceSha256: crypto.createHash('sha256').update(gpuSource).digest('hex'), comparedBytes: bytes, coverage: 'continuous GPU/software RAM, hidden bits and retained registers; state restore and reset', independentHardware: false, ...gpu.diagnostics() }));
gpu.dispose(); gpu.device.destroy(); process.exit(0);
