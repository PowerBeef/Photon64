// Expansion Pak setting in the shipped page: status text, change + reset, save states across memory sizes.
//   node tools/xpakui.mjs rom-that-requires-the-pak [other-rom]
import path from 'path';
import { chromium } from 'playwright';
const [romA, romB] = process.argv.slice(2);
const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
const page = await (await browser.newContext({ viewport: { width: 1100, height: 760 } })).newPage();
page.on('pageerror', e => console.log('[pageerror]', e.message));
await page.goto('file://' + path.resolve('out/photon64.html'));
let bad = 0;
const st = () => page.evaluate(() => { const p = window.__photon, v = p.ex.n64_xpak(), m = new Uint32Array(p.ex.memory.buffer, p.hi[0], 0x200000);
  return { kind: v & 15, now: (v >> 8) & 1, next: (v >> 9) & 1, size: Math.max(m[0x318 >> 2], m[0x3F0 >> 2]) >>> 20, info: document.getElementById('xpakinfo').textContent, toast: document.getElementById('toast').textContent, frames: new Uint32Array(p.ex.memory.buffer, p.hi[21], 2)[0] }; });
const check = (name, s, cond) => { if (!cond) bad++; console.log(cond ? 'ok  ' : 'FAIL', name, '|', s.info, '| toast:', s.toast); };
const set = v => page.evaluate(v => { const el = document.getElementById('s-xpak'); el.value = v; el.dispatchEvent(new Event('change')); }, v);
console.log('before a game:', await page.evaluate(() => document.getElementById('xpakinfo').textContent));
await page.setInputFiles('#file', path.resolve(romA));
await page.waitForTimeout(2500);
let s = await st(); check('loaded on Auto: required game gets the pak', s, s.kind === 3 && s.now && /requires it\. Installed\.$/.test(s.info) && /Expansion Pak/.test(s.toast));
await set('off'); s = await st(); check('set to Removed while running: pending', s, s.now && !s.next && /Reset the game to remove it/.test(s.info) && /Reset the game to remove/.test(s.toast));
await page.evaluate(() => window.__photon.resetGame()); await page.waitForTimeout(1500);
s = await st(); check('after reset: 4 MB machine', s, !s.now && s.size === 4 && /Not installed\.$/.test(s.info));
await page.keyboard.press('F2'); await page.waitForTimeout(2500);           // save state on the 4 MB machine
await set('auto'); await page.evaluate(() => window.__photon.resetGame()); await page.waitForTimeout(1500);
s = await st(); check('back to Auto + reset: 8 MB machine', s, s.now && s.size === 8 && /Installed\.$/.test(s.info));
const f0 = s.frames;
await page.keyboard.press('F4'); await page.waitForTimeout(2500);           // the state was made without the pak
s = await st(); check('state from the 4 MB machine loads as a 4 MB machine and keeps running', s, !s.now && s.next && s.size === 4 && /Reset the game to install it/.test(s.info) && s.frames > 0);
await page.evaluate(() => window.__photon.resetGame()); await page.waitForTimeout(1200);
s = await st(); check('reset installs it again', s, s.now && s.size === 8);
// reload with Removed: the page warns
await set('off');
await page.setInputFiles('#file', path.resolve(romA)); await page.waitForTimeout(2500);
s = await st(); check('loading a game that requires it with the setting on Removed warns', s, !s.now && /needs the Expansion Pak/.test(s.toast));
await page.screenshot({ path: 'out/xpak_warn.png' });
await set('auto');
if (romB) { await page.setInputFiles('#file', path.resolve(romB)); await page.waitForTimeout(2500); s = await st(); check('a game that does not use it: installed anyway on Auto', s, s.kind === 0 && s.now && s.size === 8 && /not known to use it\. Installed\.$/.test(s.info)); }
await page.evaluate(() => { window.__photon.openSheet('settings'); document.querySelector('#tabs [data-tab=console]').click(); });
await page.waitForTimeout(300); await page.screenshot({ path: 'out/xpak_settings.png' });
await browser.close();
console.log(bad ? `${bad} FAILED` : 'all passed'); process.exit(bad ? 1 : 0);
