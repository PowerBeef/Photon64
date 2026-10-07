// Lays the touch pad out on a matrix of screen sizes / safe areas and checks that it fits.
//   node tools/layouttest.mjs [--shots dir] [--rom file]
import path from 'path'; import fs from 'fs';
import { chromium } from 'playwright';
const arg = n => { const i = process.argv.indexOf(n); return i < 0 ? null : process.argv[i + 1]; };
const shots = arg('--shots'), romFile = arg('--rom') || 'roms/Super Mario 64 (USA).z64';
if (shots) fs.mkdirSync(shots, { recursive: true });
// name, width, height, insets [top, right, bottom, left]
const P = (name, w, h, t = 0, b = 0, side = 0, lb = 21) => [[name + ' portrait', w, h, [t, 0, b, 0]], [name + ' landscape', h, w, [0, side, b ? lb : 0, side]]];
const cases = [
  ...P('iPhone SE (1st)', 320, 568, 20), ...P('iPhone SE (3rd)', 375, 667, 20), ...P('iPhone 13 mini', 375, 812, 50, 34, 50),
  ...P('iPhone 17 Pro', 402, 874, 62, 34, 62), ...P('iPhone 17 Pro Max', 440, 956, 62, 34, 62),
  ['iPhone 17 Pro in-app portrait', 402, 874, [126, 0, 24, 0]], ['iPhone 17 Pro in-app landscape', 874, 402, [0, 62, 82, 62]],
  ['iPhone 17 Pro Safari portrait', 402, 660, [0, 0, 0, 0]], ['iPhone 17 Pro Safari landscape', 874, 340, [0, 62, 21, 62]],
  ...P('Android small', 360, 640), ...P('Android tall', 360, 800, 24, 16, 0, 0), ...P('Pixel', 412, 915, 28, 24, 28, 0),
  ...P('Fold cover', 280, 653), ...P('Fold cover 2', 344, 882, 30, 20, 30, 0), ...P('Fold open', 884, 1104), ['Fold half', 720, 600, [0, 0, 0, 0]], ['Fold half upright', 600, 720, [0, 0, 0, 0]],
  ['Square', 500, 500, [0, 0, 0, 0]], ['Nearly square', 500, 540, [0, 0, 0, 0]], ['Ultra wide', 960, 360, [0, 40, 0, 40]],
  ...P('iPad mini', 744, 1133, 24, 20, 0, 20), ...P('iPad 11', 834, 1194, 24, 20, 0, 20), ...P('iPad 13', 1024, 1366, 24, 20, 0, 20),
  ['iPad slide-over', 320, 1024, [24, 0, 20, 0]], ['iPad split', 507, 1024, [24, 0, 20, 0]],
  ['Touch laptop', 1280, 800, [0, 0, 0, 0]], ['Touch desktop', 1920, 1080, [0, 0, 0, 0]],
];
const variants = [{ tsize: 100, dpad: true }, { tsize: 100, dpad: false }, { tsize: 70, dpad: true }, { tsize: 130, dpad: true }, { tsize: 130, dpad: false, tlift: 100 }, { tsize: 100, dpad: false, tlift: 0 }];

const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
const ctx = await browser.newContext({ viewport: { width: 402, height: 874 }, deviceScaleFactor: 1, hasTouch: true, isMobile: true });
const page = await ctx.newPage();
page.on('pageerror', e => console.log('[pageerror]', e.message));
await page.goto('file://' + path.resolve('out/photon64.html'));
await page.setInputFiles('#file', path.resolve(romFile));
await page.waitForTimeout(9000);
await page.evaluate(() => window.__photon.setPaused(true));
await page.addStyleTag({ content: '#flag, #toast { display: none !important; }' });

