// JS input-validation checks: exercises the shipped app.js functions (extracted
// verbatim from src/web/app.js, never copied) under Node.
//   node tools/jscheck.mjs
import fs from 'fs';
import path from 'path';
import zlib from 'zlib';
import { fileURLToPath } from 'url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const src = fs.readFileSync(path.join(root, 'src/web/app.js'), 'utf8');

// --- extract verbatim source chunks (fails loudly if the app moves them) ---
function bracesFrom(i) {  // balanced-brace span starting at the '{' at/after i
  i = src.indexOf('{', i);
  let d = 0;
  for (let j = i; j < src.length; j++) {
    const c = src[j];
    if (c === "'" || c === '"' || c === '`') {  // skip strings
      for (j++; j < src.length; j++) {
        if (src[j] === '\\') j++;
        else if (src[j] === c) break;
        else if (c === '`' && src[j] === '$' && src[j + 1] === '{') { j++; let dd = 1; for (j++; dd && j < src.length; j++) { if (src[j] === '{') dd++; if (src[j] === '}') dd--; } j--; }
      }
      continue;
    }
    if (c === '/' && src[j + 1] === '/') { j = src.indexOf('\n', j); if (j < 0) j = src.length; continue; }
    if (c === '/' && src[j + 1] === '*') { j = src.indexOf('*/', j) + 1; continue; }
    if (c === '{') d++;
    if (c === '}') { if (--d === 0) return src.slice(i, j + 1); }
  }
  throw new Error('unbalanced braces');
}
function line(start) {
  const i = src.indexOf(start);
  if (i < 0) throw new Error('not found in app.js: ' + start);
  return src.slice(i, src.indexOf('\n', i));
}
// simpler robust path: slice from each marker through its balanced end
function chunk(startMarker) {
  const i = src.indexOf(startMarker);
  if (i < 0) throw new Error('not found in app.js: ' + startMarker);
  return src.slice(i, i + bracesFrom(i).length + (src.indexOf('{', i) - i));
}

const parts = [
  line('const MAX_ROM'),
  chunk('function checkRomSize'),
  chunk('async function unzip'),
  src.slice(src.indexOf('let _mbuf'), src.indexOf('\n', src.indexOf('const u32v = '))),
  line('const SLOTS = '),
  line('const stateKey = '),
  chunk('async function stateMeta'),
];
for (const p of parts) if (!p || p.length < 10) throw new Error('extraction failed');
const factory = new Function('self', 'ex', 'rom', 'idbGet',
  parts.join('\n') + '\nreturn { unzip, checkRomSize, u8v, u32v, stateMeta, MAX_ROM };');

// --- minimal zip builder ---
function zip(entries) {
  const chunks = [], central = [];
  let off = 0;
  for (const e of entries) {
    const name = Buffer.from(e.name);
    const raw = Buffer.from(e.data);
    const comp = e.method === 8 ? zlib.deflateRawSync(raw) : raw;
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(e.method, 8);
    lh.writeUInt32LE(e.csize ?? comp.length, 18); lh.writeUInt32LE(e.usize ?? raw.length, 22);
    lh.writeUInt16LE(name.length, 26);
    chunks.push(lh, name, comp);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(e.method, 10);
    ch.writeUInt32LE(e.csize ?? comp.length, 20); ch.writeUInt32LE(e.usize ?? raw.length, 24);
    ch.writeUInt16LE(name.length, 28); ch.writeUInt32LE(e.off ?? off, 42);
    central.push(ch, name);
    off += 30 + name.length + comp.length;
  }
  const cdOff = off, cd = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(entries[0].cdOff ?? cdOff, 16);
  return new Uint8Array(Buffer.concat([...chunks, cd, end]));
}
const payload = (n, seed) => Uint8Array.from({ length: n }, (_, i) => (seed + i * 7) & 0xFF);

// --- run ---
let n = 0, bad = 0;
const check = (name, ok) => { n++; console.log((ok ? 'ok  ' : 'FAIL') + ' ' + name); if (!ok) bad++; };
const selfStub = { DecompressionStream };
const mk = (stubs = {}) => factory(selfStub, stubs.ex ?? null, stubs.rom ?? null, stubs.idbGet ?? null);

const t = mk();
const rom = payload(0x1000, 3);
check('zip stored roundtrip', await t.unzip(zip([{ name: 'game.z64', data: rom, method: 0 }]))
  .then(r => r.name === 'game.z64' && r.data.length === rom.length && r.data.every((v, i) => v === rom[i])));
check('zip deflated roundtrip', await t.unzip(zip([{ name: 'game.z64', data: rom, method: 8 }]))
  .then(r => r.data.length === rom.length && r.data.every((v, i) => v === rom[i])));
check('zip truncated csize rejected', await t.unzip(zip([{ name: 'game.z64', data: rom, method: 0, csize: 0x1000 + 500 }]))
  .then(() => false, e => /truncated/.test(e.message)));
check('zip bad local offset rejected', await t.unzip(zip([{ name: 'game.z64', data: rom, method: 0, off: 99999 }]))
  .then(() => false, e => /truncated|valid/.test(e.message)));
check('zip stored size lie rejected', await t.unzip(zip([{ name: 'game.z64', data: rom, method: 0, usize: 0x2000 }]))
  .then(() => false, e => /corrupt/.test(e.message)));
check('zip oversize usize rejected', await t.unzip(zip([{ name: 'game.z64', data: rom, method: 8, usize: 65 * 1024 * 1024 }]))
  .then(() => false, e => /too large/.test(e.message)));
check('zip deflate size lie rejected', await t.unzip(zip([{ name: 'game.z64', data: rom, method: 8, usize: 0x2000 }]))
  .then(() => false, e => /corrupt/.test(e.message)));
check('checkRomSize 64MB ok / 64MB+1 throws', (() => {
  try { t.checkRomSize(64 * 1024 * 1024); } catch { return false; }
  try { t.checkRomSize(64 * 1024 * 1024 + 1); return false; } catch (e) { return /too large/.test(e.message); }
})());
check('views cached, refreshed on buffer swap', (() => {
  const v = mk({ ex: { memory: { buffer: new ArrayBuffer(16) } } });
  if (v.u8v() !== v.u8v() || v.u32v() !== v.u32v()) return false;
  const nb = new ArrayBuffer(32);
  v.u8v(); // bound to old buffer
  const vv = mk({ ex: { memory: { buffer: nb } } });
  return vv.u8v().buffer === nb && vv.u8v() === vv.u8v() && vv.u8v() !== v.u8v();
})());
check('stateMeta backfills every slot', await (async () => {
  const store = new Map([['state:k', new ArrayBuffer(8)], ['state:k:2', new ArrayBuffer(8)]]);
  const v = mk({ rom: { key: 'k' }, idbGet: async k => store.get(k) ?? null });
  const m = await v.stateMeta();
  return m[0] && m[0].t === 0 && !m[1] && m[2] && m[2].t === 0 && !m[3];
})());
check('stateMeta keeps existing meta', await (async () => {
  const store = new Map([['smeta:k', [{ t: 5, thumb: 'x' }]], ['state:k', new ArrayBuffer(8)]]);
  const v = mk({ rom: { key: 'k' }, idbGet: async k => store.get(k) ?? null });
  const m = await v.stateMeta();
  return m[0].t === 5 && m[0].thumb === 'x';
})());

console.log(`${n} checks, ${bad} failures`);
process.exit(bad ? 1 : 0);
