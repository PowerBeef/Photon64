import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const context = vm.createContext({ console, performance, GPUBufferUsage: { MAP_READ: 1, COPY_DST: 2 }, GPUMapMode: { READ: 1 } });
vm.runInContext(fs.readFileSync(new URL('../src/web/gpu.js', import.meta.url), 'utf8') + '\nthis.N64Gpu = N64Gpu;', context);
const proto = context.N64Gpu.prototype;
function gpu(map = async () => {}) {
  const mem = new ArrayBuffer(160), calls = { done: 0, apply: 0, destroy: 0, scan: [], uploads: [] };
  const g = Object.assign(Object.create(proto), { mem: { buffer: mem }, p: { syncMax: 4, nruns: 0, runs: 4, stage: 20, feedback: 80, feedbackDirty: 136 },
    scratch: 0, scratchWords: 16, fbBuf: {}, native: { feedbackBuf: {} }, touched: new Map(), inflight: [], pool: [],
    stats: { waits: 0, late: 0, syncs: 0, readbacks: 0, uploads: 0 },
    ex: { n64_sync_done: () => calls.done++, n64_readback_apply: () => calls.apply++,
      n64_sync_scan: (idx, n) => { calls.scan.push([idx, n]); const u = new Uint32Array(mem); u[0] = 1; u[1] = n - 1; u[2] = 1; return 1; },
      n64_config() {}, n64_sync_full() {} },
    device: { createBuffer: () => ({ size: 64, mapAsync: map, getMappedRange: (_, bytes) => new ArrayBuffer(bytes), unmap() {}, destroy() { calls.destroy++; } }),
      createCommandEncoder: () => ({ copyBufferToBuffer() {}, finish() { return {}; } }),
      queue: { submit() {}, writeBuffer: (_, offset) => calls.uploads.push(offset) } } });
  return { g, calls };
}
test('full sync range includes every chunk tail', () => {
  for (const n of [0, 4, 5, 8, 9]) { const { g, calls } = gpu(); g.syncRange(10, n);
    assert.deepEqual(calls.scan, Array.from({ length: Math.ceil(n / 4) }, (_, i) => [10 + i * 4, Math.min(4, n - i * 4)]));
    assert.deepEqual(calls.uploads, calls.scan.map(([i, c]) => (i + c - 1) * 4)); }
  const { g, calls } = gpu(); g.syncRange(0x400000 - 2, 10); assert.deepEqual(calls.scan, [[0x400000 - 2, 2], [0, 4], [4, 4]]);
  g.p.syncMax = 0; assert.throws(() => g.syncRange(0, 1), /capacity/);
});
test('rejected readback retains regions, destroys staging and never acknowledges', async () => {
  const { g, calls } = gpu(async () => { throw new Error('map failed'); }); g.touch(0, 4);
  await assert.rejects(g.syncNow(), /map failed/); assert.equal(calls.done, 0); assert.equal(calls.apply, 0);
  assert.equal(calls.destroy, 1); assert.equal(g.inflight.length, 0); assert.equal(g.touched.get(0), 4);
  await assert.rejects(g.syncNow(), /map failed/); assert.equal(calls.done, 0);
});
test('submission failure also retains regions and destroys resources', async () => {
  const { g, calls } = gpu(); g.touch(0, 4); g.device.queue.submit = () => { throw new Error('submit failed'); };
  await assert.rejects(g.syncNow(), /submit failed/); assert.equal(calls.done, 0); assert.equal(calls.destroy, 1); assert.equal(g.touched.get(0), 4);
});
test('successful barrier applies bytes before acknowledging', async () => {
  const { g, calls } = gpu(); g.touch(0, 4); await g.syncNow(); assert.equal(calls.apply, 1); assert.equal(calls.done, 1); assert.equal(calls.destroy, 0); assert.equal(g.pool.length, 1);
});
test('old generation completion cannot apply bytes after reset', async () => {
  let release; const { g, calls } = gpu(() => new Promise(r => { release = r; })); g.touch(0, 4); g.startReadback();
  const old = g.tail; g.p.syncMax = 0x400000; g.reset(); release(); await old; assert.equal(calls.apply, 0); assert.equal(calls.destroy, 1);
});

