import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const source = fs.readFileSync(new URL('../src/web/app.js', import.meta.url), 'utf8');
const wasm = fs.readFileSync(new URL('../out/n64.wasm', import.meta.url));
const turn = () => new Promise(r => setImmediate(r));
function environment(actual = false) {
  const messages = [], records = new Map(), transactions = [];
  let fail = false, hold = null;
  const db = { transaction(_, mode) {
    const tx = { abort() { queueMicrotask(() => tx.onabort?.()); } }, staged = [];
    const request = value => { const r = {}; queueMicrotask(() => { r.result = structuredClone(value); r.onsuccess?.(); }); return r; };
    tx.objectStore = () => ({ get: k => request(records.get(k)), count: k => request(Number(records.has(k))),
      put: (v, k) => staged.push([k, structuredClone(v)]), delete: k => staged.push([k]) });
    if (mode === 'readwrite') {
      transactions.push(staged);
      queueMicrotask(() => {
        const complete = () => { if (fail) tx.onabort?.(); else { for (const [k, v] of staged) { if (v === undefined) records.delete(k); else records.set(k, v); } tx.oncomplete?.(); } };
        if (hold) hold(complete); else complete();
      });
    }
    return tx;
  } };
  const context = vm.createContext({ console: { error() {}, log() {} }, window: {}, self: { CompressionStream, DecompressionStream },
    document: { getElementById: () => ({ hidden: true, classList: { toggle() {} } }), title: '' },
    localStorage: { getItem() { return null; } }, matchMedia: () => ({ matches: false }), addEventListener() {},
    Uint8Array, Uint32Array, DataView, ArrayBuffer, WebAssembly, Blob, Response, CompressionStream, DecompressionStream,
    TextDecoder, URLSearchParams, location: { search: '' }, Promise, Map, Set, performance, setTimeout, clearTimeout, structuredClone, navigator: {} });
  vm.runInContext(source.slice(0, source.indexOf('(async function main()')), context);
  const ex = actual ? new WebAssembly.Instance(new WebAssembly.Module(wasm), { env: { host_log() {}, host_gpu_flush() {} } }).exports : {
    memory: { buffer: new ArrayBuffer(1024 * 1024) }, n64_state_size: () => 128, n64_set_rom() {}, n64_config() {}, n64_reset() {} };
  const hi = actual ? Array.from(new Uint32Array(ex.memory.buffer, ex.n64_host_info(), 24)) : [];
  if (!actual) { hi[12] = 4096; hi[13] = 6144; hi[14] = 137216; hi[16] = 400000; }
  Object.assign(context, { _ex: ex, _hi: hi, _db: db, _messages: messages });
  vm.runInContext(`ex = _ex; hi = _hi; idb = Promise.resolve(_db); this.realApplyRenderer = applyRenderer;
    toast = msg => _messages.push(msg);
    for (const name of ['applyRenderer', 'resize', 'updateTouchVisibility', 'updateFlag', 'audioStart', 'audioReset', 'wakeLock', 'pokeMenu', 'closeSheet', 'renderLibrary']) eval(name + ' = () => {}');
    updateXpak = () => ({ kind: 0, now: false }); grabThumb = async () => '';
    this.api = { loadRom, saveState, loadState, flushSaves, idbWrite, openFile, readBounded, sessionOp, goHome, resetGame,
      setRom: r => { rom = r; running = !!r; }, getRom: () => rom, running: () => running,
      dirty: () => u32v()[hi[16] >> 2], setDirty: n => { u32v()[hi[16] >> 2] = n; },
      setDB: db => { idb = Promise.resolve(db); }, battery: () => saveBlob(),
      setBattery: b => restoreSaveBlob(b), pointer: () => romPtr,
      setGpu: g => { gpu = g; }, rendererFailed: () => rendererFailed,
      setGet: fn => { idbGet = fn; }, setKeep: v => { settings.keep = v; } };`, context);
  return { api: context.api, ex, hi, messages, records, transactions, context,
    fail: v => { fail = v; }, hold: fn => { hold = fn; } };
}
const fake = (title, marker) => { const b = new Uint8Array(8192); b.set([0x80, 0x37, 0x12, 0x40]); b.set(Buffer.from(title), 0x20); b[0x10] = marker; return b; };

test('actual WASM: same-cartridge replacement restores the latest battery write', async () => {
  const e = environment(true), cartridge = fake('A', 1);
  await e.api.loadRom(cartridge, 'A.z64');
  const latest = e.api.battery(); latest.fill(0xA5); e.api.setBattery(latest); e.api.setDirty(9);
  await e.api.loadRom(cartridge, 'A.z64');
  assert.deepEqual(e.api.battery(), latest);
  assert.deepEqual(new Uint8Array(e.records.get('save:' + e.api.getRom().key)), latest);
});
test('synchronous renderer initialization failure stops the session', () => {
  const e = environment(); e.api.setRom({ key: 'A' });
  e.api.setGpu({ reset() { throw new Error('submission failed'); } });
  e.context.realApplyRenderer();
  assert.equal(e.api.rendererFailed(), true); assert.equal(e.api.running(), false);
  assert.match(e.messages.at(-1), /synchronization failed/);
});

