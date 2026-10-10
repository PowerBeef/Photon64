// Interactive core debugging without a browser. One JSON request per stdin line.
// node tools/coreplay.mjs ROM [output-directory]
// {"op":"step","fields":60,"input":"A+X=80"} | {"op":"inspect"} | {"op":"snapshot"} | {"op":"quit"}
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';
import zlib from 'node:zlib';
import { parseInputs } from './benchmark.mjs';

const romPath = process.argv[2], directory = path.resolve(process.argv[3] || 'out/coreplay');
if (!romPath) throw new Error('Usage: node tools/coreplay.mjs ROM [output-directory]');
fs.mkdirSync(directory, { recursive: true });
const ex = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync('out/n64.wasm')), { env: {
  host_log() {}, host_gpu_flush() { throw new Error('Unexpected GPU flush in software mode'); }
} }).exports;
const rom = fs.readFileSync(romPath), ptr = ex.n64_alloc(rom.length);
if (!ptr) throw new Error('ROM allocation failed');
new Uint8Array(ex.memory.buffer, ptr, rom.length).set(rom);
if (!ex.n64_load(ptr, rom.length)) throw new Error('Invalid ROM');
const hi = Array.from(new Uint32Array(ex.memory.buffer, ex.n64_host_info(), 24));
const inspect = () => {
  const stats = Array.from(new Uint32Array(ex.memory.buffer, hi[21], 8));
  const debug = Array.from(new Uint32Array(ex.memory.buffer, ex.n64_debug_state(), 20));
  return { fields: stats[0], primitives: stats[1], pc: stats[6].toString(16), debugWords: debug, memoryMiB: ex.memory.buffer.byteLength / 1048576 };
};
const crcTable = new Uint32Array(256).map((_, n) => {
  let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0;
});
function chunk(type, data) {
  const out = Buffer.alloc(data.length + 12); out.writeUInt32BE(data.length); out.write(type, 4); data.copy(out, 8);
  let crc = ~0; for (const b of out.subarray(4, -4)) crc = crcTable[(crc ^ b) & 255] ^ (crc >>> 8);
  out.writeUInt32BE(~crc >>> 0, out.length - 4); return out;
}
function snapshot() {
  // Rendering occurs once per stepped field; snapshot reads the current VI without advancing its noise sequence.
  const dims = ex.n64_vi_dims(), w = dims & 65535, h = (dims >>> 16) & 32767;
  if (!w || !h || dims >>> 31) throw new Error('No valid VI image yet');
  const rgba = new Uint8Array(ex.memory.buffer, hi[1], w * h * 4), raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) raw.set(rgba.subarray(y * w * 4, (y + 1) * w * 4), y * (w * 4 + 1) + 1);
  const hdr = Buffer.alloc(13); hdr.writeUInt32BE(w); hdr.writeUInt32BE(h, 4); hdr[8] = 8; hdr[9] = 6;
  const file = path.join(directory, `field-${inspect().fields}.png`);
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', hdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
  return { file, width: w, height: h };
}
console.log(JSON.stringify({ ready: true, rom: path.basename(romPath), commands: ['step', 'replay', 'inspect', 'snapshot', 'quit'] }));
for await (const line of readline.createInterface({ input: process.stdin })) {
  try {
    const request = JSON.parse(line);
    if (request.op === 'quit') break;
    let result;
    if (request.op === 'step' || request.op === 'replay') {
      if (!Number.isSafeInteger(request.fields) || request.fields < 1 || request.fields > 10000) throw new Error('fields must be 1..10000');
      const start = inspect().fields;
      const events = request.op === 'replay' ? parseInputs(fs.readFileSync(request.inputs, 'utf8')) : parseInputs(request.input ? `${start}:${request.input}:${request.fields}` : '');
      for (let f = start; f < start + request.fields; f++) {
        let b = 0, x = 0, y = 0;
        for (const e of events) if (f >= e.frame && f < e.frame + e.duration) { b |= e.buttons; x = e.x ?? x; y = e.y ?? y; }
        ex.n64_input(0, b, x, y);
        if (ex.n64_frame()) throw new Error('Unexpected GPU synchronization');
        ex.n64_vi_render();
      }
      ex.n64_input(0, 0, 0, 0);
      result = inspect();
    } else if (request.op === 'inspect') result = inspect();
    else if (request.op === 'snapshot') result = snapshot();
    else throw new Error('Unknown operation');
    console.log(JSON.stringify({ ok: true, ...result }));
  } catch (error) { console.log(JSON.stringify({ ok: false, error: error.message })); }
}
