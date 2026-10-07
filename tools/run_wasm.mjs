// Node harness for the WASM core: perf + parity checks.
import fs from 'fs';
const wasm = fs.readFileSync(new URL('../out/n64.wasm', import.meta.url));
const mod = new WebAssembly.Module(wasm);
console.log('imports:', WebAssembly.Module.imports(mod).map(i => i.module + '.' + i.name).join(', '));
let mem;
const inst = new WebAssembly.Instance(mod, { env: {
  host_log: (p, a, b) => { const u8 = new Uint8Array(mem.buffer, p, b === 1 ? a : 64); let s = ''; for (const c of u8) { if (!c && b !== 1) break; s += String.fromCharCode(c); } process.stdout.write(b === 1 ? s : `[log] ${s} ${a} ${b}\n`); },
  host_gpu_flush: () => {},
} });
const ex = inst.exports; mem = ex.memory;
const rom = fs.readFileSync(process.argv[2]);
const frames = +(process.argv[3] || 300);
const p = ex.n64_alloc(rom.length);
new Uint8Array(mem.buffer, p, rom.length).set(rom);
if (!ex.n64_load(p, rom.length)) throw new Error("bad rom");
if (process.env.GPU) { ex.n64_config(0, 1); ex.n64_config(8, 0); }   // (no real GPU here: never wait for copy-backs)
if (process.env.CPI) ex.n64_config(1, +process.env.CPI);
const hi = new Uint32Array(mem.buffer, ex.n64_host_info(), 32);
const stats = () => new Uint32Array(mem.buffer, hi[21], 16);
const inputs = (process.argv[4] || '').split(',').filter(Boolean).map(s => { const [f, b, d] = s.split(':'); return { f: +f, b, d: +(d || 1) }; });
const BT = { A: 0x8000, B: 0x4000, Z: 0x2000, START: 0x1000 };
let t0 = performance.now(), tmax = 0, slow = 0;
const times = [];
for (let i = 0; i < frames; i++) {
  let b = 0, sx = 0, sy = 0;
  for (const e of inputs) if (i >= e.f && i < e.f + e.d) for (const k of e.b.split('+')) { if (k.startsWith('X=')) sx = +k.slice(2); else if (k.startsWith('Y=')) sy = +k.slice(2); else b |= BT[k] || 0; }
  ex.n64_input(0, b, sx, sy);
  const a = performance.now();
  while (ex.n64_frame()) ex.n64_sync_done();
  const d = performance.now() - a; times.push(d);
}
const dt = performance.now() - t0;
times.sort((a, b) => a - b);
const st = stats();
console.log(`frames=${frames} total=${dt.toFixed(0)}ms avg=${(dt / frames).toFixed(2)}ms median=${times[frames >> 1].toFixed(2)}ms p99=${times[Math.floor(frames * 0.99)].toFixed(2)}ms max=${times[frames - 1].toFixed(2)}ms  fps=${(1000 * frames / dt).toFixed(1)}`);
console.log(`prims=${st[1]} flushes=${st[2]} rsp=${st[3]} idle=${(100 * st[4] / st[5]).toFixed(1)}% pc=${st[6].toString(16)} mem=${(mem.buffer.byteLength / 1048576).toFixed(1)}MB`);
{ const u = new Uint32Array(mem.buffer, hi[0], 0x200000); let h = 2166136261; for (let k = 0; k < u.length; k++) h = Math.imul(h ^ u[k], 16777619);
  const a = new Uint16Array(mem.buffer, hi[3], 65536); let ha = 2166136261; for (let k = 0; k < 65536; k++) ha = Math.imul(ha ^ a[k], 16777619);
  console.log(`hash rdram=${(h >>> 0).toString(16).padStart(8, '0')} audio=${(ha >>> 0).toString(16).padStart(8, '0')}`); }
