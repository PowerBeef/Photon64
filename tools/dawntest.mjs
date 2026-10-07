// Same tests as gputest.mjs, but on Dawn's node bindings (no browser): much faster shader compiles, and it can run
// on Mesa lavapipe (GPU_BACKEND=lvp, default) or whatever Vulkan driver is installed.
//   node tools/dawntest.mjs rom frames "check,frames" [inputs] [--images prefix] [--hd L] [--window n] [--list file] [--stripped]
import fs from 'fs'; import path from 'path'; import zlib from 'zlib'; import vm from 'vm'; import { fileURLToPath } from 'url';
if ((process.env.GPU_BACKEND || 'lvp') === 'lvp') process.env.VK_ICD_FILENAMES = process.env.VK_DRIVER_FILES = '/usr/share/vulkan/icd.d/lvp_icd.json';
const dawnBindings = process.env.DAWN_WEBGPU || '/home/claude/dawn/node_modules/webgpu/index.js';
const { create, globals } = await import(dawnBindings);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const flag = n => { const i = args.indexOf(n); if (i < 0) return null; const v = args[i + 1]; args.splice(i, 2); return v; };
const bool = n => { const i = args.indexOf(n); if (i < 0) return false; args.splice(i, 1); return true; };
const stripped = bool('--stripped'); const hdArg = flag('--hd'); const hd = hdArg === null ? null : +hdArg;
const images = flag('--images'), window_ = +(flag('--window') || 0), progress = +(flag('--progress') || 0), noref = bool('--noref'), readback = bool('--readback'), noexact = bool('--noexact');
const trace = +(flag('--trace') || 0), sharp = bool('--sharp');
const pts = flag('--pts'), region = flag('--region');
const listFile = flag('--list'), fn = flag('--fn'), hdWords = +(flag('--hdwords') || 0);
const [romPath, frames, checks, inputs] = args;
function png(w, h, rgba) {
  const crcT = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = b => { let c = ~0; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return ~c >>> 0; };
  const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4); d.copy(b, 8); b.writeUInt32BE(crc(b.subarray(4, 8 + d.length)), 8 + d.length); return b; };
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const gpu = create([]);
const file = u => { u = decodeURIComponent(u.split('?')[0]); return u === '/__rom' ? path.resolve(romPath) : u.startsWith('/__abs/') ? u.slice(6) : path.join(root, u); };
const ctx = { ...globals, console, performance, WebAssembly, Uint8Array, Uint16Array, Uint32Array, Int32Array, Float32Array, DataView, ArrayBuffer, Map, Set, Promise, Math, JSON, String, Array, Object, Error,
  navigator: { gpu }, window: {}, document: { getElementById: () => null }, btoa: s => Buffer.from(s, 'binary').toString('base64'), decodeURIComponent,
  fetch: async u => { const d = fs.readFileSync(file(u)); return { arrayBuffer: async () => d.buffer.slice(d.byteOffset, d.byteOffset + d.length), text: async () => d.toString('utf8'), json: async () => JSON.parse(d.toString('utf8')) }; } };
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(root, 'src/web/gpu.js'), 'utf8') + '\nthis.N64Gpu = N64Gpu;', ctx, { filename: 'gpu.js' });
const html = fs.readFileSync(path.join(root, 'tools/gputest.html'), 'utf8');
vm.runInContext(html.slice(html.lastIndexOf('<script>') + 8, html.lastIndexOf('</script>')), ctx, { filename: 'gputest.html.js' });
if (fn) { vm.runInContext(fs.readFileSync(path.resolve(fn), 'utf8'), ctx, { filename: fn }); }
const cfg = { rom: '/__rom', frames: +frames, checks: (checks || '').split(',').filter(Boolean).map(Number), inputs: inputs || '', images: !!images, noref, noexact, window: window_, progress, stripped, hd, hdWords, trace, sharp, pts: pts ? JSON.parse(pts) : null, region: region ? JSON.parse(region) : null };
if (listFile) {
  const roms = fs.readFileSync(listFile, 'utf8').split('\n').filter(Boolean).map(f => '/__abs' + f.split('/').map(encodeURIComponent).join('/'));
  const r = await ctx.window.runMulti({ roms, frames: +frames, noexact, images: !!images, hd });
  let bad = 0;
  for (const x of r.results) {
    if (x.image && images) { const d = Buffer.from(x.image.data, 'base64'); fs.writeFileSync(`${images}_${x.rom.replace(/\W+/g, '_')}.png`, png(x.image.w, x.image.h, d)); }
    if (x.error || x.vi || (x.fb && x.fb.length) || !x.sync) bad++;
  }
  console.log(`ROMs: ${r.results.length}, with differences: ${bad}`, JSON.stringify(r.stats));
  process.exit(0);
}
let res;
if (process.env.XPAK) ctx.window.XPAK_MODE = +process.env.XPAK;     // XPAK=2: run the game without the Expansion Pak
try { res = await (fn ? ctx.window.runCustom(cfg) : ctx.window.runTest(cfg)); } catch (e) { console.log('ERROR', e.stack || e.message); process.exit(1); }
if (res.images) for (const im of res.images) {
  const d = Buffer.from(im.data, 'base64');
  fs.writeFileSync(`${images}_${String(im.frame).padStart(5, '0')}.png`, png(im.w, im.h, d));
}
delete res.images;
console.log(JSON.stringify(res, null, 1));
process.exit(0);
