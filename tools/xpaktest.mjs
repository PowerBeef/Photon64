// Expansion Pak checks on the WASM core: the per-game table, the three settings, and what the machine reports and does
// with 4 MB and 8 MB. Real ROMs (optional arguments) are also booted both ways.
//   node tools/xpaktest.mjs [rom ...]
import fs from 'fs';
const wasm = fs.readFileSync(new URL('../out/n64.wasm', import.meta.url));
const make = () => { let mem; const inst = new WebAssembly.Instance(new WebAssembly.Module(wasm), { env: { host_log: () => {}, host_gpu_flush: () => {} } }); return inst.exports; };
let bad = 0;
const check = (name, got, want) => { const ok = JSON.stringify(got) === JSON.stringify(want); if (!ok) bad++; console.log(ok ? 'ok  ' : 'FAIL', name, ok ? '' : `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); };
// a header-only cartridge image (big-endian, as dumped): game code at 0x3B, revision at 0x3F
const fake = (code, rev = 0) => { const r = new Uint8Array(0x2000); r.set([0x80, 0x37, 0x12, 0x40]); for (let i = 0; i < 4; i++) r[0x3B + i] = code.charCodeAt(i); r[0x3F] = rev; return r; };
const load = (ex, rom, mode) => { ex.n64_free_all(); const p = ex.n64_alloc(rom.length); new Uint8Array(ex.memory.buffer, p, rom.length).set(rom); ex.n64_config(10, mode); if (!ex.n64_load(p, rom.length)) throw new Error('bad rom'); return ex.n64_xpak(); };
const word = (ex, addr) => new Uint32Array(ex.memory.buffer, new Uint32Array(ex.memory.buffer, ex.n64_host_info(), 4)[0], 0x200000)[addr >> 2];

const ex = make();
// table: [code, revision, expected group, installed in automatic mode]
for (const [code, rev, kind, auto, what] of [
  ['NDOE', 0, 3, 1, 'Donkey Kong 64'], ['NZSJ', 0, 3, 1, 'Majora\'s Mask (J)'], ['NPDP', 0, 3, 1, 'Perfect Dark (E)'], ['NDPE', 0, 3, 1, 'Dinosaur Planet'],
  ['NSQE', 0, 2, 1, 'StarCraft 64'], ['NRUE', 0, 2, 1, 'Rush 2049'], ['NGDJ', 0, 2, 1, 'Gauntlet Legends (J)'], ['NIJE', 0, 2, 1, 'Indiana Jones'],
  ['NT2E', 0, 1, 1, 'Turok 2'], ['NREE', 0, 1, 1, 'Resident Evil 2'], ['NB5J', 0, 1, 1, 'Biohazard 2'], ['NP3E', 0, 1, 1, 'Pokemon Stadium 2'], ['N3TE', 0, 1, 1, 'Tony Hawk 3'],
  ['NEPE', 0, 1, 1, 'Episode I Racer'], ['NRSE', 0, 1, 1, 'Rogue Squadron'], ['NO7E', 0, 1, 1, 'The World Is Not Enough'], ['ND4E', 0, 1, 1, 'Castlevania LoD'],
  ['NSME', 0, 0, 1, 'Super Mario 64'], ['NGEE', 0, 0, 1, 'GoldenEye'], ['CZLE', 0, 0, 1, 'Ocarina of Time'], ['NT3J', 0, 0, 1, 'Toukon Road 2 (not Tony Hawk 3)'],
  ['CP2J', 0, 0, 1, 'Pocket Monsters Stadium 2 (= Stadium 1)'], ['\0\0\0\0', 0, 0, 1, 'homebrew without a game code'],
  ['NSVE', 0, 4, 0, 'Silicon Valley (U) 1.0'], ['NSVE', 1, 0, 1, 'Silicon Valley (U) 1.1'], ['NSVP', 0, 0, 1, 'Silicon Valley (E)'], ['NSVJ', 0, 4, 0, 'Silicon Valley (J)'],
  ['NWBE', 0, 4, 0, 'Iggy\'s Reckin\' Balls'],
]) {
  const a = load(ex, fake(code, rev), 0), on = load(ex, fake(code, rev), 1), off = load(ex, fake(code, rev), 2);
  check(`${what.padEnd(42)} group ${kind}, auto ${auto ? 'installs' : 'leaves out'}`, [a & 15, (a >> 8) & 1, (on >> 8) & 1, (off >> 8) & 1, word(ex, 0x318)], [kind, auto, 1, 0, 0x400000]);
}
// a change of setting waits for the reset
{ let v = load(ex, fake('NDOE'), 0); ex.n64_config(10, 2); v = ex.n64_xpak(); check('setting changed while running: still installed, removal pending', [(v >> 8) & 1, (v >> 9) & 1], [1, 0]);
  ex.n64_reset(); v = ex.n64_xpak(); check('after reset: removed', [(v >> 8) & 1, (v >> 9) & 1, word(ex, 0x318)], [0, 0, 0x400000]);
  ex.n64_config(10, 0); ex.n64_reset(); v = ex.n64_xpak(); check('back to automatic and reset: installed', [(v >> 8) & 1, word(ex, 0x318)], [1, 0x800000]); }

// real cartridges: boot with and without the pak, then look at what the boot code left and how memory behaves
for (const file of process.argv.slice(2)) {
  if (!fs.existsSync(file)) { console.log('skip', file); continue; }
  const rom = new Uint8Array(fs.readFileSync(file));
  for (const [mode, size] of [[1, 0x800000], [2, 0x400000]]) {
    const e = make(); load(e, rom, mode);
    for (let i = 0; i < 120; i++) while (e.n64_frame()) e.n64_sync_done();
    const sz = [word(e, 0x318), word(e, 0x3F0)];
    check(`${file} ${size >> 20} MB: memory size left for the game by the boot code`, sz.includes(size) && !sz.includes(size ^ 0xC00000), true);
    check(`${file} ${size >> 20} MB: running (frames ${new Uint32Array(e.memory.buffer, new Uint32Array(e.memory.buffer, e.n64_host_info(), 32)[21], 4)[0]})`, true, true);
  }
}
console.log(bad ? `${bad} FAILED` : 'all passed');
process.exit(bad ? 1 : 0);
