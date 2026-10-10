// Run authored synthetic batches on the actual WGSL pipelines. GPU memory and
// feedback persist across batches; software expectations are never uploaded.
window.runCustom = async cfg => {
  const wasm = await (await fetch('/out/n64.wasm')).arrayBuffer();
  const rom = new Uint8Array(await (await fetch(cfg.rom)).arrayBuffer());
  const core = await mkCore(wasm, rom, () => {}), sh = { allowFallback: true };
  for (const n of ['rdp', 'vi', 'merge']) sh[n] = await (await fetch(`/src/web/${n}.wgsl`)).text();
  const gpu = await N64Gpu.create(core, null, sh); if (!gpu) throw new Error('no adapter'); gpu.device.pushErrorScope('validation'); gpu.reset();
  if (cfg.hd === 0) await gpu.setScale(0, true);
  const batches = await (await fetch('/out/rdp-vectors.json')).json();
  if (!Array.isArray(batches) || batches.length !== 14) throw new Error('missing synthetic batches (expected fourteen)');
  let checks = 0;
  for (const b of batches) {
    const mem = core.ex.memory.buffer;
    for (const [name, hi] of [['info', 6], ['prims', 7], ['spans', 8], ['states', 9], ['tiles', 10], ['tmem', 11]])
      new Uint32Array(mem, core.hi[hi], b[name].length).set(b[name]);
    new Uint32Array(mem, gpu.p.bins, b.bins.length).set(b.bins);
    gpu.flush(); await gpu.device.queue.onSubmittedWorkDone();
    const set = cfg.hd === 0 ? gpu.hd : gpu.native;
    const idx = b.info[3] * (b.info[0] === 4 ? 2 : 1);
    const readCircular = async (index, n) => {
      index &= 0x3FFFFF; const tail = Math.min(n, 0x400000 - index), out = new Uint32Array(n);
      out.set(await gpu.readBuf(set.target, index * 4, tail * 4));
      if (n > tail) out.set(await gpu.readBuf(set.target, 0, (n - tail) * 4), tail);
      return out;
    };
    const actual = await readCircular(idx, b.expected.length);
    for (let i = 0; i < actual.length; i++) { if ((actual[i] & 0x3FFFF) !== b.expected[i]) throw new Error(`batch ${checks}: framebuffer ${i} ${actual[i]} != ${b.expected[i]}`); }
    if (b.info[15] & 2) {
      const depth = await readCircular(b.info[4], b.depth.length);
      for (let i = 0; i < depth.length; i++) if ((depth[i] & 0x3FFFF) !== b.depth[i]) throw new Error(`batch ${checks}: depth ${i} ${depth[i]} != ${b.depth[i]}`);
    }
    const state = await gpu.readBuf(set.feedbackBuf, 0, 48);
    for (let i = 0; i < 12; i++) if ((state[i] | 0) !== b.feedback[i]) throw new Error(`batch ${checks}: feedback ${i} ${state[i] | 0} != ${b.feedback[i]}`);
    checks++;
  }
  const error = await gpu.device.popErrorScope(); if (error) throw new Error(error.message);
  return { mode: 'rdp-vectors', batches: checks, halfwords: checks * 64, registers: checks * 12,
    adapter: gpu.adapter.info, coverage: 'authored batches; framebuffer/hidden bits and all twelve retained registers' };
};
