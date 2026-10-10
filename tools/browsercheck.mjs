// Real browser persistence/lifecycle checks. Run in an environment supporting Chromium.
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import assert from 'node:assert/strict';
const rom = path.resolve(process.argv[2] || 'roms/Super Mario 64 (USA).z64');
const html = fs.readFileSync('out/photon64.html');
const errors = [], logs = [];
const evidence = process.env.BROWSER_EVIDENCE || 'out/browser-evidence';
fs.mkdirSync(evidence, { recursive: true });
const mobile = process.env.BROWSER_MOBILE === '1';
const report = { outcome: 'FAIL', mobile, renderer: 'software', checks: [], errors, logs };
const server = http.createServer((_, res) => { res.setHeader('Content-Type', 'text/html'); res.end(html); });
await new Promise(r => server.listen(0, '127.0.0.1', r));
let browser, context, page;
try {
  browser = await chromium.launch({ executablePath: process.env.CHROME_EXECUTABLE || undefined, args: ['--autoplay-policy=no-user-gesture-required'] });
  report.browser = browser.version();
  context = await browser.newContext({ viewport: mobile ? { width: 390, height: 844 } : { width: 1280, height: 800 }, isMobile: mobile, hasTouch: mobile });
  await context.tracing.start({ screenshots: true, snapshots: true, sources: true });
  page = await context.newPage();
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => logs.push({ level: m.type(), text: m.text() }));
  await page.goto(`http://127.0.0.1:${server.address().port}`);
  await page.waitForFunction(() => window.__photon?.ex);
  // Keep this lane focused on storage; GPU parity has its own adapter lane.
  await page.evaluate(() => window.__photon.settings.renderer = 'sw');
  await page.setInputFiles('#file', rom); await page.waitForFunction(() => !!window.__photon.rom);
  await page.waitForFunction(() => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] > 2);
  const field = () => page.evaluate(() => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0]);
  await page.keyboard.press('KeyP');
  const pausedField = await field();
  await page.waitForTimeout(250);
  assert.equal(await field(), pausedField, 'keyboard pause must stop emulation');
  await page.keyboard.press('KeyP');
  await page.waitForFunction(n => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] > n, pausedField);
  await page.keyboard.press('Escape');
  await page.locator('#m-resume').click();
  report.checks.push('keyboard pause/resume and menu resume');
  const cdp = await context.newCDPSession(page);
  await cdp.send('Profiler.enable'); await cdp.send('Profiler.start');
  const first = await field(), started = performance.now();
  await page.waitForFunction(n => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] >= n + 120, first, { timeout: 60000 });
  const elapsedMs = performance.now() - started, completed = (await field()) - first;
  const { profile } = await cdp.send('Profiler.stop');
  fs.writeFileSync(path.join(evidence, 'browser.cpuprofile'), JSON.stringify(profile));
  report.pacedRun = { fields: completed, elapsedMs, fieldsPerSecond: completed * 1000 / elapsedMs, note: 'Browser-paced smoke measurement with tracing/profiling overhead; not peak throughput.' };
  report.checks.push('120 additional emulated fields');
  assert.deepEqual(await page.evaluate(() => Promise.all([window.__photon.saveState(0, ''), window.__photon.saveState(1, '')])), [true, true]);
  assert.equal(await page.evaluate(() => window.__photon.loadState(0)), true);
  assert.equal(await page.evaluate(async () => {
    const p = window.__photon; p.setPaused(true);
    new Uint8Array(p.ex.memory.buffer)[p.hi[12]] = 0x5A;
    new Uint32Array(p.ex.memory.buffer)[p.hi[16] >> 2] = 1;
    return await p.flushSaves();
  }), true);
  await page.reload(); await page.waitForFunction(() => window.__photon?.ex);
  await page.waitForSelector('#lib .cart');
  await page.evaluate(() => window.__photon.settings.renderer = 'sw');
  await page.locator('#lib .cart').first().click(); await page.waitForFunction(() => !!window.__photon.rom);
  assert.equal(await page.evaluate(() => new Uint8Array(window.__photon.ex.memory.buffer)[window.__photon.hi[12]]), 0x5A);
  assert.equal(await page.evaluate(() => window.__photon.loadState(1)), true);
  report.checks.push('duplicate state saves', 'state restoration', 'durable battery reload', 'cached cartridge loading');
  assert.deepEqual(errors, []);
  report.outcome = 'PASS';
  console.log('PASS browser:', JSON.stringify(report));
} catch (error) { report.error = error.stack; throw error; }
finally {
  if (page) await page.screenshot({ path: path.join(evidence, 'final.png') }).catch(e => logs.push({ level: 'capture-error', text: e.message }));
  if (context) await context.tracing.stop({ path: path.join(evidence, 'trace.zip') }).catch(e => logs.push({ level: 'trace-error', text: e.message }));
  fs.writeFileSync(path.join(evidence, 'result.json'), JSON.stringify(report, null, 2) + '\n');
  if (browser) await browser.close(); server.close();
}
