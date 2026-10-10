// Independent scalar-vs-optimized VI artifacts on synthetic RAM/register matrices.
// Usage: node tools/vicompare.mjs out/preopt.wasm out/n64.wasm
import fs from 'node:fs';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
const load = file => {
  const ex = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(file)), { env: { host_log() {}, host_gpu_flush() {} } }).exports;
  return { ex, hi: Array.from(new Uint32Array(ex.memory.buffer, ex.n64_host_info(), 24)) };
};
const A = load(process.argv[2]), B = load(process.argv[3]);
const data = new Uint32Array(8 * 1024 * 1024 / 4); let seed = 4300;
for (let i = 0; i < data.length; i++) { seed = Math.imul(seed, 1664525) + 1013904223 >>> 0; data[i] = seed; }
for (const C of [A, B]) new Uint32Array(C.ex.memory.buffer, C.hi[0], data.length).set(data);
let checks = 0;
for (const format of [2, 3]) for (const mode of [0, 0x100, 0x200, 0x300]) for (const effects of [0, 4, 8, 12, 16, 0x10000, 0x10010, 0x1001C]) for (const origin of [0x4000, 0x7FFE00]) {
  const control = format | mode | effects;
  const images = [];
  for (const C of [A, B]) {
    const regs = new Uint32Array(C.ex.memory.buffer, C.hi[18], 14);
    regs.fill(0); regs[0] = control; regs[1] = origin; regs[2] = 320;
    regs[6] = 525; regs[9] = (108 << 16) | 748; regs[10] = (34 << 16) | 514; regs[12] = 512; regs[13] = 1024;
    C.ex.n64_vi_render();
    const dims = C.ex.n64_vi_dims(); assert.equal(dims >>> 31, 0);
    images.push(Buffer.from(new Uint8Array(C.ex.memory.buffer, C.hi[1], 640 * 240 * 4)));
  }
  assert.deepEqual(images[0], images[1], `VI differs control=${control.toString(16)} origin=${origin.toString(16)}`);
  checks++;
}
console.log(JSON.stringify({ outcome: 'PASS', checks, reference: crypto.createHash('sha256').update(fs.readFileSync(process.argv[2])).digest('hex'), candidate: crypto.createHash('sha256').update(fs.readFileSync(process.argv[3])).digest('hex'), note: 'Same synthetic RAM and VI register matrix, including filtering, gamma, dithering and wrapped origins. Reference is the pre-optimization artifact.' }));
if (process.argv.includes('--bench')) {
  const results = [];
  for (const control of [0x301E, 0x1001E]) {
    for (const C of [A, B]) {
      const r = new Uint32Array(C.ex.memory.buffer, C.hi[18], 14); r[0] = control; r[1] = 0x4000;
      for (let i = 0; i < 32; i++) C.ex.n64_vi_render();
    }
    const times = [[], []];
    for (let repeat = 0; repeat < 7; repeat++) for (const index of repeat & 1 ? [1, 0] : [0, 1]) {
      const C = [A, B][index], start = performance.now();
      for (let i = 0; i < 32; i++) C.ex.n64_vi_render();
      times[index].push((performance.now() - start) / 32);
    }
    const median = a => [...a].sort((x, y) => x - y)[3];
    const referenceMs = median(times[0]), candidateMs = median(times[1]);
    results.push({ control: control.toString(16), referenceMs, candidateMs, speedup: referenceMs / candidateMs, samplesMs: times });
  }
  console.log(JSON.stringify({ viOnlyBenchmark: results, note: 'Interleaved seven blocks per artifact after warmup; synthetic image, no CPU/RSP/RDP gameplay or browser.' }));
}
