// Photon64 front end: ROM loading, main loop and pacing, audio, input (keyboard / gamepad / touch), saves, UI.
const $ = id => document.getElementById(id);
const BTN = { A: 0x8000, B: 0x4000, Z: 0x2000, START: 0x1000, DU: 0x800, DD: 0x400, DL: 0x200, DR: 0x100, L: 0x20, R: 0x10, CU: 8, CD: 4, CL: 2, CR: 1 };
const KEY_ACTIONS = [['UP', 'Stick up'], ['DOWN', 'Stick down'], ['LEFT', 'Stick left'], ['RIGHT', 'Stick right'], ['A', 'A'], ['B', 'B'], ['Z', 'Z'], ['START', 'Start'],
  ['L', 'L'], ['R', 'R'], ['CU', 'C up'], ['CD', 'C down'], ['CL', 'C left'], ['CR', 'C right'], ['DU', 'D-pad up'], ['DD', 'D-pad down'], ['DL', 'D-pad left'], ['DR', 'D-pad right'],
  ['WALK', 'Walk (half tilt)'], ['FF', 'Fast-forward'], ['PAUSE', 'Pause']];
const DEFAULT_KEYS = { UP: 'ArrowUp', DOWN: 'ArrowDown', LEFT: 'ArrowLeft', RIGHT: 'ArrowRight', A: 'KeyX', B: 'KeyC', Z: 'KeyZ', START: 'Enter', L: 'KeyQ', R: 'KeyE',
  CU: 'KeyI', CD: 'KeyK', CL: 'KeyJ', CR: 'KeyL', DU: 'KeyT', DD: 'KeyG', DL: 'KeyF', DR: 'KeyH', WALK: 'ShiftLeft', FF: 'Tab', PAUSE: 'KeyP' };
const DEFAULT_SETTINGS = () => ({ renderer: 'auto', scale: 0, vif: true, filter: 1, aspect: 0, readback: true, hud: false, cpi: 4, pak: 1, vol: 80, mute: false, touch: 'auto', dpad: true, top: 62, tsize: 100, tlift: 50, zr: false, xpak: 'auto', keep: true,
  keys: { ...DEFAULT_KEYS } });
const settings = Object.assign(DEFAULT_SETTINGS(), loadJSON('photon64') || {});
settings.keys = Object.assign({ ...DEFAULT_KEYS }, settings.keys || {});
if (settings.padv !== 2) Object.assign(settings, { padv: 2, dpad: true, tsize: 100, tlift: 50 });   // the pad was redesigned: start from its defaults
function loadJSON(k) { try { return JSON.parse(localStorage.getItem(k)); } catch (e) { return null; } }
function saveSettings() { try { localStorage.setItem('photon64', JSON.stringify(settings)); } catch (e) { /* private mode */ } }

let ex = null, hi = null, wasmBytes = null, wasmHash = 0, gpu = null, gpuErr = '', useGpu = false, romPtr = 0, romSize = 0;
let rom = null;          // { name, id, key, saveType }
let running = false, paused = false, fastForward = false, ffLock = false, hz = 60;
const isTouch = matchMedia('(pointer: coarse)').matches || 'ontouchstart' in window;

// ---------------------------------------------------------------------------------------------- core
async function initCore() {
  const bin = atob(WASM_B64);
  wasmBytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) wasmBytes[i] = bin.charCodeAt(i);
  let h = 2166136261; for (let i = 0; i < wasmBytes.length; i++) h = Math.imul(h ^ wasmBytes[i], 16777619);
  wasmHash = h >>> 0;
  const { instance } = await WebAssembly.instantiate(wasmBytes, { env: {
    host_log: (p, a, b) => { const u8 = new Uint8Array(ex.memory.buffer, p, 96); let s = ''; for (const c of u8) { if (!c) break; s += String.fromCharCode(c); } console.log('[core]', s, a, b); },
    host_gpu_flush: () => { if (gpu) gpu.flush(); },
  } });
  ex = instance.exports;
  hi = Array.from(new Uint32Array(ex.memory.buffer, ex.n64_host_info(), 24));
}
let _mbuf = null, _u8 = null, _u32 = null;   // cached memory views (a grown buffer detaches the old one)
function _views() { const b = ex.memory.buffer; if (b !== _mbuf) { _mbuf = b; _u8 = new Uint8Array(b); _u32 = new Uint32Array(b); } }
const u8v = () => { _views(); return _u8; };
const u32v = () => { _views(); return _u32; };

