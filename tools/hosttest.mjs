// A host that shows the page through a window smaller than the page (see hostFit in app.js), imitated by making
// window.scrollY report what a page in such a web view sees. Both ways a browser might count are covered: resting below
// zero by the top inset, or counting from zero.
import path from 'path';
import { chromium } from 'playwright';
const [rom, shots] = process.argv.slice(2);
const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
let bad = 0; const check = (n, ok, x = '') => { console.log(ok ? 'ok  ' : 'FAIL', n, x); if (!ok) bad++; };
const W = 402, H = 874, T = 62, B = 87;
async function open(first) {
  const page = await (await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: 2, hasTouch: true, isMobile: true })).newPage();
  page.on('pageerror', e => { console.log('[pageerror]', e.message); bad++; });
  await page.addInitScript(v => { window.__sy = v; Object.defineProperty(window, 'scrollY', { get: () => window.__sy, configurable: true }); }, first);
  await page.goto('file://' + path.resolve('out/photon64.html') + `?insets=${T},0,${B},0`);
  await page.waitForTimeout(1200);
  return page;
}
const at = async (page, y) => { await page.evaluate(v => { window.__sy = v; window.dispatchEvent(new Event('scroll')); }, y); await page.waitForTimeout(900); };
const m = page => page.evaluate(() => { const p = window.__photon, cs = getComputedStyle(document.documentElement), box = e => { const r = e.getBoundingClientRect(); return [r.left, r.top, r.right, r.bottom].map(Math.round); };
  return { on: p.hostFit.active, s: p.hostFit.s, R: p.hostFit.R, st: cs.getPropertyValue('--st').trim(), sb: cs.getPropertyValue('--sb').trim(), logo: box(document.getElementById('logo')), fine: box(document.querySelector('.fine')),
    pad: p.pad && p.pad.insets, els: [...document.querySelectorAll('#touch .t, #b-menu')].filter(e => e.offsetParent).map(box), panel: document.getElementById('sheet').hidden ? null : box(document.querySelector('#sheet .panel > :last-child, #sheet .panel')) }; });
const drag = (page, sel) => page.evaluate(sel => { const el = document.querySelector(sel), t = new Touch({ identifier: 1, target: el, clientX: 10, clientY: 10 }), e = new TouchEvent('touchmove', { touches: [t], changedTouches: [t], cancelable: true, bubbles: true }); el.dispatchEvent(e); return e.defaultPrevented; }, sel);

{ // an ordinary browser
  const page = await open(0); let r = await m(page);
  check('ordinary page: left alone', !r.on);
  check('ordinary page: drags are not touched', await drag(page, '#logo') === false);
  await page.evaluate(() => { window.__sy = -40; }); await page.waitForTimeout(120); await page.evaluate(() => { window.__sy = -15; }); await page.waitForTimeout(260); await page.evaluate(() => { window.__sy = 0; }); await page.waitForTimeout(900);
  r = await m(page); check('ordinary page: a bounce changes nothing', !r.on);
  await page.close(); }

{ // the web view counts from the top inset: the page rests at -61
  const page = await open(-61); let r = await m(page);
  check('rests at -61: window found without any touch', r.on && r.s === 0 && r.R === 61 + B, JSON.stringify([r.on, r.s, r.R]));
  check('  top of the page is the top of the window, bottom keeps 148 clear', r.st === '0px' && r.sb === '148px' && r.logo[1] < 20 && r.fine[3] <= H - 148 - 20, JSON.stringify([r.st, r.sb, r.logo[1], r.fine[3]]));
  check('  drags no longer move the page, lists and sliders still work', await drag(page, '#logo') === true);
  if (shots) await page.screenshot({ path: `${shots}/host_home.png` });
  await page.setInputFiles('#file', path.resolve(rom)); await page.waitForTimeout(3500);
  r = await m(page); let out = r.els.filter(b => b[1] < -1 || b[3] > H - 148 + 1);
  check('  game: pad inside the window', r.pad.join() === '0,0,148,0' && r.els.length > 10 && !out.length, JSON.stringify([r.pad, out.length]));
  if (shots) await page.screenshot({ path: `${shots}/host_game.png` });
  await page.tap('#b-menu'); await page.waitForTimeout(400);
  const pb = await page.evaluate(() => { const r = document.getElementById('m-home').getBoundingClientRect(); return Math.round(r.bottom); });
  check('  menu: last row above the bottom of the window', pb <= H - 148, String(pb));
  await page.evaluate(() => { document.getElementById('m-set').click(); document.querySelector('#tabs [data-tab=pad]').click(); });
  check('  a drag inside the settings list is allowed', await drag(page, '#binds') === false && await drag(page, '#s-tsize') === false);
  await page.keyboard.press('Escape'); await page.keyboard.press('Escape');
  await at(page, 87); r = await m(page); out = r.els.filter(b => b[1] < 148 - 1 || b[3] > H + 1);
  check('  page pushed to its far end: layout follows the window', r.s === 148 && r.pad.join() === '148,0,0,0' && !out.length, JSON.stringify([r.s, r.pad, out.length]));
  await at(page, -61); r = await m(page); check('  and back', r.s === 0 && r.pad.join() === '0,0,148,0');
  await page.setViewportSize({ width: W, height: H - 1 }); await page.evaluate(() => { window.__sy = 0; }); await page.waitForTimeout(1000);
  r = await m(page); check('  a changed window is measured afresh', !r.on, JSON.stringify([r.on, r.R]));
  await page.close(); }

{ // the web view counts from zero: nothing shows until the page has been moved once
  const page = await open(0); let r = await m(page);
  check('counts from zero: nothing to see at rest', !r.on);
  await at(page, 148); r = await m(page);
  check('  found resting at 148: window known, layout at the far end', r.on && r.R === T + B && r.s === 148, JSON.stringify([r.on, r.s, r.R]));
  await at(page, 0); r = await m(page);
  check('  back at the top: top 0, bottom 149 clear', r.s === 0 && r.st === '0px' && r.sb === '149px', JSON.stringify([r.s, r.st, r.sb]));
  await page.close(); }
await browser.close();
console.log(bad ? `${bad} FAILED` : 'all passed'); process.exit(bad ? 1 : 0);
