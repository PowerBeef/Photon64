// Real browser persistence/lifecycle checks. Run in an environment supporting Chromium.
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import assert from 'node:assert/strict';
const rom = path.resolve(process.argv[2] || 'roms/Super Mario 64 (USA).z64');
const html = fs.readFileSync('out/photon64.html');
const errors = [];
const server = http.createServer((_, res) => { res.setHeader('Content-Type', 'text/html'); res.end(html); });
await new Promise(r => server.listen(0, '127.0.0.1', r));
let browser;
try {
  browser = await chromium.launch({ executablePath: process.env.CHROME_EXECUTABLE || undefined, args: ['--autoplay-policy=no-user-gesture-required'] });
  const context = await browser.newContext(); const page = await context.newPage();
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${server.address().port}`);
  await page.waitForFunction(() => window.__photon?.ex);
  // Keep this lane focused on storage; GPU parity has its own adapter lane.
  await page.evaluate(() => window.__photon.settings.renderer = 'sw');
  await page.setInputFiles('#file', rom); await page.waitForFunction(() => !!window.__photon.rom);
  await page.waitForFunction(() => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] > 2);
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
  assert.deepEqual(errors, []);
  await page.screenshot({ path: 'out/browsercheck.png' });
  console.log('PASS browser: duplicate state saves, state reload, durable battery reload, cached ROM identity, no page errors');
} finally { if (browser) await browser.close(); server.close(); }
