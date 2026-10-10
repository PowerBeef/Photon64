import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const context = vm.createContext({ console, GPUBufferUsage: { MAP_READ: 1, COPY_DST: 2 }, GPUMapMode: { READ: 1 } });
vm.runInContext(fs.readFileSync(new URL('../src/web/gpu.js', import.meta.url), 'utf8') + '\nthis.N64Gpu = N64Gpu;', context);
const proto = context.N64Gpu.prototype;
function gpu(map = async () => {}) {
  const mem = new ArrayBuffer(128), calls = { done: 0, apply: 0, destroy: 0, scan: [], uploads: [] };
  const g = Object.assign(Object.create(proto), { mem: { buffer: mem }, p: { syncMax: 4, nruns: 0, runs: 4, stage: 20 },
    scratch: 0, scratchWords: 16, fbBuf: {}, touched: new Map(), inflight: [], pool: [],
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
  const { g, calls } = gpu(); g.syncRange(0x400000 - 2, 10); assert.deepEqual(calls.scan, [[0x400000 - 2, 2]]);
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
