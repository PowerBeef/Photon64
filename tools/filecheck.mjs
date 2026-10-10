// Direct standalone-file smoke; browser-backed uploads are distinct from a native chooser.
import { chromium, webkit } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import assert from 'node:assert/strict';
const engine = process.env.BROWSER_ENGINE || 'chromium', channel = process.env.BROWSER_CHANNEL || '';
const directory = 'out/file-evidence'; fs.mkdirSync(directory, { recursive: true });
const report = { outcome: 'FAIL', engine, channel, origin: 'file:', checks: [], limitations: 'Automated desktop browser. Native mobile chooser, physical GPU and headset quality are not covered.' };
let browser;
try {
  browser = await (engine === 'webkit' ? webkit : chromium).launch(engine === 'webkit' ? {} : { channel: channel || undefined }); report.version = browser.version();
  const page = await browser.newPage(); const errors = []; page.on('pageerror', e => errors.push(e.message));
  await page.goto(pathToFileURL(path.resolve('out/photon64.html')).href); await page.waitForFunction(() => !!window.__photon?.ex);
  await page.evaluate(() => window.__photon.settings.renderer = 'sw');
  await page.setInputFiles('#file', 'testroms/RSPCP2VRCP.N64'); await page.waitForFunction(() => !!window.__photon.rom);
  await page.waitForFunction(() => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] > 2);
  await page.locator('#b-menu').click();
  const saved = await page.evaluate(async () => { const p = window.__photon; new Uint8Array(p.ex.memory.buffer)[p.hi[12]] = 90; new Uint32Array(p.ex.memory.buffer)[p.hi[16] >> 2] = 1; await p.flushSaves(); return p.diagnostics(); });
  report.diagnostics = saved; report.checks.push('standalone core runs without HTTP or external resources', 'production File import', 'pause and battery write');
  await page.reload(); await page.waitForFunction(() => !!window.__photon?.ex);
  // File-origin storage is implementation-defined; report temporary policy explicitly.
  const cached = await page.locator('#lib .cart-play').count();
  if (cached) { await page.evaluate(() => window.__photon.settings.renderer = 'sw'); await page.locator('#lib .cart-play').first().click(); await page.waitForFunction(() => !!window.__photon.rom);
    if (saved.storage === 'exclusive cartridge writer') assert.equal(await page.evaluate(() => new Uint8Array(window.__photon.ex.memory.buffer)[window.__photon.hi[12]]), 90);
    report.checks.push('cached cartridge reopens after reload'); }
  report.storage = cached ? 'library survived reload; battery policy recorded in diagnostics' : 'SKIP: file-origin IndexedDB unavailable; temporary mode';
  assert.deepEqual(errors, []); report.outcome = 'PASS';
} catch (e) { report.error = String(e.stack || e); process.exitCode = 1; }
finally { await browser?.close(); fs.writeFileSync(path.join(directory, 'result.json'), JSON.stringify(report, null, 2) + '\n'); console.log(JSON.stringify(report)); }