test('transaction abort keeps dirty progress and retry commits', async () => {
  const e = environment(); e.api.setRom({ key: 'A' }); e.api.setDirty(1); e.fail(true);
  assert.equal(await e.api.flushSaves(), false); assert.equal(e.api.dirty(), 1);
  assert.equal(e.records.has('save:A'), false); assert.match(e.messages.at(-1), /failed/);
  e.fail(false); assert.equal(await e.api.flushSaves(), true); assert.equal(e.api.dirty(), 0);
  assert.equal(e.records.get('save:A').byteLength, 0x40800);
});
test('old save completion does not clear a later guest write', async () => {
  const e = environment(); e.api.setRom({ key: 'A' }); e.api.setDirty(1);
  let commit; e.hold(fn => { commit = fn; }); const pending = e.api.flushSaves(); await turn();
  e.api.setDirty(2); commit(); await pending; assert.equal(e.api.dirty(), 2);
  e.hold(null); await e.api.flushSaves(); assert.equal(e.api.dirty(), 0);
});
test('temporary storage stays dirty and is visibly labelled', async () => {
  const e = environment(); e.api.setDB(null); e.api.setRom({ key: 'A' }); e.api.setDirty(1);
  await e.api.flushSaves(); assert.equal(e.api.dirty(), 1); assert.match(e.messages.join(' '), /Temporary storage/);
  assert.equal(await e.api.saveState(), true); assert.match(e.messages.at(-1), /temporarily/);
});
test('state payload and metadata abort together; duplicate saves preserve running intent', async () => {
  const e = environment(); e.api.setRom({ key: 'A' }); e.fail(true);
  assert.equal(await e.api.saveState(), false);
  assert.equal(e.records.size, 0); assert.ok(!e.messages.some(m => /^Saved/.test(m)));
  assert.equal(e.transactions[0].length, 2); e.fail(false);
  assert.deepEqual(await Promise.all([e.api.saveState(0), e.api.saveState(1)]), [true, true]);
  assert.equal(e.api.running(), true); assert.ok(e.records.get('state:A')); assert.ok(e.records.get('smeta:A')[1]);
});
test('oversize file is rejected before arrayBuffer', async () => {
  const e = environment(); let read = false;
  await e.api.openFile({ name: 'bad.z64', size: 64 * 1024 * 1024 + 1, arrayBuffer: async () => { read = true; } });
  assert.equal(read, false); assert.match(e.messages.at(-1), /too large/);
});
test('bounded decoder cancels on first over-budget chunk', async () => {
  const e = environment(); let cancelled = false, pulled = 0;
  const stream = new ReadableStream({ pull(c) { pulled++; c.enqueue(new Uint8Array(1024)); }, cancel() { cancelled = true; } });
  await assert.rejects(e.api.readBounded(stream, 4096), /too large/);
  assert.ok(cancelled); assert.ok(pulled <= 7);
});
test('actual WASM: A/B loads queue; batteries and cached cartridge identities agree', async () => {
  const e = environment(true); let release; const barrier = new Promise(r => { release = r; });
  e.api.setGet(async key => {
    if (key.startsWith('save:A-')) await barrier;
    if (key.startsWith('save:')) { const b = new Uint8Array(0x40800); b.fill(key.startsWith('save:A-') ? 0xA1 : 0xB2); return b.buffer; }
    return structuredClone(e.records.get(key));
  });
  const a = e.api.loadRom(fake('A', 1), 'A.z64'); const b = e.api.loadRom(fake('B', 2), 'B.z64');
  await turn(); release(); await Promise.all([a, b]);
  assert.equal(e.api.getRom().name, 'B'); assert.equal(e.api.battery()[0], 0xB2); assert.equal(e.api.running(), true);
  const list = e.records.get('lib'); assert.equal(list.length, 2);
  for (const r of list) assert.equal(new Uint8Array(e.records.get('rom:' + r.key))[0x10], r.name === 'A' ? 1 : 2);
  const ptr = e.api.pointer(), bytes = new Uint8Array(e.ex.memory.buffer, ptr, 8192).slice();
  await assert.rejects(async () => e.api.loadRom(new Uint8Array(4096), 'invalid.z64'), /N64 ROM/);
  assert.equal(e.api.getRom().name, 'B'); assert.deepEqual(new Uint8Array(e.ex.memory.buffer, ptr, 8192), bytes);
  assert.equal(e.api.running(), true);
  e.api.setGet(async () => { throw new Error('storage read failed'); });
  await assert.rejects(e.api.loadRom(fake('C', 3), 'C.z64'), /storage read failed/);
  assert.equal(e.api.getRom().name, 'B'); assert.deepEqual(new Uint8Array(e.ex.memory.buffer, ptr, 8192), bytes);
  e.api.setKeep(false);
  await Promise.all([e.api.resetGame(), e.api.goHome()]); assert.equal(e.api.getRom(), null); assert.equal(e.api.running(), false);
});
