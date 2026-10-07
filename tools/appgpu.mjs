// App end-to-end with the WebGPU renderer forced onto SwiftShader (slow, but exercises the real page code).
import fs from 'fs'; import path from 'path'; import zlib from 'zlib';
import { chromium } from 'playwright';
const [rom, prefix, scale] = process.argv.slice(2);
const browser = await chromium.launch({ args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist',
  '--disable-vulkan-surface', '--enable-unsafe-swiftshader', '--disable-gpu-watchdog', '--autoplay-policy=no-user-gesture-required'] });
// VIEWPORT=WxH runs it as a touch device of that size
const vp = (process.env.VIEWPORT || '').split('x').map(Number);
const page = await (await browser.newContext(vp[1] ? { viewport: { width: vp[0], height: vp[1] }, deviceScaleFactor: 2, hasTouch: true, isMobile: true } : { viewport: { width: 1280, height: 800 } })).newPage();
page.on('console', m => console.log('[page]', m.text().slice(0, 300)));
page.on('pageerror', e => console.log('[pageerror]', e.message));
await page.goto('file://' + path.resolve('out/photon64.html') + '?gpu=force');
await page.setInputFiles('#file', path.resolve(rom));
const stat = () => page.evaluate(() => { const p = window.__photon, g = p.gpu; return { fps: +p.perf.fps.toFixed(1), ms: +p.perf.ms.toFixed(2), useGpu: p.useGpu, stats: g && g.stats, pending: g && g.pending, done: g && g.done,
  scale: g && g.scaleLog2, busy: !!p.busy, hud: document.querySelector("#hud").textContent, max: g && g.maxScaleLog2, toast: document.querySelector('#toast') && document.querySelector('#toast').textContent, pill: document.querySelector('#gpuinfo').textContent,
  frames: new Uint32Array(p.ex.memory.buffer, p.hi[21], 8)[0], core: Array.from(new Uint32Array(p.ex.memory.buffer, p.hi[21], 8)).map((v, i) => i === 6 ? v.toString(16) : v), dbg: Array.from(new Uint32Array(p.ex.memory.buffer, p.ex.n64_debug_state(), 20)).map(v => v.toString(16)).join(' ') }; });
const t0 = Date.now();
// optional: tap a key (default Enter = START) every 2.5 s while the shaders compile, to get past title screens
// (MASHSEQ = comma list of key codes, one every 2.5 s; "-" waits)
for (const k of (process.env.MASHSEQ || '').split(',').filter(Boolean)) { await page.waitForTimeout(2380); if (k !== '-') { await page.keyboard.down(k); await page.waitForTimeout(120); await page.keyboard.up(k); } else await page.waitForTimeout(120); }
try { await page.waitForFunction(() => window.__photon && window.__photon.useGpu, null, { timeout: 240000 }); } catch (e) { console.log('gpu not active'); }
console.log('gpu active after', Date.now() - t0, 'ms', JSON.stringify(await stat()));
for (let i = 0; i < 3; i++) { await page.waitForTimeout(8000); console.log(JSON.stringify(await stat())); }
function png(w, h, rgba) {
  const crcT = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = b => { let c = ~0; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return ~c >>> 0; };
  const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4); d.copy(b, 8); b.writeUInt32BE(crc(b.subarray(4, 8 + d.length)), 8 + d.length); return b; };
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) rgba.copy(raw, y * (w * 4 + 1) + 1, y * w * 4, (y + 1) * w * 4);
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const grab = async name => {
  const r = await page.evaluate(async () => { const o = await window.__photon.gpu.readOutput(); let s = ''; for (let i = 0; i < o.data.length; i += 0x8000) s += String.fromCharCode.apply(null, o.data.subarray(i, i + 0x8000)); return { w: o.width, h: o.height, d: btoa(s) }; });
  fs.writeFileSync(`${prefix}_${name}.png`, png(r.w, r.h, Buffer.from(r.d, 'base64')));
};
if ((await stat()).useGpu) await grab('a');
console.log('layout', JSON.stringify(await page.evaluate(() => { const c = document.querySelector('#cv-gpu'), r = c.getBoundingClientRect(); return { pad: window.__photon.pad, canvas: [c.width, c.height], css: [r.left, r.top, r.width, r.height], hidden: c.hidden }; })));
if (scale) {
  await page.evaluate(v => { const s = document.querySelector('#s-scale'); s.value = v; s.dispatchEvent(new Event('change')); }, scale);
  for (let i = 0; i < 8; i++) { await page.waitForTimeout(8000); const s = await stat(); console.log('after scale', JSON.stringify(s)); if (!s.useGpu || s.scale) break; }
}
await browser.close();
