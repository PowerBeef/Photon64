// Stick and buttons driven by synthetic touch pointers with arbitrary ids (iOS hands out large and negative ones).
import path from 'path';
import { chromium } from 'playwright';
const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
const ctx = await browser.newContext({ viewport: { width: 402, height: 874 }, deviceScaleFactor: 2, hasTouch: true, isMobile: true });
const page = await ctx.newPage();
page.on('pageerror', e => console.log('[pageerror]', e.message));
await page.goto('file://' + path.resolve(process.argv[2] || 'out/photon64.html'));
await page.setInputFiles('#file', path.resolve('roms/Super Mario 64 (USA).z64'));
await page.waitForTimeout(2500);
let bad = 0;
for (const id of [5, 0, -1, -1687463250, 2147483000]) {
  const r = await page.evaluate(id => {
    const st = document.getElementById('stick').getBoundingClientRect(), stage = document.getElementById('stage'), ts = window.__photon.touchState;
    const A = document.querySelector('#touch .t[data-b=A]').getBoundingClientRect();
    const ev = (type, pid, x, y) => stage.dispatchEvent(new PointerEvent(type, { pointerId: pid, pointerType: 'touch', clientX: x, clientY: y, bubbles: true, cancelable: true, isPrimary: true }));
    const cx = st.left + st.width / 2, cy = st.top + st.height / 2, out = {};
    ev('pointerdown', id, cx, cy); ev('pointermove', id, cx + st.width * 0.3, cy);
    out.right = [Math.round(ts.x), Math.round(ts.y)];
    ev('pointermove', id, cx, cy - st.width * 0.5); out.up = [Math.round(ts.x), Math.round(ts.y)];
    ev('pointerdown', id + 1, A.left + A.width / 2, A.top + A.height / 2); out.withA = [Math.round(ts.x), Math.round(ts.y), ts.buttons.toString(16)];
    ev('pointerup', id, cx, cy); out.released = [Math.round(ts.x), Math.round(ts.y), ts.buttons.toString(16)];
    ev('pointerup', id + 1, 0, 0); out.idle = [ts.x, ts.y, ts.buttons];
    return out;
  }, id);
  const ok = r.right[0] > 60 && r.up[1] === 80 && r.withA[2] === '8000' && r.withA[1] === 80 && r.released[0] === 0 && r.released[2] === '8000' && r.idle.every(v => v === 0);
  if (!ok) bad++;
  console.log('pointer id', String(id).padStart(11), ok ? 'ok  ' : 'FAIL', JSON.stringify(r));
}
await browser.close();
process.exit(bad ? 1 : 0);
