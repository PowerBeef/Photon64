// Actual built app + shipped homebrew. Use an independent browser runner (see AGENTS.md).
import { chromium, webkit } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import assert from 'node:assert/strict';
import { auditUi } from './ui-audit.mjs';
const engine = process.env.BROWSER_ENGINE || 'chromium', channel = process.env.BROWSER_CHANNEL || '';
assert.ok(['chromium', 'webkit'].includes(engine));
const evidence = process.env.UI_EVIDENCE || 'out/ui-evidence'; fs.mkdirSync(evidence, { recursive: true });
const rom = fs.readFileSync(process.argv[2] || 'testroms/RSPCP2VRCP.N64');
const html = fs.readFileSync('out/photon64.html');
const server = http.createServer((_, res) => { res.setHeader('Content-Type', 'text/html'); res.end(html); });
await new Promise(r => server.listen(0, '127.0.0.1', r));
const cases = [
  ['small-phone', 320, 568, 2, true], ['narrow-fold', 280, 653, 3, true],
  ['notched-phone', 390, 844, 3, true, [44, 0, 34, 0]],
  ['phone-landscape', 844, 390, 3, true, [0, 44, 21, 44]],
  ['short-landscape', 568, 320, 2, true], ['short-window', 360, 240, 1, false],
  ['square', 500, 500, 2, true], ['tablet', 768, 1024, 2, true, [24, 0, 20, 0]],
  ['tablet-landscape', 1024, 768, 2, true], ['tablet-split', 320, 1024, 2, true],
  ['desktop', 1280, 720, 1, false], ['desktop-hidpi', 1440, 900, 2, false],
  ['desktop-200pc-equivalent', 640, 360, 2, false],
  ['ultrawide', 3440, 1440, 2, false], ['4k-hidpi', 3840, 2160, 2, false]
];
const report = { outcome: 'FAIL', engine, channel, renderer: 'software', cases: [], errors: [], limitations: 'Emulated viewport/DPR/touch and injected safe-area values; not physical phones, browser toolbar gestures, thermal/FPS or mobile GPU coverage.' };
let browser;
try {
  browser = await (engine === 'webkit' ? webkit : chromium).launch(engine === 'webkit' ? {} : { channel: channel || undefined, args: ['--autoplay-policy=no-user-gesture-required'] });
  report.browser = browser.version();
  for (const [name, width, height, deviceScaleFactor, mobile, insets = [0, 0, 0, 0]] of cases) {
    const row = { name, requestedViewport: [width, height], deviceScaleFactor, mobile, insets, phases: [] }; report.cases.push(row);
    const context = await browser.newContext({ viewport: { width, height }, deviceScaleFactor, isMobile: mobile, hasTouch: mobile, reducedMotion: 'reduce' });
    await context.addInitScript(() => {
      // Install before initUI captures the browser method; activate only for the
      // denied-request check, leaving normal feature detection intact.
      for (const key of ['requestFullscreen', 'webkitRequestFullscreen']) {
        const proto = typeof HTMLElement.prototype[key] === 'function' ? HTMLElement.prototype : Element.prototype, original = proto[key];
        if (typeof original === 'function') proto[key] = function (...args) { if (window.__denyFullscreen) return Promise.reject(new Error('Injected fullscreen denial')); return original.apply(this, args); };
      }
    });
    const page = await context.newPage();
    const field = () => page.evaluate(() => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0]);
    page.on('pageerror', e => report.errors.push(name + ': ' + e.message));
    const check = async phase => {
      const result = await page.evaluate(auditUi); row.phases.push({ phase, ...result });
      assert.deepEqual(result.errors, [], name + '/' + phase + ': ' + result.errors.join('; '));
    };
    const click = async selector => { const l = page.locator(selector); await l.scrollIntoViewIfNeeded(); if (mobile) await l.tap(); else await l.click(); };
    const shot = async phase => { await page.mouse.move(-1, -1); return page.screenshot({ path: path.join(evidence, name + '-' + phase + '.png'), scale: 'css' }); };
    try {
      await page.goto(`http://127.0.0.1:${server.address().port}`);
      await page.waitForFunction(() => window.__photon?.ex);
      await page.addStyleTag({ content: `:root{--st:${insets[0]}px;--sr:${insets[1]}px;--sb:${insets[2]}px;--sl:${insets[3]}px}` });
      await page.evaluate(v => { window.__photon.setInsets(v); window.__photon.settings.renderer = 'sw'; }, insets);
      await check('empty-library'); await shot('home');
      await click('#h-set'); await check('home-settings');
      // Tab, Shift+Tab, radio arrows and settings tab arrows use browser keyboard events.
      const first = page.locator('#pg-settings [data-nav=back]'); await first.focus();
      await page.keyboard.press('Shift+Tab');
      assert.ok(await page.evaluate(() => document.getElementById('sheet').contains(document.activeElement)));
      await page.keyboard.press('Tab'); assert.ok(await first.evaluate(e => e === document.activeElement));
      await page.locator('#tab-video').focus(); await page.keyboard.press('ArrowRight');
      assert.equal(await page.locator('#tab-console').getAttribute('aria-selected'), 'true');
      await page.locator('#s-pak + .seg [aria-checked=true]').focus(); await page.keyboard.press('ArrowRight');
      assert.equal(await page.locator('#s-pak').inputValue(), '2');
      await page.keyboard.press('Escape');
      assert.ok(await page.locator('#h-set').evaluate(e => e === document.activeElement));
      await page.setInputFiles('#file', { name: 'A very long homebrew cartridge filename to exercise title truncation.N64', mimeType: 'application/octet-stream', buffer: rom });
      await page.waitForFunction(() => !!window.__photon.rom);
      await page.waitForFunction(() => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] > 2);
      if (mobile) {
        const a = await page.locator('#touch .t[data-b=A]').boundingBox();
        await page.evaluate(({ x, y }) => document.getElementById('stage').dispatchEvent(new PointerEvent('pointerdown', { pointerId: 77, pointerType: 'touch', clientX: x, clientY: y, bubbles: true, cancelable: true })), { x: a.x + a.width / 2, y: a.y + a.height / 2 });
        assert.ok(await page.evaluate(() => window.__photon.touchState.buttons & 0x8000));
      }
      await click('#b-menu'); await check('game-menu'); await shot('menu');
      assert.deepEqual(await page.evaluate(() => ({ ...window.__photon.touchState })), { buttons: 0, x: 0, y: 0 }, 'Menu must release held input');
      const fields = await field();
      await page.waitForTimeout(100); assert.equal(await field(), fields, 'Menu must pause emulation');
      await click('#m-save'); await check('save-states'); await shot('states');
      await page.keyboard.press('Escape');
      await click('#m-load'); await check('load-states'); await page.keyboard.press('Escape');
      await click('#m-set');
      for (const tab of ['video', 'console', 'pad', 'data']) {
        await click('#tab-' + tab); await check('settings-' + tab); await shot(tab);
      }
      await click('#tab-video'); await click('#s-aspect + .seg button:nth-child(2)'); await check('stretch-picture');
      await click('#s-aspect + .seg button:nth-child(1)'); await check('letterboxed-picture');
      if (mobile || name === 'short-window') {
        await click('#tab-pad'); await click('#s-touch + .seg button:nth-child(2)');
        for (const [sizeKey, heightKey, dpad] of [['Home', 'Home', false], ['End', 'Home', true], ['End', 'End', false]]) {
          await page.locator('#s-tsize').focus(); await page.keyboard.press(sizeKey);
          await page.locator('#s-tlift').focus(); await page.keyboard.press(heightKey);
          if (await page.locator('#s-dpad').isChecked() !== dpad) await click('#s-dpad');
          await check('pad-' + sizeKey + '-' + heightKey + '-dpad-' + dpad);
        }
      }
      // Rotate/resize with the sheet open, then return through the navigation stack.
      await page.setViewportSize({ width: height, height: width });
      await page.waitForTimeout(400); await check('rotated-settings');
      await page.setViewportSize({ width, height }); await page.waitForTimeout(400);
      await page.keyboard.press('Escape'); assert.ok(await page.locator('#m-set').evaluate(e => e === document.activeElement));
      // A denied fullscreen request keeps recovery controls available.
      if (await page.locator('#m-full').isVisible()) {
        await page.evaluate(() => window.__denyFullscreen = true);
        await click('#m-full'); await page.waitForFunction(() => document.getElementById('toast').textContent.includes('Full screen is unavailable'));
        assert.equal(await page.locator('#sheet').isVisible(), true);
      }
      await click('#m-resume'); await page.waitForFunction(n => new Uint32Array(window.__photon.ex.memory.buffer, window.__photon.hi[21], 8)[0] > n, fields);
      // Audit stage canvas sizing too; auditUi uses the hidden home only for its control list.
      await check('resumed-stage');
      if (mobile) {
        const a = await page.locator('#touch .t[data-b=A]').boundingBox();
        await page.touchscreen.tap(a.x + a.width / 2, a.y + a.height / 2);
        assert.deepEqual(await page.evaluate(() => ({ ...window.__photon.touchState })), { buttons: 0, x: 0, y: 0 });
      }
      await click('#b-menu'); await click('#m-home'); await page.waitForSelector('#lib .cart:not(.add)');
      await check('populated-library');
      console.log('PASS UI', name, row.phases.length, 'phases');
    } catch (e) { await shot('failure').catch(() => {}); row.error = e.stack; throw e; }
    finally { await context.close(); }
  }
  assert.deepEqual(report.errors, []); report.outcome = 'PASS';
  console.log('PASS responsive:', report.cases.length, 'viewports', engine, channel, report.browser);
} catch (e) { report.error = e.stack; throw e; }
finally { fs.writeFileSync(path.join(evidence, 'result.json'), JSON.stringify(report, null, 2) + '\n'); if (browser) await browser.close(); server.close(); }