let gpuStarting = false;       // WebGPU pipelines are still being built (the game runs in software until they are ready)
// A readable name for the graphics adapter. Browsers say little here on purpose: Safari on an iPhone gives the vendor
// "apple" and the architecture "apple" and nothing else, others give a vendor and a family ("intel", "gen-12lp").
function gpuName(info) {
  const VENDOR = { apple: 'Apple', nvidia: 'NVIDIA', amd: 'AMD', intel: 'Intel', qualcomm: 'Qualcomm', arm: 'Arm', google: 'Google', samsung: 'Samsung', imgtec: 'Imagination', microsoft: 'Microsoft' };
  const clean = v => String(v || '').trim();
  const vendor = clean(info.vendor), words = [], d = clean(info.description);
  if (d && ![vendor, clean(info.architecture)].some(x => x.toLowerCase() === d.toLowerCase()) && !VENDOR[d.toLowerCase()]) return d.slice(0, 48);   // a real name, not the vendor again
  for (const w of [vendor, clean(info.architecture), clean(info.device)]) if (w && !/^0x/i.test(w) && !words.some(x => x.toLowerCase() === w.toLowerCase())) words.push(w);
  if (!words.length && d) words.push(d);
  if (!words.length) return 'WebGPU';
  words[0] = VENDOR[words[0].toLowerCase()] || words[0];
  return words.length === 1 ? words[0] + ' GPU' : words.join(' ');
}
async function initGpu() {
  gpuStarting = !!navigator.gpu;
  try {
    gpu = await N64Gpu.create({ ex }, $('cv-gpu'), Object.assign({ allowFallback: /[?&#]gpu=force/.test(location.href) }, SHADERS));
    if (!gpu) { gpuErr = navigator.gpu ? 'no suitable adapter' : 'WebGPU not available in this browser'; return; }
    gpu.onLost = info => { gpuErr = 'device lost'; rendererFailure(new Error(info.message || gpuErr)); updatePills(); };
    let desc = 'WebGPU';
    try { desc = gpuName(gpu.adapter.info || {}); } catch (e) { /* older browsers */ }
    gpu.desc = desc;
  } catch (e) { console.error(e); gpu = null; gpuErr = String(e.message || e).slice(0, 160); }
}

function applyRenderer() {
  if (busy) { busy.then(applyRenderer, rendererFailure); return; }      // (a field is waiting for GPU results: switch afterwards)
  try {
  const want = settings.renderer !== 'sw' && !!gpu;
  $('cv-gpu').hidden = !want; $('cv-sw').hidden = want;
  if (want) {
    gpu.filter = +settings.filter; gpu.stretch = !!+settings.aspect;
    if (!useGpu && ex) { gpu.exact = !!settings.readback; gpu.reset(); } else if (ex && gpu.exact !== !!settings.readback) gpu.setExact(!!settings.readback);
  } else if (useGpu && ex) {
    // leaving GPU mode: finish pending work there, copy the results back, then render in software
    ex.n64_config(6, 0);
    if (gpu) { const p = gpu.syncToCpu().then(() => { ex.n64_config(0, 0); }).catch(rendererFailure).finally(() => { if (busy === p) busy = null; }); busy = p; }
    else { ex.n64_config(0, 0); ex.n64_sync_done(); }
  } else if (ex) ex.n64_config(0, 0);                    // (make sure the core itself is rendering in software)
  useGpu = want;
  $('cv-sw').classList.toggle('px', +settings.filter === 0);
  resize();
  applyScale();
  } catch (error) { rendererFailure(error); }
}

// internal resolution of the GPU renderer (pipelines for 2x / 4x are built on first use)
async function applyScale() {
  const sel = $('s-scale');
  sel.disabled = !gpu; segSync(sel);
  if (!gpu) return;
  for (const o of sel.options) o.disabled = +o.value > gpu.maxScaleLog2;
  segSync(sel);
  const want = useGpu && ex ? Math.min(+settings.scale || 0, gpu.maxScaleLog2) : 0;
  if ((gpu.scaleWant | 0) === want) return;
  gpu.scaleWant = want;
  const g = gpu;
  try {
    if (want) toast(`Preparing ${1 << want}× renderer…`, 2500);
    await g.setScale(want);
    if (want && g.scaleWant === want) toast(`Rendering at ${1 << want}× resolution`);
  } catch (e) {
    console.error(e);
    g.scaleWant = 0; settings.scale = 0; saveSettings(); sel.value = '0'; segSync(sel);
    try { await g.setScale(0); } catch (e2) { /* device lost */ }
    toast('This GPU cannot render at a higher resolution', 4000);
  }
}

// Session operations queue in invocation order. The frame loop stays suspended
// across every await; pause intent remains owned by togglePause, not saved booleans.
let sessionTail = Promise.resolve(), rendererFailed = false;
function sessionOp(fn) {
  const result = sessionTail.then(async () => {
    running = false;
    try { if (busy) await busy; return await fn(); }
    finally { running = !!rom && !rendererFailed; acc = 0; }
  });
  sessionTail = result.catch(() => {});
  return result;
}
function rendererFailure(error) {
  console.error(error); rendererFailed = true; running = false; paused = true;
  toast('Renderer synchronization failed. Export your battery save, then reset or reopen the game.', 10000);
  updateFlag();
}

// ---------------------------------------------------------------------------------------------- ROM
const MAX_ROM = 64 * 1024 * 1024;   // biggest licensed N64 cartridge: 512 Mbit
function checkRomSize(n) { if (n > MAX_ROM) throw new Error('That file is too large to be an N64 ROM'); }
const MAX_ARCHIVE = MAX_ROM + 1024 * 1024;
function validateRom(data) {
  checkRomSize(data.length);
  if (data.length < 0x1000) throw new Error('That does not look like an N64 ROM');
  const magic = new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(0);
  if (![0x80371240, 0x37804012, 0x40123780].includes(magic)) throw new Error('That does not look like an N64 ROM');
  return magic;
}
async function readBounded(stream, limit) {
  const reader = stream.getReader(), chunks = []; let size = 0;
  try {
    for (;;) {
      const { value, done } = await reader.read(); if (done) break;
      if (value.length > limit - size) { await reader.cancel('size limit'); throw new Error('That compressed file is corrupt or too large'); }
      size += value.length; chunks.push(value);
    }
  } catch (e) { await reader.cancel().catch(() => {}); throw e; }
  finally { reader.releaseLock(); }
  const out = new Uint8Array(size); let offset = 0;
  for (const c of chunks) { out.set(c, offset); offset += c.length; }
  return out;
}
function crc32(data) {
  let c = 0xFFFFFFFF;
  for (const b of data) { c ^= b; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ ((c & 1) ? 0xEDB88320 : 0); }
  return (c ^ 0xFFFFFFFF) >>> 0;
}
async function unzip(u8) {
  if (u8.length > MAX_ARCHIVE) throw new Error('That archive is too large');
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const bounds = (p, n) => { if (p < 0 || p > u8.length - n) throw new Error('That zip file is truncated'); };
  let e = u8.length - 22;
  while (e >= 0 && dv.getUint32(e, true) !== 0x06054b50) e--;
  if (e < 0) throw new Error('Not a valid zip file');
  bounds(e, 22);
  if (e + 22 + dv.getUint16(e + 20, true) !== u8.length || dv.getUint16(e + 4, true) || dv.getUint16(e + 6, true) || dv.getUint16(e + 8, true) !== dv.getUint16(e + 10, true)) throw new Error('Not a valid single-disk zip file');
  let p = dv.getUint32(e + 16, true); const n = dv.getUint16(e + 10, true);
  const cdEnd = p + dv.getUint32(e + 12, true); if (cdEnd > e) throw new Error('That zip file is truncated');
  let best = null;
  for (let i = 0; i < n; i++) {
    bounds(p, 46); if (p + 46 > cdEnd || dv.getUint32(p, true) !== 0x02014b50) throw new Error('Not a valid zip file');
    const flags = dv.getUint16(p + 8, true), crc = dv.getUint32(p + 16, true);
    const method = dv.getUint16(p + 10, true), csize = dv.getUint32(p + 20, true), usize = dv.getUint32(p + 24, true);
    const nl = dv.getUint16(p + 28, true), xl = dv.getUint16(p + 30, true), cl = dv.getUint16(p + 32, true), off = dv.getUint32(p + 42, true);
    bounds(p, 46 + nl + xl + cl); if (p + 46 + nl + xl + cl > cdEnd) throw new Error('That zip file is truncated');
    const name = new TextDecoder().decode(u8.subarray(p + 46, p + 46 + nl));
    const good = /\.(z64|n64|v64|rom|bin)$/i.test(name);
    if (usize >= 0x1000 && (!best || (good && !best.good) || (good === best.good && usize > best.usize))) best = { method, csize, usize, off, name, good, flags, crc };
    p += 46 + nl + xl + cl;
  }
  if (!best) throw new Error('No ROM found inside the zip');
  if (best.usize > MAX_ROM) throw new Error('That ROM is too large');
  if (best.flags & ~0x808) throw new Error('Unsupported zip flags');
  const lo = best.off;
  if (lo > u8.length - 30) throw new Error('That zip file is truncated');
  if (dv.getUint32(lo, true) !== 0x04034b50 || dv.getUint16(lo + 8, true) !== best.method || dv.getUint16(lo + 6, true) !== best.flags) throw new Error('Not a valid zip local record');
  const start = lo + 30 + dv.getUint16(lo + 26, true) + dv.getUint16(lo + 28, true);
  if (start > u8.length || best.csize > u8.length - start) throw new Error('That zip file is truncated');
  const data = u8.subarray(start, start + best.csize);
  if (best.method === 0) {
    if (data.length !== best.usize) throw new Error('That zip file is corrupt');
    if (crc32(data) !== best.crc) throw new Error('That zip file is corrupt (CRC)');
    return { data: data.slice(), name: best.name };
  }
  if (best.method !== 8 || !self.DecompressionStream) throw new Error('Unsupported zip compression');
  const out = await readBounded(new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw')), best.usize);
  if (out.byteLength !== best.usize) throw new Error('That zip file is corrupt');
  if (crc32(out) !== best.crc) throw new Error('That zip file is corrupt (CRC)');
  return { data: out, name: best.name };
}

async function openFile(file) {
  try {
    if (file.size > (/\.zip$/i.test(file.name) ? MAX_ARCHIVE : MAX_ROM)) throw new Error('That file is too large');
    let data = new Uint8Array(await file.arrayBuffer()), name = file.name;
    if (data[0] === 0x50 && data[1] === 0x4B) ({ data, name } = await unzip(data));
    await loadRom(data, name);
  } catch (e) { console.error(e); toast(String(e.message || e), 4000); }
}

function loadRom(data, fileName, fromLibrary) {
  validateRom(data);
  const candidate = data.slice();
  return sessionOp(() => loadRomNow(candidate, fileName, fromLibrary));
}
function romIdentity(data, fileName) {
  const magic = validateRom(data);
  const hb = o => data[magic === 0x80371240 ? o : magic === 0x37804012 ? o ^ 1 : o ^ 3];
  let title = ''; for (let i = 0x20; i < 0x34; i++) { const c = hb(i); title += c >= 32 && c < 127 ? String.fromCharCode(c) : ' '; }
  title = title.trim() || fileName.replace(/\.[^.]+$/, '');
  const id3 = String.fromCharCode(hb(0x3B), hb(0x3C), hb(0x3D));
  let crc = ''; for (let i = 0x10; i < 0x18; i++) crc += hb(i).toString(16).padStart(2, '0');
  // shown name: the file name without its extension and dump tags, e.g. "Mario Kart 64 (USA).z64" -> "Mario Kart 64"
  const name = (fileName || '').replace(/\.[^.]+$/, '').replace(/\s*[([].*$/, '').replace(/_/g, ' ').trim() || title;
  return { title, name, file: fileName, id: id3, key: `${title}-${crc}` };
}
async function loadRomNow(data, fileName, fromLibrary) {
  const identity = romIdentity(data, fileName);
  if (rom && !await flushSaves()) throw new Error('Battery save could not be stored. Export it before replacing this game.');
  // A same-cartridge reload must read the save committed by the flush above.
  const sv = await idbGet('save:' + identity.key);
  // One reusable maximum-size cartridge allocation. Validation and allocation
  // failure cannot overwrite the previous game; no unbounded heap growth on loads.
  const ptr = romPtr || ex.n64_alloc(MAX_ROM);
  if (!ptr) throw new Error('Out of memory');
  romGen++;
  romPtr = ptr; romSize = data.length & ~3;
  u8v().set(data.subarray(0, romSize), romPtr);
  ex.n64_config(10, xpakMode());                         // before loading: the machine is built with or without the Expansion Pak
  if (!ex.n64_load(romPtr, romSize)) throw new Error('That does not look like an N64 ROM');
  const m = u8v(), saveType = u32v()[hi[15] >> 2];
  rom = { ...identity, saveType };
  const name = rom.name;
  // battery-backed memory
  m.fill(0, hi[12], hi[12] + 0x800); m.fill(saveType === 4 ? 0xFF : 0, hi[13], hi[13] + 0x20000); m.fill(0, hi[14], hi[14] + 0x20000);
  formatPaks();
  if (sv && sv.byteLength === 0x800 + 0x20000 + 0x20000) restoreSaveBlob(new Uint8Array(sv));
  u32v()[hi[16] >> 2] = 0;
  ex.n64_config(1, +settings.cpi); ex.n64_config(5, +settings.pak); ex.n64_config(9, settings.vif ? 0 : 1);
  rendererFailed = false;
  if (gpuErr === 'device lost') gpu = null;
  useGpu = false; applyRenderer();
  if (rendererFailed) throw new Error('Renderer initialization failed');
  hz = u32v()[hi[22] >> 2] === 0 ? 50 : 60;
  document.title = name + ' — Photon64';
  closeSheet(true); $('home').hidden = true; $('stage').hidden = false;
  updateTouchVisibility(); resize();
  paused = false; ffLock = false; updateFlag();
  menuThumb = '';
  audioStart(); if (actx) actx.resume().catch(() => {}); audioReset();
  acc = 0; wakeLock();
  const xp = updateXpak();
  if (xp.kind >= 2 && xp.kind <= 3 && !xp.now) toast(`${name} ${xp.kind === 3 ? 'needs' : 'needs for some of its content'} the Expansion Pak, which is set to Removed in Settings`, 6000);
  else toast(`${name}${xp.now && xp.kind ? ' · Expansion Pak' : xp.kind === 4 && !xp.now ? ' · Expansion Pak left out' : ''}${useGpu ? '' : gpuStarting && settings.renderer !== 'sw' ? ' · software until WebGPU is ready' : ''}`, gpuStarting ? 4500 : 2200);
  await libAdd(data, fromLibrary, rom, romSize);
  pokeMenu();
}

// Expansion Pak. The core knows from the game ID what each game does with it; "Auto" installs it unless the game is known to
// misbehave with it. Like the real thing it can only be changed with the power off, so a change waits for the next reset.
const xpakMode = () => ({ auto: 0, on: 1, off: 2 })[settings.xpak] || 0;
function updateXpak() {
  const el = $('xpakinfo');
  if (!ex || !rom) { el.textContent = 'Four extra megabytes of memory. Auto installs it unless a game is known to misbehave with it.'; return { kind: 0, now: true, next: true }; }
  const v = ex.n64_xpak(), kind = v & 15, now = !!(v & 0x100), next = !!(v & 0x200);
  el.textContent = ['This game is not known to use it.', 'This game uses it for extras, usually a high-resolution mode you switch on in the game\'s own options.',
    'This game needs it for part of its content.', 'This game requires it.', 'This game is known to misbehave with it.'][kind] +
    (now ? ' Installed.' : ' Not installed.') + (now !== next ? ` Reset the game to ${next ? 'install' : 'remove'} it.` : '');
  return { kind, now, next };
}
function applyXpak() {
  if (!ex) return;
  ex.n64_config(10, xpakMode());
  const x = updateXpak();
  if (rom && x.now !== x.next) toast(`Reset the game to ${x.next ? 'install' : 'remove'} the Expansion Pak`, 3500);
}

// Controller Pak: write an empty, valid file system so games accept it without a "corrupted" prompt.
function formatPaks() {
  const m = u8v();
  for (let c = 0; c < 4; c++) {
    const p = hi[14] + c * 0x8000;
    const idBlock = new Uint8Array(32);
    for (let i = 0; i < 24; i++) idBlock[i] = (i * 37 + c * 11 + 5) & 0xFF;   // serial
    idBlock[25] = 0x01; idBlock[26] = 0x01;                                    // device id, bank count
    let s1 = 0; for (let i = 0; i < 28; i += 2) s1 = (s1 + ((idBlock[i] << 8) | idBlock[i + 1])) & 0xFFFF;
    const s2 = (0xFFF2 - s1) & 0xFFFF;
    idBlock[28] = s1 >> 8; idBlock[29] = s1 & 0xFF; idBlock[30] = s2 >> 8; idBlock[31] = s2 & 0xFF;
    for (const off of [0x20, 0x60, 0x80, 0xC0]) m.set(idBlock, p + off);
    for (const page of [0x100, 0x200]) {
      for (let i = 5; i < 128; i++) { m[p + page + i * 2] = 0; m[p + page + i * 2 + 1] = 3; }
      let ck = 0; for (let i = 10; i < 256; i++) ck = (ck + m[p + page + i]) & 0xFF;
      m[p + page + 1] = ck;
    }
  }
}

// ---------------------------------------------------------------------------------------------- persistence
let idb = null;
function idbOpen() {
  if (idb) return idb;
  idb = new Promise(res => {
    try {
      const r = indexedDB.open('photon64', 1);
      r.onupgradeneeded = () => r.result.createObjectStore('kv');
      r.onsuccess = () => res(r.result); r.onerror = () => res(null); r.onblocked = () => res(null);
    } catch (e) { res(null); }
  });
  return idb;
}
async function idbGet(k) {
  const db = await idbOpen(); if (!db) return memStore.get(k);
  return new Promise((res, reject) => { try { const r = db.transaction('kv').objectStore('kv').get(k); r.onsuccess = () => res(r.result); r.onerror = () => reject(r.error || new Error('Could not read browser storage')); } catch (e) { reject(e); } });
}
let storageWarned = false;
async function idbWrite(entries, deletes = []) {
  const db = await idbOpen();
  if (!db) {
    for (const [k, v] of entries) memStore.set(k, v);
    for (const k of deletes) memStore.delete(k);
    if (!storageWarned) { storageWarned = true; toast('Temporary storage: saves last only until this page closes. Export your battery save.', 10000); }
    return 'temporary';
  }
  return new Promise(res => {
    try {
      const t = db.transaction('kv', 'readwrite'), store = t.objectStore('kv');
      t.oncomplete = () => res(true); t.onerror = t.onabort = () => res(false);
      try { for (const [k, v] of entries) store.put(v, k); for (const k of deletes) store.delete(k); }
      catch (e) { t.abort(); res(false); }
    } catch (e) { res(false); }
  });
}
function idbSet(k, v) { return idbWrite([[k, v]]); }
async function idbHas(k) {
  const db = await idbOpen(); if (!db) return memStore.has(k);
  return new Promise(res => { try { const r = db.transaction('kv').objectStore('kv').count(k); r.onsuccess = () => res(r.result > 0); r.onerror = () => res(false); } catch (e) { res(false); } });
}
function idbDel(k) { return idbWrite([], [k]); }
const memStore = new Map();
function saveBlob() {
  const m = u8v(), out = new Uint8Array(0x800 + 0x20000 + 0x20000);
  out.set(m.subarray(hi[12], hi[12] + 0x800), 0); out.set(m.subarray(hi[13], hi[13] + 0x20000), 0x800); out.set(m.subarray(hi[14], hi[14] + 0x20000), 0x20800);
  return out;
}
function restoreSaveBlob(b) { const m = u8v(); m.set(b.subarray(0, 0x800), hi[12]); m.set(b.subarray(0x800, 0x20800), hi[13]); m.set(b.subarray(0x20800, 0x40800), hi[14]); }
let savePending = null, saveRetryAt = 0;
async function flushSaves(force = true) {
  if (!force && Date.now() < saveRetryAt) return false;
  if (savePending) { const ok = await savePending; if (!ok) return false; return flushSaves(); }
  if (!ex || !rom) return true;
  const generation = u32v()[hi[16] >> 2];
  if (!generation) return true;
  const r = rom, gen = romGen, bytes = saveBlob();
  savePending = (async () => {
    const outcome = await idbSet('save:' + r.key, bytes.buffer);
    if (outcome === true && rom === r && romGen === gen && u32v()[hi[16] >> 2] === generation) u32v()[hi[16] >> 2] = 0;
    if (outcome === false) { saveRetryAt = Date.now() + 5000; toast('Battery save failed. Progress is still in memory; retry or export it from Settings.', 6000); }
    else saveRetryAt = 0;
    return outcome !== false;
  })();
  try { return await savePending; } finally { savePending = null; }
}

// Save states: a compressed image of the core's static memory (everything except the ROM).
async function gz(u8, dir, limit = MAX_ROM) {
  const s = new Blob([u8]).stream().pipeThrough(dir ? new CompressionStream('gzip') : new DecompressionStream('gzip'));
  return readBounded(s, limit);
}
const SLOTS = 4;
const stateKey = (slot, r = rom) => 'state:' + r.key + (slot ? ':' + slot : '');
async function stateMeta(r = rom) {
  const m = (r && await idbGet('smeta:' + r.key)) || [];
  if (r) for (let s = 0; s < SLOTS; s++) if (!m[s] && await idbGet(stateKey(s, r))) m[s] = { t: 0, thumb: '' };
  return m;
}
function saveState(slot = 0, thumb) {
  const r = rom; return sessionOp(() => rom === r && saveStateNow(slot, thumb));
}
async function saveStateNow(slot = 0, thumb) {
  if (!rom || !self.CompressionStream) return toast('Save states are not supported in this browser');
  try {
    if (rendererFailed) throw new Error('Renderer state is inconsistent');
    if (busy) await busy;
    if (thumb === undefined) thumb = await grabThumb();
    if (useGpu) await gpu.syncToCpu();
    const size = ex.n64_state_size();
    const head = new Uint32Array([0x34365350, wasmHash, size, romSize]);
    const z = await gz(u8v().slice(0, size), true);
    const blob = new Uint8Array(16 + z.length); blob.set(new Uint8Array(head.buffer), 0); blob.set(z, 16);
    const meta = await stateMeta(); meta[slot] = { t: Date.now(), thumb: thumb || '' };
    const outcome = await idbWrite([[stateKey(slot), blob.buffer], ['smeta:' + rom.key, meta]]);
    if (outcome === false) throw new Error('State transaction failed');
    toast(outcome === true ? `Saved to slot ${slot + 1}` : `Slot ${slot + 1} saved temporarily — keep this page open`);
    return true;
  } catch (e) { console.error(e); toast('Could not save state'); return false; }
}
function loadState(slot = 0) {
  const r = rom; return sessionOp(() => rom === r && loadStateNow(slot)).catch(e => { console.error(e); toast('Could not load state'); return false; });
}
async function loadStateNow(slot = 0) {
  if (!rom || !self.DecompressionStream) return;
  const buf = await idbGet(stateKey(slot));
  if (!buf) return toast(`Slot ${slot + 1} is empty`);
  if (!(buf instanceof ArrayBuffer) || buf.byteLength < 16 || buf.byteLength > MAX_ROM) return toast('That state is corrupt');
  const head = new Uint32Array(buf.slice(0, 16));
  if (head[0] !== 0x34365350 || head[1] !== wasmHash || head[2] !== ex.n64_state_size() || head[3] !== romSize) return toast('That state was made by a different version of Photon64');
  try {
    if (busy) await busy;
    const raw = await gz(new Uint8Array(buf, 16), false, head[2]);
    if (raw.length !== head[2]) throw new Error('size');
    romGen++;
    u8v().set(raw, 0);
    ex.n64_set_rom(romPtr, romSize);
    ex.n64_config(1, +settings.cpi); ex.n64_config(5, +settings.pak); ex.n64_config(9, settings.vif ? 0 : 1);
    ex.n64_config(10, xpakMode()); updateXpak();           // (the restored machine keeps the memory it was saved with until it is reset)
    useGpu = false; ex.n64_config(0, 0); applyRenderer();
    audioReset();
    toast(`Loaded slot ${slot + 1}`); return true;
  } catch (e) { console.error(e); toast('Could not load state'); return false; }
}
// A small picture of what is on screen now (for the library and the state slots).
let thumbDark = false;
async function grabThumb() {
  try {
    const c = document.createElement('canvas'); c.width = 320; c.height = 240;
    const g = c.getContext('2d');
    if (useGpu && gpu) {
      const o = await gpu.readOutput(), t = document.createElement('canvas');
      t.width = o.width; t.height = o.height;
      t.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(o.data.buffer, o.data.byteOffset, o.width * o.height * 4), o.width, o.height), 0, 0);
      g.drawImage(t, 0, 0, 320, 240);
    } else { const sw = $('cv-sw'); if (!sw.width) return ''; g.drawImage(sw, 0, 0, 320, 240); }
    const d = g.getImageData(0, 0, 320, 240).data; let sum = 0;
    for (let i = 0; i < d.length; i += 64) sum += d[i] + d[i + 1] + d[i + 2];
    thumbDark = sum / (d.length / 64) < 18;
    return c.toDataURL('image/jpeg', 0.82);
  } catch (e) { console.warn(e); return ''; }
}

// ---------------------------------------------------------------------------------------------- audio
class Resampler {
  constructor(outRate) {
    this.size = 16384; this.l = new Float32Array(this.size); this.r = new Float32Array(this.size);
    this.wr = 0; this.rd = 0; this.frac = 0; this.srcRate = 32000; this.outRate = outRate; this.started = false; this.avg = 0; this.hl = 0; this.hr = 0;
  }
  push(s, rate) {
    if (rate && rate !== this.srcRate) { this.srcRate = rate; }
    const m = this.size - 1;
    for (let i = 0; i < s.length; i += 2) { const k = this.wr & m; this.l[k] = s[i] / 32768; this.r[k] = s[i + 1] / 32768; this.wr++; }
    if (this.wr - this.rd > this.size - 512) this.rd = this.wr - (this.size >> 2);
  }
  reset() { this.rd = this.wr; this.started = false; this.frac = 0; }
  pull(L, R) {
    const n = L.length, m = this.size - 1, target = this.srcRate * 0.055;
    let avail = this.wr - this.rd;
    if (!this.started) {
      if (avail < target) { for (let i = 0; i < n; i++) { this.hl *= 0.995; this.hr *= 0.995; L[i] = this.hl; R[i] = this.hr; } return; }
      this.started = true; this.avg = avail;
    }
    // dynamic rate control: keep the queue near the target so the emulator's frame pacing never has to chase the audio clock
    this.avg += (avail - this.avg) * 0.02;
    const err = (this.avg - target) / target;
    const step = (this.srcRate / this.outRate) * (1 + Math.max(-0.02, Math.min(0.02, err * 0.04)));
    let frac = this.frac, rd = this.rd, i = 0;
    for (; i < n; i++) {
      if (this.wr - rd < 2) break;
      const a = rd & m, b = (rd + 1) & m;
      L[i] = this.hl = this.l[a] + (this.l[b] - this.l[a]) * frac;
      R[i] = this.hr = this.r[a] + (this.r[b] - this.r[a]) * frac;
      frac += step; const k = frac | 0; rd += k; frac -= k;
    }
    if (i < n) { this.started = false; for (; i < n; i++) { this.hl *= 0.995; this.hr *= 0.995; L[i] = this.hl; R[i] = this.hr; } }
    this.frac = frac; this.rd = Math.min(rd, this.wr);
  }
}
let actx = null, anode = null, again = null, aLocal = null, lastWr = 0, audioReady = false;
async function audioStart() {
  if (actx) { if (actx.state !== 'running') actx.resume().catch(() => {}); return; }
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return;
  try {
    actx = new AC({ latencyHint: 'interactive' });
    again = actx.createGain(); again.connect(actx.destination); setVolume();
    if (actx.audioWorklet) {
      try {
        const src = `${Resampler.toString()}
          registerProcessor('n64', class extends AudioWorkletProcessor {
            constructor() { super(); this.rs = new Resampler(sampleRate); this.port.onmessage = e => { const d = e.data; if (d.reset) this.rs.reset(); if (d.s) this.rs.push(d.s, d.rate); }; }
            process(_, out) { const o = out[0]; this.rs.pull(o[0], o[1] || o[0]); return true; }
          });`;
        // data: URLs work for worklets even from file:// pages (where blob: modules can be refused); blob: is the fallback
        try { await actx.audioWorklet.addModule('data:text/javascript;base64,' + btoa(src)); }
        catch (e1) { await actx.audioWorklet.addModule(URL.createObjectURL(new Blob([src], { type: 'text/javascript' }))); }
        anode = new AudioWorkletNode(actx, 'n64', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
        anode.connect(again);
      } catch (e) { console.warn('AudioWorklet unavailable, using ScriptProcessor:', e && e.message); anode = null; }
    }
    if (!anode) {   // fallback for browsers without AudioWorklet (or where blob modules are blocked)
      aLocal = new Resampler(actx.sampleRate);
      const sp = actx.createScriptProcessor(1024, 0, 2);
      sp.onaudioprocess = e => aLocal.pull(e.outputBuffer.getChannelData(0), e.outputBuffer.getChannelData(1));
      sp.connect(again); anode = sp;
    }
    audioReady = true;
    actx.resume().catch(() => {});
  } catch (e) { console.warn('audio unavailable', e); }
}
function setVolume() { if (again) again.gain.value = settings.mute ? 0 : Math.pow(settings.vol / 100, 2); updateMenuTiles(); }
function audioReset() { if (ex) lastWr = u32v()[hi[4] >> 2]; if (aLocal) aLocal.reset(); else if (anode && anode.port) anode.port.postMessage({ reset: 1 }); }
function audioPump(drop) {
  const u = u32v(), wr = u[hi[4] >> 2];
  let n = (wr - lastWr) >>> 0;
  if (!n) return;
  if (n > 16384) { lastWr = wr - 16384; n = 16384; }
  if (!audioReady || drop) { lastWr = wr; return; }
  const ring = new Int16Array(ex.memory.buffer, hi[3], 65536), s = new Int16Array(n * 2);
  const a = (lastWr & 32767) * 2, first = Math.min(n * 2, 65536 - a);
  s.set(ring.subarray(a, a + first)); if (first < n * 2) s.set(ring.subarray(0, n * 2 - first), first);
  lastWr = wr;
  const rate = u[hi[5] >> 2] || 32000;
  if (aLocal) aLocal.push(s, rate); else anode.port.postMessage({ s, rate }, [s.buffer]);
}

// ---------------------------------------------------------------------------------------------- input
const keysDown = new Set();
const touchState = { buttons: 0, x: 0, y: 0 };
let rebinding = null, rumbleOn = false;
function readInput() {
  let b = touchState.buttons, x = touchState.x, y = touchState.y;
  const k = settings.keys, d = a => keysDown.has(k[a]);
  for (const name in BTN) if (d(name)) b |= BTN[name];
  let kx = (d('RIGHT') ? 1 : 0) - (d('LEFT') ? 1 : 0), ky = (d('UP') ? 1 : 0) - (d('DOWN') ? 1 : 0);
  if (kx || ky) { const mag = (d('WALK') ? 34 : 80) * (kx && ky ? 0.85 : 1); x = kx * mag; y = ky * mag; }
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (const gp of pads) {
    if (!gp || !gp.connected) continue;
    const B = i => gp.buttons[i] && (gp.buttons[i].pressed || gp.buttons[i].value > 0.4);
    if (B(0)) b |= BTN.A; if (B(2) || B(1)) b |= BTN.B; if (B(6) || B(7)) b |= BTN.Z; if (B(9)) b |= BTN.START;
    if (B(4)) b |= BTN.L; if (B(5)) b |= BTN.R; if (B(12)) b |= BTN.DU; if (B(13)) b |= BTN.DD; if (B(14)) b |= BTN.DL; if (B(15)) b |= BTN.DR;
    if (B(3)) b |= BTN.CL;
    const ax = gp.axes;
    if (ax.length >= 2) {
      let sx = ax[0], sy = -ax[1]; const m = Math.hypot(sx, sy);
      if (m > 0.14) { const s = Math.min(1, (m - 0.14) / 0.78) / m; x = sx * s * 80; y = sy * s * 80; }
    }
    if (ax.length >= 4) { if (ax[2] > 0.5) b |= BTN.CR; if (ax[2] < -0.5) b |= BTN.CL; if (ax[3] > 0.5) b |= BTN.CD; if (ax[3] < -0.5) b |= BTN.CU; }
    if (rumbleOn && gp.vibrationActuator && gp.vibrationActuator.playEffect) gp.vibrationActuator.playEffect('dual-rumble', { duration: 60, strongMagnitude: 0.8, weakMagnitude: 0.5 }).catch(() => {});
    break;
  }
  // real sticks top out near 80 on the axes and ~68 on the diagonals
  x = Math.max(-80, Math.min(80, x)); y = Math.max(-80, Math.min(80, y));
  const ad = Math.abs(x) + Math.abs(y); if (ad > 136) { x *= 136 / ad; y *= 136 / ad; }
  ex.n64_input(0, b, Math.round(x), Math.round(y));
}
addEventListener('keydown', e => {
  if (rebinding) { e.preventDefault(); if (e.code !== 'Escape') { settings.keys[rebinding] = e.code; saveSettings(); } rebinding = null; buildBinds(); return; }
  if (e.code === 'Escape' && !e.repeat) { e.preventDefault(); if (sheetOpen()) sheetBack(); else if (rom) openMenu(); return; }
  if (sheetOpen() || e.metaKey || e.ctrlKey || e.altKey) return;
  if (!rom) return;
  const k = settings.keys;
  if (e.code === k.PAUSE && !e.repeat) { togglePause(); e.preventDefault(); return; }
  if (e.code === k.FF) { fastForward = true; e.preventDefault(); return; }
  if (e.code === 'F2' && !e.repeat) { saveState(); e.preventDefault(); return; }
  if (e.code === 'F4' && !e.repeat) { loadState(); e.preventDefault(); return; }
  if (Object.values(k).includes(e.code)) { keysDown.add(e.code); e.preventDefault(); audioStart(); }
});
addEventListener('keyup', e => { keysDown.delete(e.code); if (e.code === settings.keys.FF) { fastForward = false; if (!ffLock) audioReset(); } });
function releaseInput() {
  const stage = $('stage');
  for (const id of pointers.keys()) { try { if (stage.hasPointerCapture(id)) stage.releasePointerCapture(id); } catch (e) { /* pointer already ended */ } }
  keysDown.clear(); fastForward = false; pointers.clear(); stickPid = null; touchUpdate();
  if (ex) ex.n64_input(0, 0, 0, 0);
}
addEventListener('blur', releaseInput);

// ---- touch pad layout ----
// Nothing about the pad is fixed in CSS: every control is placed here from the real viewport and its safe area, in one unit u
// (CSS px; the stick is 30 u across). The arrangement is the familiar one for this controller on a touch screen: two columns,
// left hand L / stick / D-pad, right hand R / A-B-Z triangle / C buttons. On an upright screen the picture sits at the top and
// the columns share the room below it, START between them. On a wide screen each hand gets a bottom corner: stick and face
// buttons at the edge, D-pad and C buttons below and inwards, where they may lie on the corners of the picture.
const PAD = { stick: 30, dpad: 29, arm: 9.8, face: 14, plate: 30, c: 10, cOff: 9.2, lr: [25, 8.25], start: [16, 7], menu: 8.5,
  tall: 81, wide: 67 };         // height of the whole block in each arrangement, 1.5 u clear above and below included
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
let insetProbe = null, insetOverride = null, padInfo = null;
{ const q = new URLSearchParams(location.search).get('insets'); if (q) insetOverride = q.split(',').map(v => +v || 0); }
function safeInsets() {               // [top, right, bottom, left] in CSS px: what to keep clear of at each edge of the page
  const r = rawInsets();
  return hostFit.active ? [hostFit.s, r[1], hostFit.R - hostFit.s, r[3]] : r;
}
// ---- a page shown through a window smaller than itself ----
// An app that embeds this page in a web view may give that view's scroller content insets (room for the status bar above,
// for its own buttons below). WebKit does not count such insets as covering the page: the page is still laid out at the
// full height of the view and is then shown shifted down by the top inset, free to slide by the sum of the two - and it is
// still told the safe area, so it pads for the status bar a second time. A page cannot make itself shorter than the view
// (the document is never shorter than its viewport), so instead the layout follows the window that is really shown:
// with the page moved s from its uppermost position and R the distance it can move, the part in view starts s below the
// top of the page and ends R - s above its bottom. Those two take the place of the safe area, and drags are stopped.
//   The signs of such a host: a page that cannot scroll (html and body clip) resting below zero - by the top inset - or
// found resting at a positive scroll position. Until the far end has been seen, the bottom inset is taken to be the safe
// area the page is told about, which is what such insets are there to clear.
const hostFit = { active: false, s: 0, R: 0, rest: 0, max: 0, pos: false, last: 0, n: 0, touch: 0 };
function hostLayout(sy = window.scrollY) {
  const [t, , b] = rawInsets();
  const R = Math.max(hostFit.rest < -2 ? b - hostFit.rest : hostFit.pos ? t + b : 0, hostFit.max);
  const active = R > 2, sNow = active ? clamp(sy - hostFit.rest, 0, R) : 0;
  if (active === hostFit.active && Math.abs(R - hostFit.R) < 0.5 && Math.abs(sNow - hostFit.s) < 0.5) return false;
  hostFit.active = active; hostFit.R = R; hostFit.s = sNow;
  const st = document.documentElement.style;
  if (active) { st.setProperty('--st', sNow + 'px'); st.setProperty('--sb', R - sNow + 'px'); } else { st.removeProperty('--st'); st.removeProperty('--sb'); }
  return true;
}
function hostSample(sy = window.scrollY) {          // called a few times a second; acts on positions the page rests at
  if (hostFit.touch === true || performance.now() - hostFit.touch < 500) { hostFit.n = 0; return false; }   // a finger may be pulling the page about
  if (Math.abs(sy - hostFit.last) > 1) { hostFit.last = sy; hostFit.n = 1; return false; }                  // still moving
  if (++hostFit.n < 2) return false;
  if (sy < hostFit.rest) hostFit.rest = sy;
  if (sy > 2) hostFit.pos = true;
  hostFit.max = Math.max(hostFit.max, sy - hostFit.rest);
  return hostLayout(sy);
}
function hostReset() { hostFit.rest = 0; hostFit.max = 0; hostFit.pos = false; hostFit.n = 0; hostLayout(); }      // the window changed: measure afresh
function hostWatch() {
  addEventListener('pointerdown', () => { hostFit.touch = true; }, { capture: true, passive: true });
  for (const ev of ['pointerup', 'pointercancel']) addEventListener(ev, () => { hostFit.touch = performance.now(); }, { capture: true, passive: true });
  setInterval(() => { if (hostSample()) { resize(); dispInfo(); } }, 250);
  addEventListener('scroll', () => { if (hostFit.active && hostLayout()) resize(); }, { passive: true });   // keep to the window while the page moves
  // once the window is known, a drag that nothing on the page can use must not move the page
  addEventListener('touchmove', e => {
    if (!hostFit.active || !e.cancelable) return;
    if (e.touches.length < 2) for (let el = e.target; el && el.nodeType === 1 && el !== document.body; el = el.parentElement) {
      if (el.matches('input[type=range]')) return;
      const o = getComputedStyle(el).overflowY;
      if ((o === 'auto' || o === 'scroll') && el.scrollHeight > el.clientHeight + 1) return;
    }
    e.preventDefault();
  }, { passive: false });
}
// What the browser tells the page about the window it is shown in - for working out why a layout does not fit.
let dispSeen = null;
function dispInfo() {
  const el = $('dispinfo'); if (!el || !$('dispbox').open) return;
  const vv = self.visualViewport, de = document.documentElement, b = document.body.getBoundingClientRect(), f = v => Math.round(v * 10) / 10;
  let frame = 'top level'; try { if (self.top !== self) frame = 'embedded'; } catch (e) { frame = 'embedded (other origin)'; }
  const meta = document.querySelector('meta[name=viewport]');
  el.textContent = [
    `window ${innerWidth} x ${innerHeight}   outer ${outerWidth} x ${outerHeight}`,
    `screen ${screen.width} x ${screen.height} @${f(devicePixelRatio)}   avail ${screen.availWidth} x ${screen.availHeight}`,
    vv ? `visual ${f(vv.width)} x ${f(vv.height)}  offset ${f(vv.offsetLeft)},${f(vv.offsetTop)}  page ${f(vv.pageLeft)},${f(vv.pageTop)}  scale ${f(vv.scale)}` : 'visual -',
    `root ${de.clientWidth} x ${de.clientHeight}   body ${f(b.width)} x ${f(b.height)} at ${f(b.left)},${f(b.top)}`,
    `scroll ${f(scrollX)},${f(scrollY)}   doc ${de.scrollWidth} x ${de.scrollHeight}`,
    `safe area (env) ${rawInsets().map(f).join(' ')}  [top right bottom left]`,
    `used ${safeInsets().map(f).join(' ')}   window fit ${hostFit.active ? 'on' : 'off'}  s ${f(hostFit.s)}  R ${f(hostFit.R)}  rest ${f(hostFit.rest)}  max ${f(hostFit.max)}`,
    `frame ${frame}   origin ${location.origin}   ${location.protocol}`,
    `standalone ${navigator.standalone === undefined ? '-' : navigator.standalone}   touch ${navigator.maxTouchPoints}   ${matchMedia('(display-mode: standalone)').matches ? 'display-mode standalone' : 'display-mode browser'}`,
    `visible part of page ${dispSeen ? dispSeen : '(measuring)'}`,
    `viewport meta: ${meta ? meta.content : '-'}`,
    navigator.userAgent,
  ].join('\n');
}
function dispWatch() {
  $('dispbox').addEventListener('toggle', dispInfo);
  for (const ev of ['scroll', 'resize']) addEventListener(ev, dispInfo, { passive: true });
  if (self.IntersectionObserver) try {     // seen from the top of the window stack, even from inside a frame
    new IntersectionObserver(es => { const r = es[es.length - 1], a = r.intersectionRect, t = r.boundingClientRect, f = v => Math.round(v);
      dispSeen = `x ${f(a.left - t.left)}..${f(a.right - t.left)}  y ${f(a.top - t.top)}..${f(a.bottom - t.top)}  of ${f(t.width)} x ${f(t.height)}` + (r.rootBounds ? `  root ${f(r.rootBounds.width)} x ${f(r.rootBounds.height)}` : '  root hidden'); dispInfo(); },
      { threshold: Array.from({ length: 101 }, (_, i) => i / 100) }).observe(document.body);
  } catch (e) { dispSeen = 'not available'; }
}
function rawInsets() {
  if (insetOverride) return insetOverride;
  if (!insetProbe) {
    insetProbe = document.createElement('div');
    insetProbe.style.cssText = 'position:fixed;left:0;top:0;width:0;height:0;visibility:hidden;pointer-events:none;' +
      'padding:env(safe-area-inset-top,0px) env(safe-area-inset-right,0px) env(safe-area-inset-bottom,0px) env(safe-area-inset-left,0px)';
    document.body.appendChild(insetProbe);
  }
  const c = getComputedStyle(insetProbe);
  return [c.paddingTop, c.paddingRight, c.paddingBottom, c.paddingLeft].map(v => parseFloat(v) || 0);
}
function buildPad() {              // (re)draw the artwork; which of Z and R is the face button is a setting
  const pad = $('touch'), q = b => pad.querySelector(`.t[data-b=${b}]`);
  if (!$('stage').querySelector('#pg-a')) $('stage').insertAdjacentHTML('afterbegin', PadArt.defs());
  $('stick').innerHTML = PadArt.stick() + '<div id="nub">' + PadArt.nub() + '</div>';
  $('dpad').innerHTML = PadArt.dpad(); $('cplate').innerHTML = PadArt.cluster();
  q('A').innerHTML = PadArt.face('a', 'A'); q('B').innerHTML = PadArt.face('b', 'B');
  const [f, sh] = settings.zr ? ['R', 'Z'] : ['Z', 'R'];
  q(f).innerHTML = PadArt.face('n', f); q(sh).innerHTML = PadArt.bar('n', 303, 'r', sh, 50);
  q('L').innerHTML = PadArt.bar('n', 303, 'l', 'L', 50);
  for (const [b, r] of [['CU', 0], ['CR', 90], ['CD', 180], ['CL', 270]]) q(b).innerHTML = PadArt.cButton(r);
  q('START').innerHTML = PadArt.bar('s', 229, '', 'START', 31, 2.5);
  $('b-menu').innerHTML = PadArt.menu();
}
function layoutControls() {
  const stage = $('stage'), scr = $('screen'), hud = $('hud'), pad = $('touch'), menu = $('b-menu');
  const W = stage.clientWidth, H = stage.clientHeight;
  if (!W || !H) return;
  const [it, ir, ib, il] = safeInsets();
  const wS = Math.max(120, W - il - ir), hS = Math.max(120, H - it - ib), show = !pad.hidden;
  const size = clamp(+settings.tsize || 100, 50, 150) / 100;
  const uT = clamp(4 + (Math.min(W, H) - 430) * 0.0036, 4, 5.3) * size;        // preferred unit: 4 px on phones, a little more on tablets
  const lift = clamp(settings.tlift == null ? 50 : +settings.tlift, 0, 100) / 50;
  const place = (free, k) => (lift <= 1 ? k * lift : k + (1 - k) * (lift - 1)) * Math.max(0, free);
  let u = 0, stacked = false, picTop = 0, picH = H, cyb = 0;
  if (show && H >= W * 1.05) {
    // upright: picture, then the pad. The picture gives up to 38 % of its height before the pad starts to shrink.
    picTop = Math.round(it);
    const room = H - ib - picTop, full = W * 0.75;
    picH = Math.round(clamp(room - PAD.tall * uT, full * 0.62, full));
    u = Math.min(uT, wS / 72, (room - picH) / PAD.tall);
    if (u >= 2.9 * Math.min(1, size)) { stacked = true; cyb = H - ib - 1.5 * u - place(room - picH - PAD.tall * u, 0.6); }
    else { picTop = 0; picH = H; }
  }
  const picW = stacked || +settings.aspect ? W : Math.min(W, H * 4 / 3), pillar = (W - picW) / 2;
  if (show && !stacked) {
    // wide (or too little room to stack): the picture keeps the whole screen
    u = clamp((pillar - Math.max(il, ir) - 6) / 32.5, 0.9 * uT, uT);             // lean towards keeping the outer column off the picture
    u = Math.max(1.5, Math.min(u, (wS - 12) / 119, hS / PAD.wide));
    cyb = H - ib - 1.5 * u - place(hS - PAD.wide * u, 0.1);
  }
  // picture area
  scr.style.flex = stacked ? '0 0 auto' : ''; scr.style.height = stacked ? picH + 'px' : ''; scr.style.marginTop = stacked ? picTop + 'px' : '';
  document.body.classList.toggle('stacked', stacked);
  // performance readout, messages, and - without the pad - the menu button in the top corner
  hud.style.left = (show ? Math.max(6, il + 4) : Math.max(8, il) + 52) + 'px'; hud.style.top = (stacked ? picTop + 6 : Math.max(show ? 6 : 16, it + 4)) + 'px';
  $('toast').style.bottom = !stage.hidden && stacked ? H - picTop - picH + 10 + 'px' : (show && !stage.hidden ? Math.max(ib + 8, H * 0.09) + 'px' : '');
  padInfo = { W, H, insets: [it, ir, ib, il], u, stacked, picTop, picH };
  if (!show) { const st = menu.style; st.left = Math.max(10, il + 6) + 'px'; st.top = Math.max(10, it + 6) + 'px'; st.width = st.height = '40px'; st.opacity = ''; return; }
  if (!show) return;

  // the controls. A control lying on the picture is drawn see-through (the opacity setting), the others solid.
  const picY = stacked ? picTop : (H - Math.min(H, +settings.aspect ? H : W * 0.75)) / 2, picB = stacked ? picTop + picH : H - picY;
  const onPic = (x, y, w, h) => !stacked && Math.max(0, Math.min(x + w / 2, pillar + picW) - Math.max(x - w / 2, pillar)) * Math.max(0, Math.min(y + h / 2, picB) - Math.max(y - h / 2, picY)) > 0.25 * w * h;
  const q = b => pad.querySelector(`.t[data-b=${b}]`), ghost = settings.top / 100;
  const put = (el, x, y, w, h = w, group) => {          // centre and size in px; returns whether it lies on the picture
    const g = group === undefined ? onPic(x, y, w, h) : group, st = el.style;
    st.left = x - w / 2 + 'px'; st.top = y - h / 2 + 'px'; st.width = w + 'px'; st.height = h + 'px'; st.opacity = g ? ghost : '';
    return g;
  };
  const ms = Math.max(u, 6), face3 = q(settings.zr ? 'R' : 'Z'), shR = q(settings.zr ? 'Z' : 'R');
  let xL, xR, yS, yD, yLR, sL, xLp, xRp;                // column centres, row heights (px), shoulder shift, D-pad / C plate centres
  if (stacked) {
    const c = clamp(0.2463 * wS / u, 18, 24.75) * u;
    xL = xLp = il + c; xR = xRp = W - ir - c; yD = cyb - 15 * u; yS = cyb - 50.25 * u; yLR = cyb - 73.875 * u; sL = Math.min(4 * u, c - 14.5 * u);
    put(q('START'), il + wS / 2, cyb - 5 * u, PAD.start[0] * u, PAD.start[1] * u);
    put(menu, il + wS / 2, yLR, Math.max(PAD.menu * u, 32));
  } else {
    const X0 = il + ms, X1 = W - ir - ms;
    xL = X0 + 15 * u; xR = X1 - 16.25 * u; xLp = X0 + 41.5 * u; xRp = X1 - 42.5 * u; yD = cyb - 15 * u; yS = cyb - 36 * u; yLR = cyb - 59.625 * u; sL = 2.5 * u;
    put(q('START'), X1 - 8 * u, cyb - 3.5 * u, PAD.start[0] * u, PAD.start[1] * u);
    const m = Math.max(PAD.menu * u, 32); put(menu, X0 + m / 2, cyb - m / 2, m);
  }
  put($('stick'), xL, yS, PAD.stick * u);
  put(q('L'), xL - sL, yLR, PAD.lr[0] * u, PAD.lr[1] * u); put(shR, (stacked ? xR : xR + 1.25 * u) + sL, yLR, PAD.lr[0] * u, PAD.lr[1] * u);
  const gf = onPic(xR, yS, 32.5 * u, 32.5 * u);              // the triangle is judged as one, so its three buttons always look alike
  put(q('B'), xR - 9 * u, yS - 4.25 * u, PAD.face * u, PAD.face * u, gf); put(face3, xR + 9.25 * u, yS - 9 * u, PAD.face * u, PAD.face * u, gf); put(q('A'), xR + 4.25 * u, yS + 9.25 * u, PAD.face * u, PAD.face * u, gf);
  const gd = put($('dpad'), xLp, yD + 0.5 * u, PAD.dpad * u), ao = (PAD.dpad - PAD.arm) / 2 * u;
  for (const [b, dx, dy] of [['DU', 0, -1], ['DD', 0, 1], ['DL', -1, 0], ['DR', 1, 0]]) put(q(b), xLp + dx * ao, yD + 0.5 * u + dy * ao, PAD.arm * u, PAD.arm * u, gd);
  const gc = put($('cplate'), xRp, yD, PAD.plate * u);
  for (const [b, dx, dy] of [['CU', 0, -1], ['CD', 0, 1], ['CL', -1, 0], ['CR', 1, 0]]) put(q(b), xRp + dx * PAD.cOff * u, yD + dy * PAD.cOff * u, PAD.c * u, PAD.c * u, gc);
}
// every pointer presses whatever control it is over (slightly enlarged hit areas let one thumb hold two buttons)
const touchEls = [], pointers = new Map();
let stickPid = null;               // id of the pointer holding the stick. Ids are arbitrary integers (iOS hands out large and negative ones), so no value can mean "none"
function touchLayout() {
  layoutControls();
  touchEls.length = 0;
  for (const el of document.querySelectorAll('#touch .t[data-b]')) {
    if (el.offsetParent === null) continue;
    const r = el.getBoundingClientRect();
    touchEls.push({ el, mask: BTN[el.dataset.b], cx: r.left + r.width / 2, cy: r.top + r.height / 2, rx: r.width / 2 * 1.22 + 4, ry: r.height / 2 * 1.22 + 4 });
  }
}
function touchUpdate() {
  let b = 0;
  for (const t of touchEls) {
    let on = false;
    for (const [id, p] of pointers) { if (id === stickPid) continue; const dx = (p.x - t.cx) / t.rx, dy = (p.y - t.cy) / t.ry; if (dx * dx + dy * dy <= 1) { on = true; break; } }
    t.el.classList.toggle('down', on);
    if (on) b |= t.mask;
  }
  touchState.buttons = b;
  const st = $('stick'), nub = $('nub');
  if (stickPid !== null && pointers.has(stickPid)) {
    const r = st.getBoundingClientRect(), p = pointers.get(stickPid), R = r.width / 2;
    let dx = (p.x - (r.left + R)) / (R * 0.72), dy = (p.y - (r.top + R)) / (R * 0.72);
    const m = Math.hypot(dx, dy); if (m > 1) { dx /= m; dy /= m; }
    const dz = 0.08, mm = Math.hypot(dx, dy), s = mm > dz ? (mm - dz) / (1 - dz) / mm : 0;
    touchState.x = dx * s * 80; touchState.y = -dy * s * 80;
    nub.style.transform = `translate(${dx * R * 0.5}px, ${dy * R * 0.5}px)`; st.classList.add('down');
  } else { touchState.x = touchState.y = 0; nub.style.transform = ''; st.classList.remove('down'); }
}
function initTouch() {
  const stage = $('stage');
  const inStick = (x, y) => {      // the stick also catches thumbs landing a little outside it, unless they are on a neighbouring button
    const r = $('stick').getBoundingClientRect(), R = r.width / 2, d = Math.hypot(x - r.left - R, y - r.top - R);
    return d < R || (d < R * 1.25 && !touchEls.some(t => ((x - t.cx) / t.rx) ** 2 + ((y - t.cy) / t.ry) ** 2 <= 1));
  };
  const chrome = e => e.target.closest('#b-menu');
  const down = e => {
    if ($('touch').hidden || e.pointerType === 'mouse' || chrome(e) || sheetOpen()) return;
    audioStart();
    if (!touchEls.length) touchLayout();
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    try { stage.setPointerCapture(e.pointerId); } catch (e) { /* synthetic pointer or a pointer that already ended */ }
    if ((stickPid === null || !pointers.has(stickPid)) && inStick(e.clientX, e.clientY)) stickPid = e.pointerId;
    touchUpdate(); e.preventDefault();
  };
  const move = e => { const p = pointers.get(e.pointerId); if (!p) return; p.x = e.clientX; p.y = e.clientY; touchUpdate(); e.preventDefault(); };
  const up = e => { if (!pointers.delete(e.pointerId)) return; if (e.pointerId === stickPid) stickPid = null; touchUpdate(); };
  stage.addEventListener('pointerdown', down, { passive: false });
  stage.addEventListener('pointermove', move, { passive: false });
  for (const t of ['pointerup', 'pointercancel', 'lostpointercapture']) stage.addEventListener(t, up);
  stage.addEventListener('touchstart', e => { if (!$('touch').hidden && !chrome(e)) e.preventDefault(); }, { passive: false });
  stage.addEventListener('contextmenu', e => e.preventDefault());
}
function updateTouchVisibility() {
  const show = settings.touch === 'on' || (settings.touch === 'auto' && isTouch);
  document.body.classList.toggle('touch', show);
  $('touch').hidden = !show;
  for (const el of document.querySelectorAll('#touch .dp, #dpad')) el.hidden = !settings.dpad;
  resize(); requestAnimationFrame(resize);
}

// ---------------------------------------------------------------------------------------------- main loop
let acc = 0, rafPrev = 0, ctx2d = null, img2d = null;
let busy = null, romGen = 0;     // busy: promise while a field waits for GPU results the game wants to read
const bp = { off: false, since: 0 };
const perf = { frames: 0, emu: 0, t0: 0, fps: 0, ms: 0, peak: 0, stalls: 0, slow: 0, syncs0: 0, waits0: 0 };
// One field. Returns false if it could not be finished yet: the game is about to look at something the GPU drew,
// so the core has stopped until those results have been copied back (see N64Gpu.syncNow).
function emulate(drawLast, dropAudio) {
  try {
  readInput();
  const t = performance.now();
  if (ex.n64_frame()) {
    const gen = romGen;
    const p = (async () => {
      try {
        do { if (gpu && useGpu) await gpu.syncNow(); else ex.n64_sync_done(); } while (gen === romGen && ex.n64_frame());
        if (gen === romGen) finishField(drawLast, dropAudio, t);
      } catch (e) { rendererFailure(e); }
    })();
    busy = p;
    p.then(() => { if (busy === p) busy = null; });
    return false;
  }
  finishField(drawLast, dropAudio, t);
  return true;
  } catch (error) { rendererFailure(error); return false; }
}
function finishField(drawLast, dropAudio, t) {
  if (useGpu) gpu.present(drawLast);
  else { ex.n64_vi_render(); if (drawLast) drawSoftware(); }
  const dt = performance.now() - t;
  perf.emu += dt; perf.frames++; if (dt > perf.peak) perf.peak = dt;
  audioPump(dropAudio);
}
function drawSoftware() {
  const dims = ex.n64_vi_dims(), w = dims & 0xFFFF, h = (dims >> 16) & 0x7FFF, cv = $('cv-sw');
  if (cv.width !== w || cv.height !== h || !img2d) { cv.width = w; cv.height = h; ctx2d = cv.getContext('2d', { alpha: false }); img2d = ctx2d.createImageData(w, h); }
  img2d.data.set(new Uint8Array(ex.memory.buffer, hi[1], w * h * 4));
  ctx2d.putImageData(img2d, 0, 0);
}
function tick(now) {
  requestAnimationFrame(tick);
  const dt = Math.min(now - rafPrev, 200); rafPrev = now;
  if (!running || paused || busy || document.hidden) return;
  // never queue more than a couple of frames ahead of the GPU: a slow GPU slows the game down instead of piling up work
  // (if the browser never reports any completed work at all, the throttle is switched off rather than freezing the game)
  if (useGpu && !bp.off && gpu.pending > 4) {
    if (!bp.since) bp.since = now;
    if (now - bp.since > 10000 && !gpu.done) bp.off = true;
    else { acc = 0; perf.stalls++; return; }
  } else bp.since = 0;
  const frameMs = 1000 / hz;
  let n = 0;
  if (fastForward || ffLock) {
    const t0 = performance.now();
    let ok;
    do { ok = emulate(false, true); n++; } while (ok && n < 12 && performance.now() - t0 < 11);
    if (ok) emulate(true, true);
  } else {
    // one emulated field per display refresh whenever the two rates are within a few percent; otherwise accumulate
    acc += dt;
    while (acc >= frameMs * 0.97 && n < 3) { acc -= frameMs; n++; }
    if (acc > frameMs * 3) acc = 0;
    if (acc < -frameMs * 0.03) acc = -frameMs * 0.03;
    for (let i = 0; i < n; i++) if (!emulate(i === n - 1, false)) break;
  }
  if (n) {
    const rumble = u32v()[hi[23] >> 2] !== 0;
    if (rumble !== rumbleOn) { rumbleOn = rumble; if (isTouch && navigator.vibrate) navigator.vibrate(rumble ? 400 : 0); }
  }
  if (now - perf.t0 >= 1000) {
    perf.fps = perf.frames * 1000 / (now - perf.t0); perf.ms = perf.frames ? perf.emu / perf.frames : 0;
    // (reads: how often per second the game looked at GPU-drawn memory; waits: how often that had to wait for the GPU)
    const gs = useGpu ? gpu.stats : { syncs: 0, waits: 0 }, rd = gs.syncs - perf.syncs0, wt = gs.waits - perf.waits0;
    perf.syncs0 = gs.syncs; perf.waits0 = gs.waits;
    if (settings.hud) $('hud').textContent = `${perf.fps.toFixed(1)} fps  ${perf.ms.toFixed(2)} ms (peak ${perf.peak.toFixed(1)})  ${useGpu ? 'WebGPU' + (gpu.scaleLog2 ? ` ${1 << gpu.scaleLog2}×` : '') : 'software'}${rd > 0 ? `  reads ${rd} (${wt} waited)` : ''}${(fastForward || ffLock) ? '  ▶▶' : ''}`;
    // GPU persistently unable to keep up? say so once
    if (useGpu && perf.stalls > 20 && perf.fps < hz * 0.8) { if (++perf.slow === 4) toast('The GPU is not keeping up — try the software renderer in Settings', 5000); } else if (perf.slow < 4) perf.slow = 0;
    perf.frames = 0; perf.emu = 0; perf.peak = 0; perf.stalls = 0; perf.t0 = now;
    if (padInfo && !$('stage').hidden) { const i = safeInsets(), j = padInfo.insets; if (i.some((v, k) => Math.abs(v - j[k]) > 0.5)) resize(); }   // safe area changed without a resize event
    flushSaves(false);
  }
}
function resize() {
  touchLayout();
  const s = $('screen');
  // Reduce both dimensions together: independently clamping a high-DPI or ultrawide
  // canvas changes the aspect ratio seen by the GPU presentation shader.
  const dpr = Math.min(window.devicePixelRatio || 1, 3, 4096 / Math.max(1, s.clientWidth), 4096 / Math.max(1, s.clientHeight));
  const w = Math.max(1, Math.round(s.clientWidth * dpr)), h = Math.max(1, Math.round(s.clientHeight * dpr));
  const cv = $('cv-gpu');
  if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
  // software canvas: letterbox to 4:3 with CSS (its backing store is the raw 640-wide VI field)
  const sw = $('cv-sw'), cw = s.clientWidth, ch = s.clientHeight;
  let dw = cw, dh = ch;
  if (!+settings.aspect) { if (cw * 3 > ch * 4) dw = ch * 4 / 3; else dh = cw * 3 / 4; }
  sw.style.left = (cw - dw) / 2 + 'px'; sw.style.top = (ch - dh) / 2 + 'px'; sw.style.width = dw + 'px'; sw.style.height = dh + 'px';
}
function togglePause(force) {
  if (!rom || rendererFailed) return;
  paused = force === undefined ? !paused : force;
  if (actx) { if (paused) actx.suspend().catch(() => {}); else actx.resume().catch(() => {}); }
  if (paused) flushSaves(); else { acc = 0; audioReset(); }
  updateFlag();
}
let lock = null;
async function wakeLock() { try { if (navigator.wakeLock && !lock) { lock = await navigator.wakeLock.request('screen'); lock.addEventListener('release', () => { lock = null; }); } } catch (e) { /* not allowed */ } }

// ---------------------------------------------------------------------------------------------- UI
// Two screens and one sheet. Home is the library; the game screen shows nothing but the picture, the pad and one menu button.
// That button (or Esc) pauses the game and opens the sheet, which holds the game menu, the save state slots and the settings.
let toastT = 0, menuT = 0;
function toast(msg, ms = 1800) { const t = $('toast'); t.textContent = msg; t.classList.add('show'); clearTimeout(toastT); toastT = setTimeout(() => t.classList.remove('show'), ms); }
// with a mouse the menu button fades away until the pointer moves
function pokeMenu() { const b = $('b-menu'); b.classList.remove('idle'); clearTimeout(menuT); menuT = setTimeout(() => b.classList.add('idle'), 2600); }
function updateFlag() {              // note at the top of the picture while the game is paused or fast-forwarding
  const f = $('flag'), on = !!rom && !sheetOpen() && (paused || fastForward || ffLock);
  f.hidden = !on;
  if (on) f.innerHTML = UiArt.icon(paused ? 'pause' : 'ff') + (paused ? 'Paused' : 'Fast forward');
}
function keyName(c) { return (c || '—').replace(/^Key|^Digit/, '').replace(/^Arrow/, '').replace('ShiftLeft', 'Shift').replace('ShiftRight', 'R Shift'); }
function buildBinds() {
  const host = $('binds'); host.textContent = '';
  for (const [a, label] of KEY_ACTIONS) {
    const b = document.createElement('button');
    b.innerHTML = `<span></span><b></b>`; b.firstChild.textContent = label; b.lastChild.textContent = rebinding === a ? 'press a key' : keyName(settings.keys[a]);
    if (rebinding === a) b.className = 'wait';
    b.onclick = () => { rebinding = a; buildBinds(); };
    host.appendChild(b);
  }
}
function updatePills() {
  const p = $('pills'); p.textContent = '';
  const add = (t, c) => { const s = document.createElement('span'); s.className = 'pill ' + (c || ''); s.textContent = t; p.appendChild(s); };
  if (gpu) add('WebGPU ready' + (gpu.desc && gpu.desc !== 'WebGPU' ? ' · ' + gpu.desc : ''), 'ok');
  else if (gpuErr) add('Software renderer · ' + gpuErr, 'warn');
  else add('Preparing WebGPU…');
  $('gpuinfo').textContent = gpu ? (gpu.desc || 'WebGPU') : (gpuErr || 'starting…');
}

// ---- settings controls. A <select> stays the source of truth; what is shown is a row of buttons made from its options.
function segSync(sel) {
  const seg = sel._seg; if (!seg) return;
  [...seg.children].forEach((b, i) => { const o = sel.options[i], on = o.value === sel.value;
    b.classList.toggle('on', on); b.disabled = sel.disabled || o.disabled;
    b.setAttribute('aria-checked', String(on)); b.tabIndex = on && !b.disabled ? 0 : -1;
  });
}
function enhanceSelect(sel) {
  const seg = document.createElement('div'); seg.className = 'seg'; seg.setAttribute('role', 'radiogroup');
  const label = sel.parentNode.querySelector('label');
  if (label) { label.id = sel.id + '-label'; seg.setAttribute('aria-labelledby', label.id); }
  for (const o of sel.options) {
    const b = document.createElement('button'); b.type = 'button'; b.textContent = o.textContent; b.setAttribute('role', 'radio');
    b.onclick = () => { if (sel.value === o.value) return; sel.value = o.value; sel.dispatchEvent(new Event('change')); segSync(sel); };
    seg.appendChild(b);
  }
  sel.hidden = true; sel._seg = seg; sel.after(seg); segSync(sel);
  sel.addEventListener('change', () => segSync(sel));
  seg.addEventListener('keydown', e => {
    if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(e.key)) return;
    const buttons = [...seg.children].filter(b => !b.disabled), i = buttons.indexOf(document.activeElement);
    if (i < 0) return;
    e.preventDefault();
    const n = e.key === 'Home' ? 0 : e.key === 'End' ? buttons.length - 1 : (i + (['ArrowLeft', 'ArrowUp'].includes(e.key) ? -1 : 1) + buttons.length) % buttons.length;
    buttons[n].click(); buttons[n].focus();
  });
}
const bound = [];                 // every settings control: how to show the stored value again, and what a change sets off
function bindSetting(id, key, kind, onchange) {
  const el = $(id);
  const show = () => { if (kind === 'check') el.checked = !!settings[key]; else el.value = settings[key]; segSync(el); };
  show(); bound.push({ show, onchange });
  el.addEventListener(kind === 'range' ? 'input' : 'change', () => {
    settings[key] = kind === 'check' ? el.checked : (kind === 'range' || kind === 'num' ? +el.value : el.value);
    saveSettings(); if (onchange) onchange();
  });
}

// ---- the sheet
let sheetPage = null, sheetStack = [], sheetOpener = null, menuThumb = '', resumeOnClose = false, statesMode = 'save';
const BLANK = 'data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==';
function sheetOpen() { return !$('sheet').hidden; }
function sheetFocusables() {
  return [...$('sheet').querySelectorAll('button, input, [tabindex], summary')].filter(e => !e.disabled && e.tabIndex >= 0 && e.getClientRects().length);
}
function focusSheet(target) { (target && target.getClientRects().length ? target : sheetFocusables()[0] || $('sheet').querySelector('.panel')).focus(); }
function showPage(page, focus) {
  sheetPage = page;
  $('s-export').disabled = $('s-import').disabled = !rom;
  for (const p of ['menu', 'states', 'settings']) $('pg-' + p).hidden = p !== page;
  $('sheet').classList.toggle('wide', page === 'settings');
  const panel = $('sheet').querySelector('.panel');
  if (page === 'menu') { panel.removeAttribute('aria-labelledby'); panel.setAttribute('aria-label', 'Game menu'); }
  else { panel.removeAttribute('aria-label'); panel.setAttribute('aria-labelledby', page === 'settings' ? 'set-title' : 'st-title'); }
  for (const b of document.querySelectorAll('#sheet [data-nav=back]')) {
    b.innerHTML = UiArt.icon(sheetStack.length ? 'back' : 'close'); b.setAttribute('aria-label', sheetStack.length ? 'Back' : 'Close');
  }
  focusSheet(focus);
}
function openSheet(page, opener = document.activeElement) {
  // Safari does not necessarily focus a clicked button. Keep its explicit trigger
  // rather than relying on activeElement when restoring navigation focus.
  if (sheetOpen()) sheetStack.push({ page: sheetPage, focus: opener });
  else {
    sheetOpener = opener;
    sheetStack = []; $('sheet').hidden = false;
    $('home').inert = $('stage').inert = true; releaseInput();
    if (rom) { resumeOnClose = !paused; togglePause(true); }      // the game waits while the sheet is up
  }
  showPage(page); updateFlag();
}
function sheetBack() { if (sheetStack.length) { const prev = sheetStack.pop(); showPage(prev.page, prev.focus); } else closeSheet(); }
function closeSheet(stayPaused) {
  if (!sheetOpen()) return;
  $('sheet').hidden = true; sheetStack = [];
  $('home').inert = $('stage').inert = false;
  if (rebinding) { rebinding = null; buildBinds(); }
  for (const b of document.querySelectorAll('#sheet .sure')) disarm(b);
  if (resumeOnClose) { resumeOnClose = false; if (!stayPaused) togglePause(false); }
  updateFlag(); pokeMenu();
  const target = sheetOpener && sheetOpener !== document.body && sheetOpener.isConnected && sheetOpener.getClientRects().length ? sheetOpener : $(rom ? 'b-menu' : 'h-set');
  sheetOpener = null; target.focus({ preventScroll: true });
}
function fullscreenElement() { return document.fullscreenElement || document.webkitFullscreenElement; }
function exitFullscreen() { const exit = document.exitFullscreen || document.webkitExitFullscreen; return exit ? Promise.resolve(exit.call(document)) : Promise.resolve(); }
// destructive actions ask once more: the first tap arms the button, the second one within a moment does it
function disarm(b) { clearTimeout(b._t); b.classList.remove('sure'); if (b._label) b._label.textContent = b._text; }
function confirmTap(b, label, sure, fn) {
  if (b.classList.contains('sure')) { disarm(b); fn(); return; }
  b._label = label; b._text = label.textContent; label.textContent = sure; b.classList.add('sure');
  b._t = setTimeout(() => disarm(b), 2600);
}
function updateMenuTiles() {
  const m = $('m-mute'); if (!m || !m.firstChild.firstChild) return;
  m.firstChild.innerHTML = UiArt.icon(settings.mute ? 'mute' : 'sound'); m.lastChild.textContent = settings.mute ? 'Sound off' : 'Sound on';
  m.classList.toggle('on', !!settings.mute); $('m-ff').classList.toggle('on', ffLock);
}
async function openMenu(opener = $('b-menu')) {
  if (!rom || sheetOpen()) return;
  openSheet('menu', opener);
  $('m-title').textContent = rom.name;
  $('m-sub').textContent = `${['', 'EEPROM save', 'EEPROM save', 'SRAM save', 'Flash save'][rom.saveType] || ''} · ${useGpu ? 'WebGPU' + (gpu.scaleLog2 ? ` ${1 << gpu.scaleLog2}×` : '') : 'Software'}`;
  $('m-thumb').src = menuThumb || BLANK;
  updateMenuTiles();
  const r = rom;
  if (busy) await busy;
  const t = await grabThumb();
  if (t && rom === r) { menuThumb = t; $('m-thumb').src = t; if (settings.keep && !thumbDark) idbSet('thumb:' + r.key, t); }
}
function resetGame() { return sessionOp(resetGameNow); }
async function resetGameNow() {
  if (!rom) return;
  if (busy) await busy;
  romGen++; ex.n64_reset();
  if (gpuErr === 'device lost') { gpu = null; useGpu = false; ex.n64_config(0, 0); applyRenderer(); }
  else if (useGpu) { try { gpu.reset(); } catch (error) { rendererFailure(error); return false; } }
  rendererFailed = false;
  updateXpak(); audioReset(); closeSheet(); togglePause(false); toast('Game reset');
}
function ago(t) {
  if (!t) return 'Saved earlier';
  const m = Math.round((Date.now() - t) / 60000);
  if (m < 1) return 'Just now'; if (m < 60) return `${m} min ago`; if (m < 1440) return `${Math.round(m / 60)} h ago`;
  return new Date(t).toLocaleDateString(undefined, { day: 'numeric', month: 'short' });
}
async function openStates(mode, opener) {
  statesMode = mode;
  $('st-title').textContent = mode === 'save' ? 'Save state' : 'Load state';
  $('st-hint').textContent = (mode === 'save' ? 'Saving replaces what is in the slot.' : 'Loading replaces the game in progress.') + (isTouch ? '' : mode === 'save' ? ' F2 saves to slot 1 at any time.' : ' F4 loads slot 1 at any time.');
  const host = $('slots'); host.textContent = '';
  openSheet('states', opener);
  const r = rom;
  let meta; try { meta = await stateMeta(r); } catch (e) { toast('Could not read state slots'); return; }
  if (rom !== r) return;
  host.textContent = '';
  for (let i = 0; i < SLOTS; i++) {
    const m = meta[i], b = document.createElement('button');
    b.className = 'slot'; b.disabled = mode === 'load' && !m;
    b.innerHTML = '<div class="im"></div><b></b><span></span>';
    if (m && m.thumb) b.firstChild.style.backgroundImage = `url(${m.thumb})`; else b.firstChild.textContent = i + 1;
    b.children[1].textContent = `Slot ${i + 1}`; b.children[2].textContent = m ? ago(m.t) : 'Empty';
    b.onclick = async () => {
      if (mode === 'save') { if (await saveState(i, menuThumb)) closeSheet(); }
      else { if (await loadState(i)) closeSheet(); }
    };
    host.appendChild(b);
  }
}

// ---- library: the games opened before, kept in this browser with a picture of where they were left
async function libList() { return (await idbGet('lib')) || []; }
async function libAdd(data, fromLibrary, r = rom, size = romSize) {
  if (!settings.keep) return;
  const list = await libList();
  const e = list.find(x => x.key === r.key) || { key: r.key, name: r.name, title: r.title, file: r.file, size };
  e.last = Date.now();
  const entries = [['lib', [e, ...list.filter(x => x.key !== r.key)]]];
  if (!fromLibrary && !await idbHas('rom:' + r.key)) entries.push(['rom:' + r.key, data.slice(0, size).buffer]);
  if (await idbWrite(entries) === false) toast('Not enough browser storage to keep this game in the library', 4000);
}
function libRemove(key) { return sessionOp(() => libRemoveNow(key)); }
async function libRemoveNow(key) {
  if (await idbWrite([['lib', (await libList()).filter(x => x.key !== key)]], ['rom:' + key, 'thumb:' + key]) === false) return toast('Could not remove that game');
  renderLibrary();
}
async function libStart(e) {
  try {
    const buf = await idbGet('rom:' + e.key);
    if (!buf) { toast('That game is no longer stored here — add it again', 3500); return libRemove(e.key); }
    await loadRom(new Uint8Array(buf), e.file || e.name, true);
  } catch (err) { console.error(err); toast(String(err.message || err), 4000); }
}
async function renderLibrary() {
  const list = await libList(), host = $('lib');
  const thumbs = await Promise.all(list.map(e => idbGet('thumb:' + e.key)));
  $('empty').hidden = list.length > 0; $('libwrap').hidden = !list.length;
  host.textContent = '';
  list.forEach((e, i) => {
    const c = document.createElement('div');
    c.className = 'cart'; c.tabIndex = 0; c.setAttribute('role', 'button');
    c.innerHTML = '<div class="lbl"></div><div class="nm"></div><div class="sub"></div><div class="grip"></div><button class="rm" aria-label="Remove from library"></button>';
    if (thumbs[i]) { const im = new Image(); im.alt = ''; im.src = thumbs[i]; c.firstChild.appendChild(im); }
    else { c.firstChild.innerHTML = '<div class="ph"><span></span></div>'; c.firstChild.firstChild.firstChild.textContent = (e.name.match(/\b[A-Za-z0-9]/g) || ['?']).slice(0, 3).join('').toUpperCase(); }
    c.children[1].textContent = e.name; c.children[2].textContent = ago(e.last).replace('Saved earlier', '');
    const rm = c.lastChild; rm.innerHTML = UiArt.icon('trash');
    rm.onclick = ev => { ev.stopPropagation(); if (rm.classList.contains('sure')) libRemove(e.key); else { rm.classList.add('sure'); rm.textContent = 'Remove'; setTimeout(() => { rm.classList.remove('sure'); rm.innerHTML = UiArt.icon('trash'); }, 2600); } };
    c.onclick = () => libStart(e);
    c.onkeydown = ev => { if (ev.target === c && (ev.key === 'Enter' || ev.key === ' ')) { ev.preventDefault(); libStart(e); } };
    host.appendChild(c);
  });
  const add = document.createElement('button');
  add.className = 'cart add'; add.id = 'h-add'; add.innerHTML = `<span>${UiArt.icon('plus')}Add game</span>`;
  add.onclick = () => $('file').click();
  host.appendChild(add);
}
function goHome() { return sessionOp(goHomeNow).catch(e => toast(e.message, 6000)); }
async function goHomeNow() {
  if (busy) await busy;
  if (!await flushSaves()) throw new Error('Export your battery save before leaving this game.');
  romGen++;
  if (actx) actx.suspend().catch(() => {});
  paused = false; ffLock = false; fastForward = false; resumeOnClose = false; menuThumb = '';
  closeSheet(true); releaseInput(); $('stage').hidden = true; $('home').hidden = false;
  if (settings.keep && rom) { const k = rom.key, list = await libList(), e = list.find(x => x.key === k); if (e) { e.last = Date.now(); await idbSet('lib', list); } }
  rom = null; document.title = 'Photon64';
  if (fullscreenElement()) exitFullscreen().catch(() => {});
  updateFlag(); resize(); renderLibrary();
  $('h-set').focus({ preventScroll: true });
}

// Everything in Settings back to how it was the first time, the keyboard included. Games, saves and states are not settings.
function restoreDefaults() {
  Object.assign(settings, DEFAULT_SETTINGS());
  saveSettings();
  for (const b of bound) b.show();
  for (const f of new Set(bound.map(b => b.onchange))) if (f) f();
  buildBinds(); updateMenuTiles();
  toast('Settings restored to their defaults', 2400);
}

function initUI() {
  const ic = (id, name) => { const el = $(id); (el.querySelector('i') || el).innerHTML = UiArt.icon(name); };
  // home
  $('logo').innerHTML = UiArt.logo(40); $('emptyart').innerHTML = UiArt.emptyCart(116);
  $('drop').innerHTML = UiArt.icon('plus') + 'Choose a ROM'; ic('h-set', 'gear');
  if (isTouch) document.body.classList.add('touchdev');
  const pick = () => $('file').click();
  $('drop').onclick = pick;
  $('file').onchange = e => { const f = e.target.files[0]; e.target.value = ''; if (f) openFile(f); };
  const over = on => { $('empty').classList.toggle('over', on); const a = $('h-add'); if (a) a.classList.toggle('over', on); };
  for (const ev of ['dragenter', 'dragover']) addEventListener(ev, e => { e.preventDefault(); over(true); });
  for (const ev of ['dragleave', 'drop']) addEventListener(ev, e => { e.preventDefault(); over(false); });
  addEventListener('drop', e => { const f = e.dataTransfer && e.dataTransfer.files[0]; if (f) openFile(f); });
  $('h-set').onclick = e => openSheet('settings', e.currentTarget);

  // game menu
  $('m-resume').innerHTML = UiArt.icon('play') + 'Resume';
  ic('m-save', 'save'); ic('m-load', 'load'); ic('m-ff', 'ff'); ic('m-mute', 'sound'); ic('m-full', 'full'); ic('m-set', 'gear'); ic('m-reset', 'reset'); ic('m-home', 'library');
  $('b-menu').onclick = e => openMenu(e.currentTarget);
  $('m-resume').onclick = () => closeSheet();
  $('m-save').onclick = e => openStates('save', e.currentTarget); $('m-load').onclick = e => openStates('load', e.currentTarget);
  $('m-ff').onclick = () => { ffLock = !ffLock; if (!ffLock) audioReset(); closeSheet(); };
  $('m-mute').onclick = () => { settings.mute = !settings.mute; saveSettings(); setVolume(); };
  const fsEl = document.documentElement;
  const requestFull = fsEl.requestFullscreen || fsEl.webkitRequestFullscreen;
  const fullEnabled = document.fullscreenEnabled ?? document.webkitFullscreenEnabled ?? !!requestFull;
  if (!requestFull || !fullEnabled) { $('m-full').hidden = true; $('m-full').parentNode.classList.add('n5'); }
  $('m-full').onclick = async () => {
    try { if (fullscreenElement()) await exitFullscreen(); else await requestFull.call(fsEl); closeSheet(); }
    catch (e) { toast('Full screen is unavailable in this browser or window', 3000); }
  };
  $('m-set').onclick = e => openSheet('settings', e.currentTarget);
  $('m-reset').onclick = () => confirmTap($('m-reset'), $('m-reset').lastChild, 'Tap to reset', resetGame);
  $('m-home').onclick = goHome;
  $('sheet').querySelector('.scrim').onclick = () => closeSheet();
  for (const b of document.querySelectorAll('#sheet [data-nav=back]')) b.onclick = sheetBack;
  $('sheet').addEventListener('keydown', e => {
    if (e.key !== 'Tab' || rebinding) return;
    const controls = sheetFocusables(), first = controls[0], last = controls[controls.length - 1];
    if (!first) { e.preventDefault(); focusSheet(); }
    else if (e.shiftKey && (document.activeElement === first || !controls.includes(document.activeElement))) { e.preventDefault(); last.focus(); }
    else if (!e.shiftKey && (document.activeElement === last || !controls.includes(document.activeElement))) { e.preventDefault(); first.focus(); }
  });

  // settings
  const tabs = { video: ['video', 'Video'], console: ['console', 'Console'], pad: ['pad', 'Controls'], data: ['data', 'Data'] };
  const showTab = t => {
    for (const b of $('tabs').children) { const on = b.dataset.tab === t; b.classList.toggle('on', on); b.setAttribute('aria-selected', String(on)); b.tabIndex = on ? 0 : -1; }
    for (const p of $('set-body').children) p.hidden = p.dataset.pane !== t;
    $('set-body').scrollTop = 0;
  };
  $('tabs').setAttribute('role', 'tablist'); $('tabs').setAttribute('aria-label', 'Settings');
  for (const b of $('tabs').children) {
    const [i, label] = tabs[b.dataset.tab]; b.innerHTML = UiArt.icon(i) + label; b.onclick = () => showTab(b.dataset.tab);
    b.id = 'tab-' + b.dataset.tab; b.setAttribute('role', 'tab'); b.setAttribute('aria-controls', 'pane-' + b.dataset.tab);
  }
  for (const p of $('set-body').children) { p.id = 'pane-' + p.dataset.pane; p.setAttribute('role', 'tabpanel'); p.setAttribute('aria-labelledby', 'tab-' + p.dataset.pane); }
  $('tabs').addEventListener('keydown', e => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key)) return;
    const buttons = [...$('tabs').children], i = buttons.indexOf(document.activeElement); if (i < 0) return;
    e.preventDefault();
    const n = e.key === 'Home' ? 0 : e.key === 'End' ? buttons.length - 1 : (i + (e.key === 'ArrowLeft' ? -1 : 1) + buttons.length) % buttons.length;
    buttons[n].click(); buttons[n].focus();
  });
  showTab('video');
  bindSetting('s-renderer', 'renderer', 'sel', applyRenderer);
  bindSetting('s-scale', 'scale', 'num', applyScale);
  bindSetting('s-vif', 'vif', 'check', () => { if (ex) ex.n64_config(9, settings.vif ? 0 : 1); });
  bindSetting('s-filter', 'filter', 'num', applyRenderer);
  bindSetting('s-aspect', 'aspect', 'num', applyRenderer);
  bindSetting('s-readback', 'readback', 'check', applyRenderer);
  bindSetting('s-hud', 'hud', 'check', () => { $('hud').hidden = !settings.hud; });
  bindSetting('s-cpi', 'cpi', 'num', () => { if (ex) ex.n64_config(1, +settings.cpi); });
  bindSetting('s-pak', 'pak', 'num', () => { if (ex) ex.n64_config(5, +settings.pak); });
  bindSetting('s-xpak', 'xpak', 'sel', applyXpak);
  bindSetting('s-vol', 'vol', 'range', setVolume);
  bindSetting('s-touch', 'touch', 'sel', updateTouchVisibility);
  bindSetting('s-dpad', 'dpad', 'check', updateTouchVisibility);
  bindSetting('s-top', 'top', 'range', updateTouchVisibility);
  bindSetting('s-tsize', 'tsize', 'range', updateTouchVisibility);
  bindSetting('s-tlift', 'tlift', 'range', updateTouchVisibility);
  bindSetting('s-zr', 'zr', 'check', () => { buildPad(); updateTouchVisibility(); });
  bindSetting('s-keep', 'keep', 'check');
  for (const sel of document.querySelectorAll('#sheet select')) enhanceSelect(sel);
  updateXpak();
  $('hud').hidden = !settings.hud;
  $('s-defaults').onclick = () => confirmTap($('s-defaults'), $('s-defaults'), 'Tap again to restore', restoreDefaults);
  $('s-keys-reset').onclick = () => { settings.keys = { ...DEFAULT_KEYS }; saveSettings(); buildBinds(); };
  $('s-export').onclick = () => {
    if (!rom) return toast('Start a game first');
    const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([saveBlob()])); a.download = rom.name.replace(/[^\w.-]+/g, '_') + '.p64save'; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  };
  $('s-import').onclick = () => { if (!rom) return toast('Start a game first'); $('savefile').click(); };
  $('savefile').onchange = async e => {
    const f = e.target.files[0], r = rom; e.target.value = ''; if (!f) return;
    try {
      if (f.size !== 0x40800) throw new Error('Not a Photon64 save file');
      const b = new Uint8Array(await f.arrayBuffer());
      await sessionOp(async () => {
        if (!r || rom !== r) throw new Error('The active game changed. Import the save again.');
        if (b.length !== 0x40800) throw new Error('Not a Photon64 save file');
        const outcome = await idbSet('save:' + r.key, b.buffer);
        if (outcome === false) throw new Error('Could not import save');
        restoreSaveBlob(b); await resetGameNow();
        toast(outcome === true ? 'Save imported — game reset' : 'Save imported temporarily — export before closing');
      });
    } catch (err) { toast(err.message, 6000); }
  };

  for (const ev of ['pointermove', 'pointerdown']) addEventListener(ev, e => { if (e.pointerType === 'mouse') pokeMenu(); });
  addEventListener('pointerdown', () => audioStart(), { capture: true });
  // rotation and browser chrome changes arrive in several steps on phones; lay out again once things have settled
  const relayout = e => { if (e && e.type !== 'fullscreenchange' && e.target !== self.visualViewport) hostReset(); resize(); clearTimeout(relayout.t); relayout.t = setTimeout(resize, 350); };
  for (const ev of ['resize', 'orientationchange', 'fullscreenchange', 'webkitfullscreenchange']) addEventListener(ev, relayout);
  if (self.visualViewport) visualViewport.addEventListener('resize', relayout);
  if (self.ResizeObserver) new ResizeObserver(resize).observe($('screen'));
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { releaseInput(); flushSaves(); if (actx && !paused) actx.suspend().catch(() => {}); }
    else { acc = 0; rafPrev = performance.now(); if (actx && !paused && rom) actx.resume().catch(() => {}); if (running) wakeLock(); audioReset(); }
  });
  addEventListener('pagehide', () => { releaseInput(); flushSaves(); });
  buildPad(); buildBinds(); updatePills(); initTouch(); renderLibrary(); hostWatch(); dispWatch();
}

(async function main() {
  initUI();
  try { await initCore(); } catch (e) { console.error(e); $('emptymsg').textContent = 'WebAssembly failed to start: ' + e.message; return; }
  requestAnimationFrame(tick);
  await initGpu();
  updatePills();
  gpuStarting = false;
  if (rom) { applyRenderer(); if (useGpu) toast('WebGPU renderer ready'); } else applyScale();
})();
// test hook
window.__photon = { get ex() { return ex; }, get gpu() { return gpu; }, get hi() { return hi; }, get useGpu() { return useGpu; }, loadRom, perf, settings, gpuName, hostSample, hostFit, saveState, loadState, flushSaves, resetGame, openMenu, closeSheet, openSheet, libList,
  get rom() { return rom; }, get running() { return running; }, get rendererFailed() { return rendererFailed; }, get busy() { return busy; }, setPaused: p => { paused = p; }, emulate, touchState, updateXpak,
  get pad() { return padInfo; }, setInsets: v => { insetOverride = v; resize(); } };
