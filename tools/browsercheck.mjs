// Real browser persistence/lifecycle checks. Run in an environment supporting the selected Playwright browser.
import { chromium, webkit } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import assert from 'node:assert/strict';
const rom = path.resolve(process.argv[2] || 'roms/Super Mario 64 (USA).z64');
const html = fs.readFileSync('out/photon64.html');
const errors = [], logs = [];
const evidence = process.env.BROWSER_EVIDENCE || 'out/browser-evidence';
fs.mkdirSync(evidence, { recursive: true });
const engine = process.env.BROWSER_ENGINE || 'chromium', channel = process.env.BROWSER_CHANNEL || '';
if (!['chromium', 'webkit'].includes(engine)) throw Error('Unsupported browser engine: ' + engine);
const mobile = process.env.BROWSER_MOBILE === '1';
const report = { outcome: 'FAIL', engine, channel, mobile, renderer: 'software', checks: [], errors, logs };
const server = http.createServer((_, res) => { res.setHeader('Content-Type', 'text/html'); res.end(html); });
await new Promise(r => server.listen(0, '127.0.0.1', r));
let browser, context, page;
try {
  browser = await (engine === 'webkit' ? webkit : chromium).launch(engine === 'webkit' ? {} : { channel: channel || undefined, executablePath: process.env.CHROME_EXECUTABLE || undefined, args: ['--autoplay-policy=no-user-gesture-required'] });
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
  const cdp = engine === 'chromium' ? await context.newCDPSession(page) : null;
  if (cdp) { await cdp.send('Profiler.enable'); await cdp.send('Profiler.start'); }
  const first = await field(), started = performance.now();
  await page.waitForFunction(n => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] >= n + 120, first, { timeout: 60000 });
  const elapsedMs = performance.now() - started, completed = (await field()) - first;
  if (cdp) {
    const { profile } = await cdp.send('Profiler.stop');
    fs.writeFileSync(path.join(evidence, 'browser.cpuprofile'), JSON.stringify(profile));
  }
  report.cpuProfile = cdp ? 'PASS: captured through CDP' : 'SKIP: CDP profiler is Chromium-only';
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
  // Real browser transaction failure, dirty retention and recovery. Override the
  // platform API only for this transaction; the production storage path runs unchanged.
  assert.equal(await page.evaluate(async () => {
    const p = window.__photon; p.setPaused(true);
    const dirty = new Uint32Array(p.ex.memory.buffer, p.hi[16], 1); dirty[0] = 17;
    const original = IDBObjectStore.prototype.put;
    IDBObjectStore.prototype.put = function () { throw new DOMException('Injected quota failure', 'QuotaExceededError'); };
    try { return (await p.flushSaves()) === false && dirty[0] === 17; }
    finally { IDBObjectStore.prototype.put = original; }
  }), true);
  assert.equal(await page.evaluate(() => window.__photon.flushSaves()), true);
  assert.equal(await page.evaluate(() => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[16], 1)[0]), 0);
  report.checks.push('quota failure retains dirty progress; retry commits');
  // Invalid candidates cannot replace the active cartridge.
  assert.equal(await page.evaluate(async () => {
    const p = window.__photon, original = p.rom;
    try { await p.loadRom(new Uint8Array(32), 'invalid.z64'); return false; }
    catch { return p.rom === original && p.running; }
  }), true);
  report.checks.push('invalid cartridge preserves active session');
  // A synchronous presentation failure used to escape the frame loop. It now
  // stops emulation, leaves export/reset available and recovers after reset.
  await page.evaluate(() => {
    const original = CanvasRenderingContext2D.prototype.putImageData;
    window.restorePresentation = () => { CanvasRenderingContext2D.prototype.putImageData = original; };
    CanvasRenderingContext2D.prototype.putImageData = function () { throw new Error('Injected presentation failure'); };
    window.__photon.setPaused(false);
  });
  await page.waitForFunction(() => window.__photon.rendererFailed && !window.__photon.running);
  const failedField = await field(); await page.waitForTimeout(200);
  assert.equal(await field(), failedField);
  assert.match(await page.locator('#toast').textContent(), /Renderer synchronization failed/);
  await page.evaluate(() => { window.restorePresentation(); return window.__photon.resetGame(); });
  await page.waitForFunction(() => !window.__photon.rendererFailed && window.__photon.running);
  report.checks.push('presentation failure stops fields; reset recovers');
  // Held keyboard and touch inputs must be released when focus is lost.
  await page.keyboard.down('KeyX');
  if (mobile) {
    const button = page.locator('#touch .t[data-b=A]');
    const box = await button.boundingBox();
    if (cdp) await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: box.x + box.width / 2, y: box.y + box.height / 2, id: 7 }] });
    else await page.evaluate(({ x, y }) => document.getElementById('stage').dispatchEvent(new PointerEvent('pointerdown', { pointerId: 7, pointerType: 'touch', clientX: x, clientY: y, bubbles: true, cancelable: true })), { x: box.x + box.width / 2, y: box.y + box.height / 2 });
    report.heldTouch = cdp ? 'CDP touch input' : 'Synthetic PointerEvent; real taps covered by responsive lane';
    assert.ok(await page.evaluate(() => window.__photon.touchState.buttons & 0x8000));
  }
  await page.evaluate(() => window.dispatchEvent(new Event('blur')));
  assert.deepEqual(await page.evaluate(() => ({ ...window.__photon.touchState })), { buttons: 0, x: 0, y: 0 });
  if (mobile && cdp) await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await page.keyboard.up('KeyX');
  report.checks.push('focus loss releases held inputs');
  const temporary = await browser.newContext();
  await temporary.addInitScript(() => { indexedDB.open = () => { throw new DOMException('Injected unavailable storage', 'SecurityError'); }; });
  const temporaryPage = await temporary.newPage();
  temporaryPage.on('pageerror', e => errors.push(e.message));
  await temporaryPage.goto(`http://127.0.0.1:${server.address().port}`);
  await temporaryPage.waitForFunction(() => window.__photon?.ex);
  await temporaryPage.evaluate(() => window.__photon.settings.renderer = 'sw');
  await temporaryPage.setInputFiles('#file', rom);
  await temporaryPage.waitForFunction(() => !!window.__photon.rom);
  assert.equal(await temporaryPage.evaluate(async () => {
    const p = window.__photon; p.setPaused(true);
    const dirty = new Uint32Array(p.ex.memory.buffer, p.hi[16], 1); dirty[0] = 19;
    return await p.flushSaves() && dirty[0] === 19 && await p.saveState(0, '');
  }), true);
  assert.match(await temporaryPage.locator('#toast').textContent(), /temporarily/i);
  await temporaryPage.screenshot({ path: path.join(evidence, 'temporary-storage.png') });
  await temporaryPage.reload(); await temporaryPage.waitForFunction(() => window.__photon?.ex);
  assert.deepEqual(await temporaryPage.evaluate(() => window.__photon.libList()), []);
  assert.equal(await temporaryPage.locator('#lib .cart:not(.add)').count(), 0);
  await temporary.close();
  report.checks.push('unavailable storage stays dirty, labels temporary states, and disappears after reload');
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