test('obsolete synchronization barrier cannot acknowledge a new generation', async () => {
  let release; const { g, calls } = gpu(() => new Promise(r => { release = r; }));
  g.touch(0, 4); const pending = g.syncNow(); g.p.syncMax = 0x400000; g.reset();
  const acknowledgements = calls.done; release();
  await assert.rejects(pending, /Obsolete/); assert.equal(calls.done, acknowledgements); assert.equal(calls.apply, 0);
});

test('lost device never acknowledges an empty synchronization barrier', async () => {
  const { g, calls } = gpu(); g.lost = true;
  await assert.rejects(g.syncNow(), /device lost/);
  assert.equal(calls.done, 0);
});

test('feedback snapshot offsets remain aligned for odd halfword counts', async () => {
  const { g } = gpu();
  g.device.createBuffer = () => ({ size: 256, mapAsync: async () => {}, getMappedRange(offset, size) {
    assert.equal(offset % 8, 0); return new ArrayBuffer(size);
  }, unmap() {}, destroy() {} });
  g.touch(1, 1); await g.syncNow();
});


test('negative apron and circular touched ranges include both ends', () => {
  const { g, calls } = gpu(); g.syncRange(-2, 4); assert.deepEqual(calls.scan, [[0x400000 - 2, 2], [0, 2]]);
  g.touch(0x400000 - 2, 4); assert.equal(g.touched.get(0), 2); assert.equal(g.touched.get(0x400000 - 2), 2);
  g.touch(7, 0x400001); assert.equal(g.touched.get(0), 0x400000);
});
test('debug readback map failure destroys its staging buffer', async () => {
  const { g, calls } = gpu(async () => { throw new Error('debug map failed'); });
  await assert.rejects(g.readBuf({}, 0, 16), /debug map failed/); assert.equal(calls.destroy, 1);
});
test('asynchronous validation error blocks acknowledgement and records first cause', async () => {
  const { g, calls } = gpu(); g.device.pushErrorScope = () => {}; g.device.popErrorScope = async () => ({ message: 'invalid dispatch' });
  g.touch(0, 4); await assert.rejects(g.syncNow(), /invalid dispatch/); assert.equal(calls.done, 0); assert.equal(g.firstError.phase, 'readback');
});
test('partially built scale set is destroyed after pipeline rejection', async () => {
  const { g } = gpu(); let allocated = 0, destroyed = 0;
  context.GPUTextureUsage = {}; context.GPUBufferUsage.STORAGE = 4;
  const resource = () => { allocated++; return { destroy() { destroyed++; }, createView() {} }; };
  g.hdWords = 0x400000; g.module = async () => ({});
  g.device.createBuffer = resource; g.device.createTexture = resource;
  g.device.createRenderPipelineAsync = async () => ({});
  g.device.createComputePipelineAsync = async d => { if (d.compute.entryPoint === 'ordered_main') throw new Error('pipeline failed'); return {}; };
  await assert.rejects(g.buildSet(true, 1), /pipeline failed/); assert.equal(allocated, 4); assert.equal(destroyed, 4);
});

test('upload validation cannot be swallowed by an outer scale-build scope', async () => {
  const {g,calls}=gpu();let depth=1;
  g.device.pushErrorScope=()=>{depth++};
  g.device.popErrorScope=()=>{assert.equal(depth--,2);return Promise.resolve({message:'invalid upload'})};
  g.writeBuffer({},0,new Uint32Array(1));await assert.rejects(g.syncNow(),/invalid upload/);
  assert.equal(depth,1);assert.equal(calls.done,0);assert.equal(g.firstError.phase,'upload');
});
test('completed validation tails retain no nested submission history', async () => {
  const {g}=gpu();g.device.pushErrorScope=()=>{};g.device.popErrorScope=async()=>null;
  for(let i=0;i<128;i++)g.submit(g.device.createCommandEncoder(),'render');
  assert.equal(await g.validationTail,undefined);assert.equal(g.firstError,undefined);
});
