// End-to-end test of the shipped HTML in headless Chromium.
//   node tools/apptest.mjs rom outPrefix [--mobile] [--portrait] [--nogpu] [--steps "wait:3000,shot:a,key:Enter,..."]
import fs from 'fs'; import path from 'path';
import { chromium, devices } from 'playwright';
const args = process.argv.slice(2);
const bool = n => { const i = args.indexOf(n); if (i < 0) return false; args.splice(i, 1); return true; };
const flag = n => { const i = args.indexOf(n); if (i < 0) return null; const v = args[i + 1]; args.splice(i, 2); return v; };
const mobile = bool('--mobile'), portrait = bool('--portrait'), nogpu = bool('--nogpu');
const steps = (flag('--steps') || 'wait:4000,shot:run').split(',');
const html = flag('--html') || 'out/photon64.html';
const [rom, prefix] = args;
const gpuArgs = nogpu ? [] : ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist',
  '--disable-vulkan-surface', '--enable-unsafe-swiftshader', '--disable-gpu-watchdog'];
const browser = await chromium.launch({ args: [...gpuArgs, '--autoplay-policy=no-user-gesture-required'] });
const ctx = await browser.newContext(mobile ? { viewport: portrait ? { width: 390, height: 844 } : { width: 844, height: 390 }, deviceScaleFactor: 2, hasTouch: true, isMobile: true }
  : { viewport: { width: 1280, height: 800 } });
const page = await ctx.newPage();
page.on('console', m => console.log('[page]', m.text().slice(0, 300)));
page.on('pageerror', e => console.log('[pageerror]', e.message));
await page.goto('file://' + path.resolve(html) + (nogpu ? '' : '?gpu=force'));
await page.waitForTimeout(500);
await page.screenshot({ path: `${prefix}_landing.png` });
if (rom !== '-') await page.setInputFiles('#file', path.resolve(rom));
for (const st of steps) {
  const [cmd, arg, arg2] = st.split(':');
  if (cmd === 'wait') await page.waitForTimeout(+arg);
  else if (cmd === 'shot') await page.screenshot({ path: `${prefix}_${arg}.png`, timeout: 150000 });
  else if (cmd === 'pause') await page.evaluate(p => window.__photon.setPaused(p), arg === '1');
  else if (cmd === 'key') { await page.keyboard.down(arg); await page.waitForTimeout(+(arg2 || 120)); await page.keyboard.up(arg); }
  else if (cmd === 'down') await page.keyboard.down(arg);
  else if (cmd === 'up') await page.keyboard.up(arg);
  else if (cmd === 'click') await page.click(arg);
  else if (cmd === 'move') { await page.mouse.move(+arg || 300, +arg2 || 300); await page.mouse.move((+arg || 300) + 20, (+arg2 || 300) + 10); }
  else if (cmd === 'tap') await page.touchscreen.tap(+arg, +arg2);
  else if (cmd === 'waitgpu') { try { await page.waitForFunction(() => window.__photon && window.__photon.useGpu, null, { timeout: +arg || 180000 }); } catch (e) { console.log('gpu not active'); } }
  else if (cmd === 'eval') console.log('eval:', JSON.stringify(await page.evaluate(arg + (arg2 ? ':' + arg2 : ''))));
  else if (cmd === 'stat') console.log('stat:', JSON.stringify(await page.evaluate(() => { const p = window.__photon; return { fps: +p.perf.fps.toFixed(1), ms: +p.perf.ms.toFixed(2), gpu: p.useGpu, rom: p.rom, gstats: p.gpu && p.gpu.stats,
    frames: new Uint32Array(p.ex.memory.buffer, p.hi[21], 8)[0], prims: new Uint32Array(p.ex.memory.buffer, p.hi[21], 8)[1] }; })));
}
await browser.close();
