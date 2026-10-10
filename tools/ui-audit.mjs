// Shared DOM geometry checks, executed unchanged by Playwright and native Safari.
// Inspect actual clipping ancestors after scrolling each control into view.
export async function auditUi() {
  const errors = [], rect = e => { const r = e.getBoundingClientRect(); return { x: r.left, y: r.top, right: r.right, bottom: r.bottom, width: r.width, height: r.height }; };
  const visible = e => e.getClientRects().length && !e.closest('[hidden]');
  const sheet = document.getElementById('sheet'), dialog = sheet.querySelector('.panel');
  const open = !sheet.hidden, root = open ? dialog : document.getElementById('home');
  const body = rect(document.body), epsilon = 1.5;
  const css = getComputedStyle(document.documentElement), safe = ['--st', '--sr', '--sb', '--sl'].map(k => parseFloat(css.getPropertyValue(k)) || 0);
  const name = e => e.id || e.textContent.trim().slice(0, 50) || e.getAttribute('aria-label') || e.tagName;
  if (open) {
    const r = rect(dialog);
    if (r.x < -epsilon || r.right > innerWidth + epsilon || r.y < -epsilon || r.bottom > innerHeight + epsilon) errors.push('Dialog extends outside viewport');
    if (!dialog.getAttribute('aria-label') && !document.getElementById(dialog.getAttribute('aria-labelledby'))?.textContent.trim()) errors.push('Dialog has no accessible name');
    if (!dialog.contains(document.activeElement)) errors.push('Focus is outside open dialog');
    if (!document.getElementById('home').inert || !document.getElementById('stage').inert) errors.push('Background is interactive while dialog is open');
  }
  for (const e of [root, ...root.querySelectorAll('.pg, .pg-b, .tabs, .seg, #lib, #binds')].filter(visible)) {
    if (e.scrollWidth > e.clientWidth + epsilon) errors.push(name(e) + ' overflows horizontally');
  }
  const controls = [...root.querySelectorAll('button, input, summary, [role="button"]')].filter(visible);
  for (const e of controls) {
    e.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    await new Promise(r => requestAnimationFrame(r));
    const r = rect(e);
    let clip = { x: safe[3], y: safe[0], right: innerWidth - safe[1], bottom: innerHeight - safe[2] };
    for (let p = e.parentElement; p; p = p.parentElement) {
      const cs = getComputedStyle(p), b = rect(p);
      if (/(auto|scroll|hidden|clip)/.test(cs.overflowX)) { clip.x = Math.max(clip.x, b.x); clip.right = Math.min(clip.right, b.right); }
      if (/(auto|scroll|hidden|clip)/.test(cs.overflowY)) { clip.y = Math.max(clip.y, b.y); clip.bottom = Math.min(clip.bottom, b.bottom); }
    }
    if (r.width < 1 || r.height < 1 || r.x < clip.x - epsilon || r.right > clip.right + epsilon || r.y < clip.y - epsilon || r.bottom > clip.bottom + epsilon) errors.push(name(e) + ' cannot be brought fully into view');
  }
  const screen = document.getElementById('screen'), cv = document.getElementById('cv-gpu');
  if (!document.getElementById('stage').hidden && screen.clientWidth && screen.clientHeight) {
    for (const e of [...document.querySelectorAll('#touch .t, #b-menu')].filter(visible)) {
      const r = rect(e);
      if (r.x < safe[3] - epsilon || r.right > innerWidth - safe[1] + epsilon || r.y < safe[0] - epsilon || r.bottom > innerHeight - safe[2] + epsilon) errors.push(name(e) + ' is outside the safe viewport');
    }
    if (cv.width > 4096 || cv.height > 4096) errors.push('GPU presentation exceeds backing-store limit');
    if (Math.abs(cv.width * screen.clientHeight - cv.height * screen.clientWidth) > screen.clientHeight + screen.clientWidth) errors.push('GPU canvas changes display aspect ratio');
    const sw = document.getElementById('cv-sw'), r = rect(sw);
    if (!window.__photon.settings.aspect && Math.abs(r.width * 3 - r.height * 4) > 2) errors.push('Software picture changes 4:3 aspect ratio');
  }
  return { errors: [...new Set(errors)], viewport: [innerWidth, innerHeight], dpr: devicePixelRatio, body, panel: open ? rect(dialog) : null, controls: controls.length, gpuCanvas: [cv.width, cv.height], screen: [screen.clientWidth, screen.clientHeight] };
}
