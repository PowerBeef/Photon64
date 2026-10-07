// Headless Chromium (SwiftShader WebGPU) harness: GPU renderer vs. the software reference, pixel for pixel.
//   node tools/gputest.mjs rom frames "check,frames" [inputs] [--images prefix] [--noref] [--readback]
import http from 'http'; import fs from 'fs'; import path from 'path'; import zlib from 'zlib'; import { fileURLToPath } from 'url';
import { chromium } from 'playwright';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const flag = n => { const i = args.indexOf(n); if (i < 0) return null; const v = args[i + 1]; args.splice(i, 2); return v; };
const bool = n => { const i = args.indexOf(n); if (i < 0) return false; args.splice(i, 1); return true; };
const stripped = bool('--stripped'); const hdArg = flag('--hd'); const hd = hdArg === null ? null : +hdArg; const images = flag('--images'), window_ = +(flag('--window') || 0), progress = +(flag('--progress') || 0), noref = bool('--noref'), readback = bool('--readback'), noexact = bool('--noexact'), page_ = flag('--page') || '/tools/gputest.html';
const listFile = flag('--list');
const [romPath, frames, checks, inputs] = args;
const romAbs = path.resolve(romPath);
const server = http.createServer((req, res) => {
  const u = decodeURIComponent(req.url.split('?')[0]);
  const f = u === '/__rom' ? romAbs : u.startsWith('/__abs/') ? u.slice(6) : path.join(root, u);
  fs.readFile(f, (e, d) => {
    if (e) { res.writeHead(404); res.end(); return; }
    const ext = path.extname(f);
    res.writeHead(200, { 'content-type': ext === '.html' ? 'text/html' : ext === '.js' ? 'text/javascript' : ext === '.wasm' ? 'application/wasm' : 'application/octet-stream' });
    res.end(d);
  });
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const port = server.address().port;
function png(w, h, rgba) {
  const crcT = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = b => { let c = ~0; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return ~c >>> 0; };
  const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4); d.copy(b, 8); b.writeUInt32BE(crc(b.subarray(4, 8 + d.length)), 8 + d.length); return b; };
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const lvp = process.env.GPU_BACKEND === 'lvp';   // Mesa lavapipe instead of SwiftShader
const browser = await chromium.launch({ env: lvp ? { ...process.env, VK_ICD_FILENAMES: '/usr/share/vulkan/icd.d/lvp_icd.json', VK_DRIVER_FILES: '/usr/share/vulkan/icd.d/lvp_icd.json' } : process.env,
  args: [...(lvp ? ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=native', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--disable-vulkan-surface', '--disable-gpu-sandbox', '--no-sandbox', '--disable-gpu-watchdog']
    : ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--disable-vulkan-surface', '--enable-unsafe-swiftshader', '--disable-gpu-watchdog']),
    ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] });
const page = await browser.newPage();
page.on('console', m => console.log('[page]', m.text()));
page.on('pageerror', e => console.log('[pageerror]', e.message));
await page.goto(`http://127.0.0.1:${port}${page_}`);
const cfg = { rom: '/__rom', frames: +frames, checks: (checks || '').split(',').filter(Boolean).map(Number), inputs: inputs || '', images: !!images, noref, noexact, window: window_, progress, stripped, hd };
let res;
if (listFile) {
  const roms = fs.readFileSync(listFile, 'utf8').split('\n').filter(Boolean).map(f => '/__abs' + f.split('/').map(encodeURIComponent).join('/'));
  const r = await page.evaluate(c => window.runMulti(c), { roms, frames: +frames, noexact, images: !!images, hd });
  let bad = 0;
  for (const x of r.results) {
    if (x.image && images) { const d = Buffer.from(x.image.data, 'base64'); fs.writeFileSync(`${images}_${x.rom.replace(/\W+/g, '_')}.png`, png(x.image.w, x.image.h, d)); }
    if (x.error || x.vi || (x.fb && x.fb.length) || !x.sync) bad++;
  }
  console.log(`ROMs: ${r.results.length}, with differences: ${bad}`, JSON.stringify(r.stats));
  await browser.close(); server.close(); process.exit(0);
}
try { res = await page.evaluate(c => window.runTest(c), cfg); } catch (e) { console.log('ERROR', e.message); await browser.close(); server.close(); process.exit(1); }
if (res.images) for (const im of res.images) {
  const d = Buffer.from(im.data, 'base64');
  let out = d, h = im.h;
  if (h <= 288) { out = Buffer.alloc(d.length * 2); for (let y = 0; y < h * 2; y++) d.copy(out, y * im.w * 4, (y >> 1) * im.w * 4, ((y >> 1) + 1) * im.w * 4); h *= 2; }
  fs.writeFileSync(`${images}_${String(im.frame).padStart(5, '0')}.png`, png(im.w, h, out));
}
delete res.images;
console.log(JSON.stringify(res, null, 1));
await browser.close(); server.close();
