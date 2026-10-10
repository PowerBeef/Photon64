// Photon64 interface artwork: the logo, the icon set and the illustrations, all inline SVG.
// Icons are 24 x 24 line drawings in the current text colour; everything scales with the element that holds it.
const UiArt = (() => {
  const C = { green: '#22b35c', blue: '#3b7bff', red: '#ea4335', yellow: '#f6c21c' };
  const line = body => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" focusable="false">${body}</svg>`;
  const ICONS = {
    play: '<path d="M7 4.5v15l12.5-7.5z" fill="currentColor"/>',
    pause: '<path d="M8 5v14M16 5v14" stroke-width="3"/>',
    save: '<path d="M12 3v11m0 0-4-4m4 4 4-4"/><path d="M4 14v4a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-4"/>',
    load: '<path d="M12 14V3m0 0-4 4m4-4 4 4"/><path d="M4 14v4a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-4"/>',
    ff: '<path d="M3.5 6v12l8-6zM12.5 6v12l8-6z" fill="currentColor"/>',
    reset: '<path d="M4 12a8 8 0 1 0 2.5-5.8"/><path d="M4 4v5h5"/>',
    sound: '<path d="M4 10v4h3.5L13 18V6l-5.5 4z" fill="currentColor"/><path d="M16.5 9a4 4 0 0 1 0 6M19 6.5a8 8 0 0 1 0 11"/>',
    mute: '<path d="M4 10v4h3.5L13 18V6l-5.5 4z" fill="currentColor"/><path d="m17 9.5 5 5m0-5-5 5"/>',
    full: '<path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/>',
    gear: '<path d="M10.51 5.26L10.69 2.69L13.31 2.69L13.49 5.26A6.9 6.9 0 0 1 15.71 6.18L17.66 4.49L19.51 6.34L17.82 8.29A6.9 6.9 0 0 1 18.74 10.51L21.31 10.69L21.31 13.31L18.74 13.49A6.9 6.9 0 0 1 17.82 15.71L19.51 17.66L17.66 19.51L15.71 17.82A6.9 6.9 0 0 1 13.49 18.74L13.31 21.31L10.69 21.31L10.51 18.74A6.9 6.9 0 0 1 8.29 17.82L6.34 19.51L4.49 17.66L6.18 15.71A6.9 6.9 0 0 1 5.26 13.49L2.69 13.31L2.69 10.69L5.26 10.51A6.9 6.9 0 0 1 6.18 8.29L4.49 6.34L6.34 4.49L8.29 6.18A6.9 6.9 0 0 1 10.51 5.26ZM15.0 12a3.0 3.0 0 1 0 -6.0 0a3.0 3.0 0 1 0 6.0 0Z" fill="currentColor" fill-rule="evenodd" stroke-width="1.3"/>',
    library: '<rect x="3.5" y="3.5" width="7" height="7" rx="1.6"/><rect x="13.5" y="3.5" width="7" height="7" rx="1.6"/><rect x="3.5" y="13.5" width="7" height="7" rx="1.6"/><rect x="13.5" y="13.5" width="7" height="7" rx="1.6"/>',
    back: '<path d="m14.5 5-7 7 7 7"/>',
    close: '<path d="m6 6 12 12M18 6 6 18"/>',
    plus: '<path d="M12 5v14M5 12h14"/>',
    trash: '<path d="M4 7h16M9 7V4.5h6V7m-8 0 1 12.5h8L17 7"/>',
    video: '<rect x="3" y="4.5" width="18" height="12" rx="2"/><path d="M8.5 20h7M12 16.5V20"/>',
    console: '<path d="M3 15.5 5 9h14l2 6.5v2a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z"/><path d="M8 9V6.5h8V9M7 14.5h4M15.5 14.5h.01M18 14.5h.01"/>',
    pad: '<path d="M7 7h10a5 5 0 0 1 5 5v0a5 5 0 0 1-8.5 3.5h-3A5 5 0 0 1 2 12v0a5 5 0 0 1 5-5z"/><path d="M7 10.5v3M5.5 12h3M16 11h.01M18 13h.01"/>',
    data: '<ellipse cx="12" cy="6" rx="7.5" ry="3"/><path d="M4.5 6v6c0 1.7 3.4 3 7.5 3s7.5-1.3 7.5-3V6M4.5 12v6c0 1.7 3.4 3 7.5 3s7.5-1.3 7.5-3v-6"/>',
    check: '<path d="m5 12.5 4.5 4.5L19 7.5"/>',
  };
  const icon = name => line(ICONS[name] || '');

  // The generated identity is bundled from assets/logo-on-dark.svg as paths.
  const logo = (w = 244) => BRAND_SVG.replace('<svg ', `<svg width="${w}" `);
  // empty library: the cartridge as an outline, waiting for a game
  const emptyCart = (w = 120) => `<svg viewBox="0 0 48 48" width="${w}" height="${w}" aria-hidden="true" focusable="false">
    <path d="M9 5h30a4 4 0 0 1 4 4v23l-3.2 3.2V43H8.2v-7.8L5 32V9a4 4 0 0 1 4-4z" fill="#1d2027" stroke="#5a606c" stroke-width="1.2" stroke-dasharray="3 2.4"/>
    <rect x="10" y="9.5" width="28" height="20" rx="3" fill="#262a33"/>
    <path d="M24 14v11M18.5 19.5h11" stroke="#8b93a3" stroke-width="2.4" stroke-linecap="round"/></svg>`;
  return { C, icon, logo, emptyCart };
})();