let bad = 0, n = 0;
const rows = [];
for (const [name, w, h, insets] of cases) {
  for (const [vi, v] of variants.entries()) {
    await page.setViewportSize({ width: w, height: h });
    const r = await page.evaluate(async ({ insets, v }) => {
      const ph = window.__photon; Object.assign(ph.settings, { tlift: 50 }, v);
      for (const el of document.querySelectorAll('#touch .dp, #dpad')) el.hidden = !v.dpad;
      ph.setInsets(insets);
      await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
      const box = el => { const b = el.getBoundingClientRect(); return { x: b.left, y: b.top, w: b.width, h: b.height }; };
      const els = [...document.querySelectorAll('#touch .t, #touch .art')].filter(e => e.offsetParent).map(e => ({ id: e.dataset.b || e.id, art: e.classList.contains('art'), ...box(e), op: +getComputedStyle(e).opacity, svg: !!e.querySelector('svg') || e.classList.contains('dp') }));
      const s = box(document.getElementById('screen')), cv = document.getElementById('cv-sw');
      const mb = document.getElementById('b-menu');
      return { els, menu: mb.offsetParent ? box(mb) : null, screen: s, sw: cv.hidden ? null : box(cv), pad: ph.pad, vw: innerWidth, vh: innerHeight, op: getComputedStyle(document.getElementById('touch')).opacity };
    }, { insets, v });
    n++;
    const err = [], [it, ir, ib, il] = insets, safe = { x0: il, y0: it, x1: w - ir, y1: h - ib }, e = 0.6;
    const pd = r.pad, u = pd.u;
    // picture rectangle (4:3 inside the picture area)
    const s = r.screen; let pw = s.w, phh = s.h; if (pw * 3 > phh * 4) pw = phh * 4 / 3; else phh = pw * 3 / 4;
    const pic = { x: s.x + (s.w - pw) / 2, y: s.y + (s.h - phh) / 2, w: pw, h: phh };
    const hit = (a, b, g = 0) => a.x < b.x + b.w - g && b.x < a.x + a.w - g && a.y < b.y + b.h - g && b.y < a.y + a.h - g;
    for (const c of r.els) {
      if (c.x < safe.x0 - e || c.y < safe.y0 - e || c.x + c.w > safe.x1 + e || c.y + c.h > safe.y1 + e) err.push(`${c.id} outside the safe area`);
      if (r.menu && hit(c, r.menu, 1)) err.push(`${c.id} under the menu button`);
      if (pd.stacked && hit(c, pic, 1)) err.push(`${c.id} over the picture`);
      if (!c.svg) err.push(`${c.id} has no artwork`);
    }
    const hits = r.els.filter(c => !c.art).map(c => ({ ...c, round: Math.abs(c.w - c.h) < 1 }));
    for (let i = 0; i < hits.length; i++) for (let j = i + 1; j < hits.length; j++) {
      const a = hits[i], b = hits[j];
      let o;
      if (a.round && b.round) o = Math.hypot(a.x + a.w / 2 - b.x - b.w / 2, a.y + a.h / 2 - b.y - b.h / 2) < (a.w + b.w) / 2 - 0.5;
      else if (!a.round && !b.round) o = hit(a, b, 0.5);
      else { const c = a.round ? a : b, q = a.round ? b : a, cx = c.x + c.w / 2, cy = c.y + c.h / 2, rr = q.h / 2;   // circle against pill (a segment with radius)
        const sx = Math.max(q.x + rr, Math.min(q.x + q.w - rr, cx)); o = Math.hypot(cx - sx, cy - (q.y + rr)) < c.w / 2 + rr - 0.5; }
      if (o) err.push(`${a.id} overlaps ${b.id}`);
    }
    const get = id => r.els.find(c => c.id === id);
    const A = get('A'), st = get('stick'), cu = get('CU');
    const min = v.tsize < 100 ? 0.7 : 1;
    if (A.w < 44 * min) err.push(`A only ${A.w.toFixed(0)} px`);
    if (st.w < 84 * min) err.push(`stick only ${st.w.toFixed(0)} px`);
    if (get('L').h < 26 * min) err.push(`L only ${get('L').h.toFixed(0)} px high`);
    if (cu.w < 27 * min) err.push(`C only ${cu.w.toFixed(0)} px`);
    if (!r.menu) err.push('no menu button');
    if (r.menu && (r.menu.x + r.menu.w > safe.x1 + e || r.menu.y + r.menu.h > safe.y1 + e)) err.push('menu button outside the safe area');
    if (r.menu && pd.stacked && hit(r.menu, pic, 1)) err.push('menu button over the picture');
    if (r.menu && (r.menu.x < safe.x0 - e || r.menu.y < safe.y0 - e)) err.push('menu button outside the safe area');
    if (pd.stacked && pic.w < w * 0.6) err.push(`picture only ${(pic.w / w * 100).toFixed(0)} % of the width`);
    if (s.x < -e || s.x + s.w > w + e || s.y + s.h > h + e) err.push('picture area off screen');
    if (r.vw !== w || r.vh !== h) err.push('page scrolls / wrong viewport');
    const tag = `${name} ${w}x${h} [${insets}] size ${v.tsize}${v.dpad ? ' +dpad' : ''}${v.tlift != null ? ' lift ' + v.tlift : ''}`;
    rows.push({ tag, mode: pd.stacked ? 'stacked' : 'wide (' + r.els.filter(c => c.op < 0.9).length + ' on picture)', u: u.toFixed(2), A: A.w.toFixed(0), stick: st.w.toFixed(0), err });
    if (err.length) { bad++; console.log('FAIL', tag, '\n   ' + [...new Set(err)].join('\n   ')); }
    if (shots && vi < 2) await page.screenshot({ path: path.join(shots, `${String(cases.findIndex(c => c[0] === name)).padStart(2, '0')}_${name.replace(/[^\w]+/g, '_')}_${vi}.png`) });
  }
}
for (const r of rows.filter((_, i) => i % variants.length === 0)) console.log(r.tag.padEnd(64), r.mode.padEnd(20), 'u', r.u, 'A', r.A, 'stick', r.stick);
console.log(`${n} layouts checked, ${bad} failed`);
await browser.close();
process.exit(bad ? 1 : 0);
