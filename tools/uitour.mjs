// Walk through the interface in headless Chromium and take a picture of every screen.
//   node tools/uitour.mjs outDir [phone|land|desk] [romDir=roms]
import fs from 'fs'; import path from 'path';
import { chromium } from 'playwright';
const [out, prof = 'phone', romDir = 'roms'] = process.argv.slice(2);
const P = { phone: { viewport: { width: 393, height: 852 }, deviceScaleFactor: 3, hasTouch: true, isMobile: true },
  land: { viewport: { width: 852, height: 393 }, deviceScaleFactor: 3, hasTouch: true, isMobile: true },
  desk: { viewport: { width: 1280, height: 800 }, deviceScaleFactor: 1 } }[prof];
const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
const page = await (await browser.newContext(P)).newPage();
if (prof !== 'desk') await page.addInitScript(() => { delete Element.prototype.requestFullscreen; });   // as on an iPhone
const errs = [];
page.on('console', m => { if (m.type() === 'error') { errs.push(m.text()); console.log('[page]', m.text().slice(0, 300)); } });
page.on('pageerror', e => { errs.push(e.message); console.log('[pageerror]', e.message); });
const insets = prof === 'phone' ? '?insets=59,0,34,0' : prof === 'land' ? '?insets=0,59,21,59' : '';
await page.goto('file://' + path.resolve('out/photon64.html') + insets);
const shot = async n => { await page.waitForTimeout(350); await page.screenshot({ path: `${out}/${prof}_${n}.png` }); };
const roms = fs.readdirSync(romDir).filter(f => /\.z64$/.test(f)).sort();
await shot('01_empty');
let first = true;
for (const r of roms) {
  await page.setInputFiles('#file', path.join(romDir, r));
  await page.waitForFunction(() => window.__photon.rom, null, { timeout: 30000 });
  await page.waitForTimeout(first ? 9000 : 7000);
  if (first) await shot('02_game');
  await page.click('#b-menu'); await page.waitForTimeout(600);
  if (first) {
    await shot('03_menu');
    await page.click('#m-save'); await shot('04_save');
    await page.click('#slots .slot:nth-child(1)'); await page.waitForTimeout(1200);
    await page.click('#b-menu'); await page.click('#m-save'); await page.click('#slots .slot:nth-child(3)'); await page.waitForTimeout(1200);
    await page.click('#b-menu'); await page.click('#m-load'); await shot('05_load');
    await page.click('#pg-states [data-nav=back]');
    await page.click('#m-reset'); await shot('06_reset_armed');
    await page.click('#m-set');
    for (const t of ['video', 'console', 'pad', 'data']) { await page.click(`#tabs [data-tab=${t}]`); await shot('07_set_' + t); }
    await page.click('#pg-settings [data-nav=back]');
    first = false;
  }
  await page.click('#m-home'); await page.waitForTimeout(500);
}
await shot('08_library');
console.log('library:', JSON.stringify((await page.evaluate(() => window.__photon.libList())).map(e => e.name)));
await page.click('#h-set'); await shot('09_home_settings');
await browser.close();
console.log(errs.length ? 'ERRORS ' + errs.length : 'no page errors');
