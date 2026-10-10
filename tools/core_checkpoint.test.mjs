import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { seedSoftwareCore } from './core_checkpoint.mjs';
import { gpuFixture } from './gpu_fixture.mjs';

test('actual WASM: seeded warmup matches independently replayed software state', () => {
  const module = new WebAssembly.Module(fs.readFileSync(new URL('../out/n64.wasm', import.meta.url)));
  const data = gpuFixture();
  const make = () => {
    const ex = new WebAssembly.Instance(module, { env: { host_log() {}, host_gpu_flush() {} } }).exports;
    const ptr = ex.n64_reserve_rom(data.length); new Uint8Array(ex.memory.buffer, ptr, data.length).set(data);
    assert.equal(ex.n64_load(ptr, data.length), 1); return { ex, ptr };
  };
  const a = make(), independent = make(), seeded = make();
  const state = c => new Uint8Array(c.ex.memory.buffer, 0, c.ex.n64_state_size());
  for (let f = 0; f < 45; f++) { assert.equal(a.ex.n64_frame(), 0); assert.equal(independent.ex.n64_frame(), 0); }
  seedSoftwareCore(a, seeded, data.length);
  assert.deepEqual(state(seeded), state(independent));
  for (let f = 0; f < 30; f++) {
    for (const c of [a, independent, seeded]) { c.ex.n64_input(0, 0x8000, f - 15, 0); assert.equal(c.ex.n64_frame(), 0); }
    assert.deepEqual(state(seeded), state(independent));
  }
  assert.throws(() => seedSoftwareCore(a, { ex: { n64_state_size: () => 1 } }, data.length), /layout differs/);
});
