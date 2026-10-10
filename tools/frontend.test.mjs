import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { createHash } from 'node:crypto';
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
        if (hold) hold(complete); else queueMicrotask(complete);
      });
    }
    return tx;
  } };
  const context = vm.createContext({ console: { error() {}, log() {}, warn() {} }, window: {}, self: { CompressionStream, DecompressionStream },
    document: { getElementById: () => ({ hidden: true, focus() {}, classList: { toggle() {} } }), title: '' },
    localStorage: { getItem() { return null; } }, matchMedia: () => ({ matches: false }), addEventListener() {},
    Uint8Array, Uint32Array, DataView, ArrayBuffer, WebAssembly, Blob, Response, CompressionStream, DecompressionStream,
    TextEncoder, TextDecoder, URLSearchParams, location: { search: '' }, Promise, Map, Set, performance, setTimeout, clearTimeout, structuredClone, navigator: {} });
  vm.runInContext(source.slice(0, source.indexOf('(async function main()')), context);
  const ex = actual ? new WebAssembly.Instance(new WebAssembly.Module(wasm), { env: { host_log() {}, host_gpu_flush() {} } }).exports : {
    memory: { buffer: new ArrayBuffer(1024 * 1024) }, n64_state_size: () => 128, n64_set_rom() {}, n64_config() {}, n64_reset() {} };
  const hi = actual ? Array.from(new Uint32Array(ex.memory.buffer, ex.n64_host_info(), 24)) : [];
  if (!actual) { hi[12] = 4096; hi[13] = 6144; hi[14] = 137216; hi[16] = 400000; }
  Object.assign(context, { _ex: ex, _hi: hi, _db: db, _messages: messages });
  vm.runInContext(`ex = _ex; hi = _hi; idb = Promise.resolve(_db); this.realApplyRenderer = applyRenderer;
    toast = msg => _messages.push(msg);
    this.realResize = resize;
    for (const name of ['applyRenderer', 'resize', 'updateTouchVisibility', 'updateFlag', 'audioStart', 'audioReset', 'wakeLock', 'pokeMenu', 'closeSheet', 'renderLibrary', 'releaseInput']) eval(name + ' = () => {}');
    updateXpak = () => ({ kind: 0, now: false }); grabThumb = async () => '';
    this.api = { packPortable, unpackPortable, captureState, loadStateNow, setSaveMedium, restoreMediumBackup, romDigest, romIdentity, stateMeta, loadRom, saveState, loadState, flushSaves, idbWrite, openFile, readBounded, sessionOp, goHome, resetGame,
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
    if (key === 'save:sha256:' + createHash('sha256').update(fake('A', 1)).digest('hex')) await barrier;
    if (key.startsWith('save:')) { const b = new Uint8Array(0x40800); b.fill(key === 'save:sha256:' + createHash('sha256').update(fake('A', 1)).digest('hex') ? 0xA1 : 0xB2); return b.buffer; }
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

// Exercise the actual presentation function rather than a simulated callback.
test('synchronous presentation failure stops fields and preserves battery export', () => {
  const e = environment(); e.api.setRom({ key: 'A' });
  vm.runInContext(`readInput = () => {}; ex.n64_frame = () => 0; useGpu = true;
    gpu = { present() { throw new Error('presentation failed'); } };
    this.presentResult = emulate(true, false);`, e.context);
  assert.equal(e.context.presentResult, false);
  assert.equal(e.api.running(), false); assert.equal(e.api.rendererFailed(), true);
  assert.equal(e.api.battery().length, 0x40800);
});

test('presentation resize preserves high-DPI aspect ratio while limiting allocation', () => {
  const e = environment();
  for (const [width, height, dpr] of [[3440, 1440, 2], [3840, 2160, 3], [640, 360, 2], [0, 0, 1]]) {
    const cv = { width: 0, height: 0 }, sw = { style: {} }, screen = { clientWidth: width, clientHeight: height };
    e.context.document.getElementById = id => ({ 'screen': screen, 'cv-gpu': cv, 'cv-sw': sw })[id];
    e.context.window.devicePixelRatio = dpr;
    vm.runInContext('touchLayout = () => {}; settings.aspect = 0; realResize();', e.context);
    assert.ok(cv.width >= 1 && cv.width <= 4096 && cv.height >= 1 && cv.height <= 4096);
    assert.ok(Math.abs(cv.width * height - cv.height * width) <= height + width);
    if (width && height) assert.ok(Math.abs(parseFloat(sw.style.width) * 3 - parseFloat(sw.style.height) * 4) < 1);
  }
});


test('actual WASM: dirty reset survives power cycle; aborted reset retains progress', async () => {
  const e = environment(true); await e.api.loadRom(fake('reset', 1), 'reset.z64');
  const bytes = e.api.battery(); bytes.fill(0xA5); e.api.setBattery(bytes); e.api.setDirty(7);
  e.fail(true); assert.equal(await e.api.resetGame(), false); assert.equal(e.api.dirty(), 7);
  e.fail(false); await e.api.resetGame(); await e.api.loadRom(fake('reset', 1), 'reset.z64');
  assert.deepEqual(e.api.battery(), bytes);
});
test('actual WASM: state restore drains older saves and makes restored battery durable', async () => {
  const e = environment(true); await e.api.loadRom(fake('state', 1), 'state.z64');
  let bytes = e.api.battery(); bytes.fill(0x11); e.api.setBattery(bytes); e.api.setDirty(1); await e.api.flushSaves(); await e.api.saveState();
  bytes.fill(0x22); e.api.setBattery(bytes); e.api.setDirty(2);
  let commit; e.hold(fn => { commit = fn; }); const pending = e.api.flushSaves(); await turn();
  const restore = e.api.loadState(); commit(); await pending; e.hold(null); assert.equal(await restore, true);
  assert.equal(e.api.dirty(), 1); assert.equal(e.api.battery()[0], 0x11); await e.api.flushSaves();
  await e.api.loadRom(fake('state', 1), 'state.z64'); assert.equal(e.api.battery()[0], 0x11);
});
test('actual WASM: a guest write after restored-state autosave remains durable', async () => {
  const e = environment(true), cartridge = fake('restore-write', 1);
  await e.api.loadRom(cartridge, 'restore-write.z64');
  const bytes = e.api.battery(); bytes.fill(0); e.api.setBattery(bytes);
  await e.api.saveState(); assert.equal(await e.api.loadState(), true);
  assert.equal(e.api.dirty(), 1);
  let commit; e.hold(fn => { commit = fn; }); const pending = e.api.flushSaves(); await turn();
  assert.equal(e.transactions.at(-1)[0][1].byteLength, 0x40800);
  new Uint8Array(e.ex.memory.buffer)[e.hi[12]] = 0x5A;
  // Same monotonic, nonzero generation contract as the guest EEPROM command.
  e.api.setDirty((e.api.dirty() + 1) >>> 0 || 1);
  commit(); await pending; e.hold(null); await e.api.flushSaves();
  await e.api.loadRom(cartridge, 'restore-write.z64');
  assert.equal(e.api.battery()[0], 0x5A);
});
test('canonical SHA256 matches independent digest across byte orders and payload variants', async () => {
  const e = environment(), a = fake('same', 7), expected = createHash('sha256').update(a).digest('hex');
  for (const xor of [0, 1, 3]) { const swapped = Uint8Array.from(a, (_, i) => a[i ^ xor]); assert.equal(await e.api.romDigest(swapped), expected); }
  const b = a.slice(); b[5000] = 99; assert.notEqual(await e.api.romDigest(b), expected);
});
test('same header with different content has distinct caches and state keys', async () => {
  const e = environment(true), a = fake('same', 7), b = a.slice(); b[5000] = 99;
  await e.api.loadRom(a, 'same.z64'); const first = e.api.getRom().key; await e.api.saveState();
  await e.api.loadRom(b, 'same.z64'); assert.notEqual(e.api.getRom().key, first);
  assert.equal(e.records.get('lib').length, 2); assert.equal(new Uint8Array(e.records.get('rom:' + first))[5000], 0);
  await e.api.loadState(); assert.match(e.messages.at(-1), /empty/);
});
test('verified legacy migration is atomic and retains a recoverable original', async () => {
  const e = environment(true), data = fake('legacy', 1), old = e.api.romIdentity(data, 'legacy.z64').legacyKey;
  const save = new Uint8Array(0x40800).fill(0x5A).buffer;
  e.records.set('rom:' + old, data.buffer); e.records.set('save:' + old, save);
  e.fail(true); await assert.rejects(e.api.loadRom(data, 'legacy.z64'), /migration/);
  assert.equal(e.records.has('save:' + old), true); e.fail(false);
  await e.api.loadRom(data, 'legacy.z64'); assert.equal(e.api.battery()[0], 0x5A); assert.ok(e.records.has('save:' + old));
});
test('latest file selection wins even when the first read finishes last', async () => {
  const e = environment(true); let finish;
  const first = e.api.openFile({ name: 'A.z64', size: 8192, arrayBuffer: () => new Promise(r => { finish = r; }) });
  await turn(); const second = e.api.openFile({ name: 'B.z64', size: 8192, arrayBuffer: async () => fake('B', 2).buffer });
  finish(fake('A', 1).buffer); await Promise.all([first, second]); assert.equal(e.api.getRom().name, 'B'); assert.equal(e.records.get('lib').length, 1);
});
test('state slot existence never reads compressed payloads', async () => {
  const e = environment(); e.api.setRom({ key: 'A' }); e.records.set('state:A', new ArrayBuffer(100));
  e.api.setGet(async k => { assert.ok(!k.startsWith('state:')); return e.records.get(k); });
  assert.equal((await e.api.stateMeta())[0].t, 0);
});
test('obsolete scale failure cannot override a newer successful request', async () => {
  const e = environment(); let reject; const calls = [];
  e.context.document.getElementById = () => ({ options: [{ value: '0' }, { value: '1' }, { value: '2' }] });
  e.api.setGpu({ maxScaleLog2: 2, setScale: n => { calls.push(n); return n === 1 ? new Promise((_, r) => { reject = r; }) : Promise.resolve(n); } });
  vm.runInContext('useGpu=true; settings.scale=1; this.first=applyScale(); settings.scale=2; this.second=applyScale();', e.context);
  await e.context.second; reject(new Error('old allocation failed')); await e.context.first;
  assert.deepEqual(calls, [1, 2]); assert.equal(vm.runInContext('settings.scale', e.context), 2);
});

test('audio initialization closes partial context and succeeds on retry', async () => {
  const e = environment(); let created = 0, closed = 0;
  vm.runInContext('setVolume = () => {};', e.context);
  e.context.window.AudioContext = class {
    constructor() { this.number = ++created; this.state = 'suspended'; this.sampleRate = 48000; }
    createGain() { return { gain: {}, connect() {}, disconnect() {} }; }
    createScriptProcessor() { if (this.number === 1) throw new Error('output refused'); return { connect() {}, disconnect() {} }; }
    async resume() { this.state = 'running'; } async close() { closed++; this.state = 'closed'; }
  };
  await vm.runInContext('startAudio()', e.context); assert.equal(closed, 1); assert.equal(vm.runInContext('audioReady', e.context), false);
  await vm.runInContext('startAudio()', e.context); assert.equal(created, 2); assert.equal(vm.runInContext('audioReady', e.context), true);
});
test('audio resampler reports underruns and bounded overruns at 44.1/48 kHz', () => {
  const e = environment();
  for (const rate of [44100,48000]) {
    const r = vm.runInContext(`new Resampler(${rate})`, e.context), samples = new Int16Array(8000).fill(2000), l = new Float32Array(4096), right = new Float32Array(4096);
    r.push(samples, 32000); r.pull(l,right); r.pull(l,right); assert.ok(r.underruns > 0); assert.ok(l.every(Number.isFinite));
    r.push(new Int16Array(40000),32000); assert.ok(r.overruns > 0); assert.ok(r.wr-r.rd <= r.size);
  }
});
test('four controllers retain ports through disconnect and reconnect', () => {
  const e = environment(), inputs = [], configs = [];
  e.ex.n64_input = (...x) => inputs.push(x); e.ex.n64_config = (...x) => configs.push(x);
  let pads = Array.from({ length: 4 }, (_, index) => ({ index, id: 'pad'+index, connected: true, axes: [0,0], buttons: Array.from({ length: 16 }, (_, i) => ({ pressed: i === 0 })) }));
  e.context.navigator.getGamepads = () => pads;
  vm.runInContext('readInput()', e.context); assert.deepEqual(inputs.map(x=>x[0]),[0,1,2,3]); assert.ok(inputs.every(x=>x[1]===0x8000));
  pads[0].connected = false; inputs.length=0; vm.runInContext('readInput()', e.context); assert.equal(inputs[0][1],0); assert.equal(inputs[1][1],0x8000);
  pads[0].connected = true; inputs.length=0; vm.runInContext('readInput()', e.context); assert.ok(inputs.every(x=>x[1]===0x8000));
});
test('cross-tab cartridge lock rejects a competing writer before replacing the game', async () => {
  const e = environment(true), locked = new Set();
  e.context.navigator.locks = { request: async (name, _, cb) => {
    if (locked.has(name)) return cb(null); locked.add(name); try { await cb({ name }); } finally { locked.delete(name); }
  } };
  const a = fake('locked',1); const key = 'sha256:' + await e.api.romDigest(a); locked.add('photon64:'+key);
  await assert.rejects(e.api.loadRom(a,'locked.z64'), /another tab/); assert.equal(e.api.getRom(),null);
  locked.clear(); await e.api.loadRom(a,'locked.z64'); e.api.setKeep(false); await e.api.goHome(); await turn(); assert.equal(locked.size,0);
});

test('portable files reject cartridge and core mismatches before mutation', async () => {
  const e = environment(true); await e.api.loadRom(fake('portable',1), 'portable.z64');
  const battery = e.api.battery(), packed = e.api.packPortable('battery',battery);
  assert.deepEqual(e.api.unpackPortable(packed,'battery').bytes,battery);
  const state = e.api.packPortable('state',await e.api.captureState());
  assert.ok(e.api.unpackPortable(state,'state').bytes.length > 16);
  assert.throws(() => e.api.unpackPortable(packed,'state'), /different cartridge/);
  const wrong = e.api.packPortable('state',new Uint8Array(16),{build: 9});
  assert.throws(() => e.api.unpackPortable(wrong,'state'), /core build/);
  await e.api.loadRom(fake('other',2), 'other.z64');
  assert.throws(() => e.api.unpackPortable(packed,'battery'), /different cartridge/);
  assert.throws(() => e.api.unpackPortable(new Uint8Array(8),'battery'), /Invalid/);
});
test('save-medium backup restores atomically and keeps replaced progress recoverable', async () => {
  const e = environment(true); await e.api.loadRom(fake('backup',1), 'backup.z64');
  const b = e.api.battery(); b.fill(0x35); e.api.setBattery(b); e.api.setDirty(1);
  await e.api.setSaveMedium(3); const key=e.api.getRom().key;
  assert.equal(e.records.get('backup:'+key).bytes.byteLength,0x40800);
  b.fill(0x76); e.api.setBattery(b); e.api.setDirty(1); await e.api.flushSaves();
  e.fail(true); await assert.rejects(e.api.restoreMediumBackup(), /durably restore/);
  assert.equal(e.api.battery()[0],0x76); assert.equal(e.api.getRom().saveType,3);
  e.fail(false); await e.api.restoreMediumBackup(); assert.equal(e.api.battery()[0],0x35);
  assert.equal(new Uint8Array(e.records.get('save:'+key))[0],0x35);
  assert.equal(new Uint8Array(e.records.get('backup:'+key).bytes)[0],0x76);
});
test('actual WASM ROM reserve plateaus across repeated 8/32/64 MiB imports', () => {
  const e=environment(true), initial=e.ex.memory.buffer.byteLength, measurements=[];
  let ptr;
  for(const size of [8,32,64,8,64,32]) {
    const next=e.ex.n64_reserve_rom(size*1048576); assert.ok(next); if(ptr)assert.equal(next,ptr);ptr=next;
    measurements.push(e.ex.memory.buffer.byteLength);
    new Uint8Array(e.ex.memory.buffer,next,size*1048576)[size*1048576-1]=0x55;
  }
  assert.ok(measurements[0]<initial+20*1048576);
  assert.deepEqual(measurements.slice(3),[measurements[2],measurements[2],measurements[2]]);
});

test('public queued loads snapshot caller bytes while owned file loads avoid a second payload copy', async () => {
  const e=environment(true), data=fake('snapshot',1), expected=await e.api.romDigest(data);
  const load=e.api.loadRom(data,'snapshot.z64'); data[4096]^=0xff; await load;
  assert.equal(e.api.getRom().digest,expected);
  const owned=fake('owned',2); owned.slice=()=>{throw new Error('unexpected duplicate ROM payload')};
  e.api.setKeep(false); await e.api.loadRom(owned,'owned.z64',false,undefined,true);
  assert.equal(e.api.getRom().name,'owned');
});

test('lost renderer recovery releases the old device before software fallback', () => {
  const e=environment();const calls=[];
  e.api.setGpu({dispose(){calls.push('dispose')},device:{destroy(){calls.push('destroy')}},onError(){},onLost(){}});
  vm.runInContext("gpuErr='device lost'; dropLostGpu(); dropLostGpu();",e.context);
  assert.deepEqual(calls,['dispose','destroy']);
});
