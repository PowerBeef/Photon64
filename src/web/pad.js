// Photon64 touch pad artwork. Every control is an inline SVG generated here, so it stays sharp at any size and pixel density
// and the whole pad costs a few kilobytes. All shapes share one set of gradients (PadArt.defs) and the same construction:
// a soft shadow, a dark bezel with a top-lit rim, a domed cap in the control's colour, a gloss highlight and the legend.
// Two extra layers, .lit and .glow, are invisible until CSS shows them for a pressed control.
const PadArt = (() => {
  const TONE = {                  // highlight, body, shade, glow
    a: ['#9cc3ff', '#2f6df0', '#112c78', '#8dbbff'],     // A: blue
    b: ['#93ecb0', '#1ea651', '#094a22', '#7df2aa'],     // B: green
    c: ['#fff0a3', '#f4bd14', '#8a5d00', '#ffe270'],     // C: yellow
    s: ['#ffab9f', '#e23b30', '#74100b', '#ff958a'],     // START: red
    n: ['#a3acbe', '#4d5567', '#191d26', '#d3dbee'],     // Z, L, R, menu: graphite
    d: ['#838c9c', '#3b4251', '#10131a', '#d3dbee'],     // D-pad: darker graphite
    k: ['#ffffff', '#c6ccd7', '#666d7c', '#bcd6ff'],     // stick cap: light grey
  };
  const FONT = `font-family="ui-rounded,'SF Pro Rounded','Arial Rounded MT Bold',system-ui,-apple-system,'Segoe UI',Roboto,sans-serif" font-weight="800" text-anchor="middle"`;
  const svg = (w, h, body) => `<svg viewBox="0 0 ${w} ${h}" aria-hidden="true" focusable="false">${body}</svg>`;
  const defs = () => `<svg width="0" height="0" style="position:absolute" aria-hidden="true" focusable="false"><defs>` +
    Object.entries(TONE).map(([k, t]) => `<radialGradient id="pg-${k}" cx=".37" cy=".27" r=".86"><stop offset="0" stop-color="${t[0]}"/><stop offset=".5" stop-color="${t[1]}"/><stop offset="1" stop-color="${t[2]}"/></radialGradient>`).join('') +
    `<linearGradient id="pg-rim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".78"/><stop offset=".45" stop-color="#fff" stop-opacity=".24"/><stop offset="1" stop-color="#fff" stop-opacity=".13"/></linearGradient>` +
    `<linearGradient id="pg-gloss" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".62"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>` +
    `<radialGradient id="pg-shadow"><stop offset=".72" stop-color="#000" stop-opacity=".6"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>` +
    `<radialGradient id="pg-well"><stop offset="0" stop-color="#2a3142" stop-opacity=".34"/><stop offset=".7" stop-color="#0d1018" stop-opacity=".6"/><stop offset="1" stop-color="#04060a" stop-opacity=".82"/></radialGradient>` +
    `<radialGradient id="pg-dimple"><stop offset="0" stop-color="#000" stop-opacity=".5"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>` +
    `</defs></svg>`;
  const BEZEL = 'fill="#06080d" fill-opacity=".76"';
  const legend = (x, y, size, s, fill = '#fff', extra = '') =>
    `<text x="${x}" y="${y + size * 0.045}" dy=".355em" font-size="${size}" fill="#000" fill-opacity=".34" ${FONT} ${extra}>${s}</text>` +
    `<text x="${x}" y="${y}" dy=".355em" font-size="${size}" fill="${fill}" ${FONT} ${extra}>${s}</text>`;
  // an arrow head pointing up, drawn engraved (dark with a light lower edge); rot turns it
  const arrow = (cx, cy, r, rot, fill, lip) => {
    const d = `M${cx} ${cy - r}L${cx + r * 0.95} ${cy + r * 0.72}H${cx - r * 0.95}Z`;
    return `<g transform="rotate(${rot} ${cx} ${cy})"><path d="${d}" transform="translate(0 ${r * 0.1})" fill="${lip}" stroke="${lip}" stroke-width="${r * 0.3}" stroke-linejoin="round"/>` +
      `<path d="${d}" fill="${fill}" stroke="${fill}" stroke-width="${r * 0.3}" stroke-linejoin="round"/></g>`;
  };

  // round button: `inner` is the legend markup for a 100 x 100 box
  const round = (tone, inner) => svg(100, 100,
    `<circle cx="50" cy="53.5" r="46.5" fill="url(#pg-shadow)"/>` +
    `<circle cx="50" cy="50" r="46" ${BEZEL}/><circle cx="50" cy="50" r="45.2" fill="none" stroke="url(#pg-rim)" stroke-width="1.6"/>` +
    `<circle cx="50" cy="50" r="39.6" fill="url(#pg-${tone})"/><circle cx="50" cy="50" r="39" fill="none" stroke="#000" stroke-opacity=".3" stroke-width="1.2"/>` +
    `<ellipse cx="50" cy="28" rx="25" ry="13" fill="url(#pg-gloss)"/>` +
    `<circle class="lit" cx="50" cy="50" r="39.6" fill="#fff"/>` + inner +
    `<circle class="glow" cx="50" cy="50" r="47.6" fill="none" stroke="${TONE[tone][3]}" stroke-width="3.2"/>`);
  const face = (tone, letter) => round(tone, legend(50, 50, 45, letter));
  const cButton = rot => round('c', arrow(50, 49, 15, rot, '#5b3d00', '#fff3b8'));
  const menu = () => round('n', `<path d="M33 37.5H67M33 50H67M33 62.5H67" stroke="#000" stroke-opacity=".3" stroke-width="6.4" stroke-linecap="round" transform="translate(0 2)"/>` +
    `<path d="M33 37.5H67M33 50H67M33 62.5H67" stroke="#fff" stroke-width="6.4" stroke-linecap="round"/>`);

  // rounded rectangle with four corner radii (top-left, top-right, bottom-right, bottom-left)
  const rr = (x0, y0, x1, y1, [a, b, c, d]) => `M${x0 + a} ${y0}H${x1 - b}A${b} ${b} 0 0 1 ${x1} ${y0 + b}V${y1 - c}A${c} ${c} 0 0 1 ${x1 - c} ${y1}H${x0 + d}A${d} ${d} 0 0 1 ${x0} ${y1 - d}V${y0 + a}A${a} ${a} 0 0 1 ${x0 + a} ${y0}Z`;
  // bar-shaped button, w wide for a height of 100. shape: 'l' / 'r' round off the outer end like a shoulder button, anything else is a capsule
  const bar = (tone, w, shape, label, size, spacing = 0) => {
    const k = shape === 'l' ? [1, .42, .42, 1] : shape === 'r' ? [.42, 1, 1, .42] : [1, 1, 1, 1];
    const rad = h => k.map(v => v * h / 2);
    return svg(w, 100,
      `<path d="${rr(5, 12, w - 5, 100, rad(88))}" fill="#000" fill-opacity=".3"/>` +
      `<path d="${rr(4, 4, w - 4, 96, rad(92))}" ${BEZEL}/><path d="${rr(4.8, 4.8, w - 4.8, 95.2, rad(90.4))}" fill="none" stroke="url(#pg-rim)" stroke-width="1.8"/>` +
      `<path d="${rr(12, 12, w - 12, 88, rad(76))}" fill="url(#pg-${tone})"/><path d="${rr(12.7, 12.7, w - 12.7, 87.3, rad(74.6))}" fill="none" stroke="#000" stroke-opacity=".3" stroke-width="1.4"/>` +
      `<path d="${rr(20, 15.5, w - 20, 46, rad(30.5))}" fill="url(#pg-gloss)" opacity=".8"/>` +
      `<path class="lit" d="${rr(12, 12, w - 12, 88, rad(76))}" fill="#fff"/>` +
      legend(w / 2, 50, size, label, '#fff', spacing ? `letter-spacing="${spacing}"` : '') +
      `<path class="glow" d="${rr(1.6, 1.6, w - 1.6, 98.4, rad(96.8))}" fill="none" stroke="${TONE[tone][3]}" stroke-width="3.2"/>`);
  };

  // analogue stick: a recessed well with the console's eight-sided gate, and a ridged cap that moves inside it
  const stick = () => {
    const oct = Array.from({ length: 8 }, (_, i) => { const t = (i + .5) * Math.PI / 4; return (100 + 69 * Math.cos(t)).toFixed(1) + ' ' + (100 + 69 * Math.sin(t)).toFixed(1); }).join(' ');
    const tick = r => `<path d="M100 6.5l5.5 8h-11z" fill="#fff" fill-opacity=".34" transform="rotate(${r} 100 100)"/>`;
    return svg(200, 200,
      `<circle cx="100" cy="100" r="98" fill="url(#pg-well)"/><circle cx="100" cy="100" r="96.8" fill="none" stroke="url(#pg-rim)" stroke-width="2.2"/>` +
      `<circle cx="100" cy="100" r="82" fill="none" stroke="#000" stroke-opacity=".42" stroke-width="3"/><circle cx="100" cy="100" r="80" fill="none" stroke="#fff" stroke-opacity=".08" stroke-width="1.2"/>` +
      `<polygon points="${oct}" fill="#fff" fill-opacity=".035" stroke="#fff" stroke-opacity=".16" stroke-width="2" stroke-linejoin="round"/>` +
      tick(0) + tick(90) + tick(180) + tick(270) +
      `<circle class="glow" cx="100" cy="100" r="98.4" fill="none" stroke="${TONE.k[3]}" stroke-width="2.4"/>`);
  };
  const nub = () => svg(100, 100,
    `<circle cx="50" cy="56" r="44" fill="url(#pg-shadow)"/>` +
    `<circle cx="50" cy="50" r="43" fill="url(#pg-k)"/><circle cx="50" cy="50" r="42.3" fill="none" stroke="#000" stroke-opacity=".4" stroke-width="1.4"/>` +
    [31, 21.5, 12].map(r => `<circle cx="50" cy="51.2" r="${r}" fill="none" stroke="#fff" stroke-opacity=".5" stroke-width="1.5"/><circle cx="50" cy="50" r="${r}" fill="none" stroke="#000" stroke-opacity=".2" stroke-width="2.2"/>`).join('') +
    `<ellipse cx="50" cy="25.5" rx="24" ry="11" fill="url(#pg-gloss)" opacity=".85"/>`);

  // D-pad: one plus-shaped cap. The four directions are separate hit areas laid over its arms.
  const plus = (a, c, r, q) => { const b = 300 - a, d = 300 - c;
    return `M${c + r} ${a}H${d - r}A${r} ${r} 0 0 1 ${d} ${a + r}V${c - q}A${q} ${q} 0 0 0 ${d + q} ${c}H${b - r}A${r} ${r} 0 0 1 ${b} ${c + r}V${d - r}A${r} ${r} 0 0 1 ${b - r} ${d}H${d + q}A${q} ${q} 0 0 0 ${d} ${d + q}` +
      `V${b - r}A${r} ${r} 0 0 1 ${d - r} ${b}H${c + r}A${r} ${r} 0 0 1 ${c} ${b - r}V${d + q}A${q} ${q} 0 0 0 ${c - q} ${d}H${a + r}A${r} ${r} 0 0 1 ${a} ${d - r}V${c + r}A${r} ${r} 0 0 1 ${a + r} ${c}H${c - q}A${q} ${q} 0 0 0 ${c} ${c - q}V${a + r}A${r} ${r} 0 0 1 ${c + r} ${a}Z`; };
  const dpad = () => svg(300, 300,
    `<path d="${plus(8, 100, 24, 6)}" transform="translate(0 9)" fill="#000" fill-opacity=".3"/>` +
    `<path d="${plus(6, 98, 26, 6)}" ${BEZEL}/><path d="${plus(8, 100, 24, 6)}" fill="none" stroke="url(#pg-rim)" stroke-width="3.6"/>` +
    `<path d="${plus(21, 113, 15, 9)}" fill="url(#pg-d)"/><path d="${plus(22.5, 114.5, 14, 9)}" fill="none" stroke="url(#pg-rim)" stroke-opacity=".5" stroke-width="2.4"/>` +
    `<circle cx="150" cy="150" r="27" fill="url(#pg-dimple)"/><circle cx="150" cy="151" r="26" fill="none" stroke="#fff" stroke-opacity=".12" stroke-width="2"/>` +
    [0, 90, 180, 270].map(r => `<g transform="rotate(${r} 150 150)">${arrow(150, 57, 15, 0, '#f3f6fb', '#0b0d12')}</g>`).join(''));
  // plate the four C buttons sit in
  const cluster = () => svg(200, 200,
    `<circle cx="100" cy="100" r="98" fill="url(#pg-well)"/><circle cx="100" cy="100" r="96.8" fill="none" stroke="url(#pg-rim)" stroke-width="2.2"/>` +
    `<text x="100" y="100" dy=".355em" font-size="30" fill="${TONE.c[1]}" fill-opacity=".8" ${FONT}>C</text>`);
  return { defs, face, cButton, menu, bar, stick, nub, dpad, cluster };
})();
