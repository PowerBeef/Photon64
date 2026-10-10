// WebGPU back end: RDP compute pipeline, VI presentation and CPU <-> GPU framebuffer coherency.
//
// The emulated RDRAM framebuffer lives in a GPU storage buffer (one u32 per 16-bit RDRAM halfword: data in the low
// 16 bits, the two "hidden" 9th bits above). The WASM core emits batches of primitives; each batch is one compute
// dispatch that shades every covered pixel with the exact integer pipeline. Display is produced by the VI passes
// directly from GPU memory, so nothing is read back unless the game asks for it. CPU-side framebuffer writes are
// pushed to the GPU lazily. When the emulated machine touches memory the GPU has drawn into (a game reading its
// depth buffer for lens flares, a blur made from the last picture, ...) the core stops in front of that access,
// syncNow() copies the GPU's results back, and the core carries on: the game always sees exact, current data.
//
// Optional high-resolution mode: every batch is rasterized a second time at 2x / 4x into a display-only copy of
// framebuffer memory (S*S sub-pixels per emulated halfword). Emulated memory keeps receiving the exact native result.
const FB_WORDS = 0x400000, RDRAM_MASK = 0x7FFFFF;
const AA_W = 656, AA_H = 592, OUT_W = 640, OUT_H = 576;

class N64Gpu {
  // tiny preprocessor for the WGSL sources:  //#if HD ... //#else ... //#endif   and  __HD_L__
  static pp(src, hd, L, mask) {
    const out = []; const st = [];
    for (const line of src.split('\n')) {
      const t = line.trim();
      if (t.startsWith('//#if ')) { st.push(t.slice(6).trim() === 'HD' ? hd : false); continue; }
      if (t === '//#else') { st[st.length - 1] = !st[st.length - 1]; continue; }
      if (t === '//#endif') { st.pop(); continue; }
      if (st.every(Boolean)) out.push(line);
    }
    return out.join('\n').replace(/__HD_L__/g, String(L | 0)).replace(/__FB_MASK__/g, String(mask === undefined ? FB_WORDS - 1 : mask));
  }

  static async create(core, canvas, shaders) {
    if (!navigator.gpu) return null;
    let adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) adapter = await navigator.gpu.requestAdapter();
    if (!adapter) return null;
    // a CPU-emulated adapter is always slower than the built-in software renderer
    const info = adapter.info || {};
    if ((adapter.isFallbackAdapter || info.isFallbackAdapter) && !(shaders && shaders.allowFallback)) throw new Error('only a software WebGPU adapter is available');
    const lim = adapter.limits;
    if (lim.maxStorageBuffersPerShaderStage < 7 || lim.maxStorageBufferBindingSize < FB_WORDS * 4) return null;
    // the 4x high-resolution framebuffer copy needs a 256 MB binding; ask for it where the adapter has it
    const want = FB_WORDS * 4 * 16, requiredLimits = {};
    if (lim.maxStorageBufferBindingSize >= want && lim.maxBufferSize >= want) { requiredLimits.maxStorageBufferBindingSize = want; requiredLimits.maxBufferSize = want; }
    let device;
    try { device = await adapter.requestDevice({ requiredLimits }); } catch (e) { device = await adapter.requestDevice(); }
    const g = new N64Gpu();
    g.adapter = adapter;
    await g.init(device, core, canvas, shaders);
    return g;
  }

  async module(name, hd, L) {
    const m = this.device.createShaderModule({ code: N64Gpu.pp(this.shaders[name], hd, L, hd ? this.hdWords - 1 : FB_WORDS - 1) });
    const info = await m.getCompilationInfo();
    const errs = info.messages.filter(x => x.type === 'error');
    if (errs.length) throw new Error(`WGSL ${name}: ` + errs.slice(0, 8).map(e => `${e.lineNum}:${e.linePos} ${e.message}`).join(' | '));
    return m;
  }

  // pipelines + bind groups for one resolution (L = log2 scale; hd = false is the exact native set)
  async buildSet(hd, L) {
    const device = this.device, S = 1 << L, T = GPUTextureUsage;
    const [rdpMod, viMod, mergeMod] = await Promise.all([this.module('rdp', hd, L), this.module('vi', hd, L), this.module('merge', hd, L)]);
    const tri = { topology: 'triangle-list' };
    const [rdp, merge, aa, scale] = await Promise.all([
      device.createComputePipelineAsync({ layout: 'auto', compute: { module: rdpMod, entryPoint: 'main' } }),
      device.createComputePipelineAsync({ layout: 'auto', compute: { module: mergeMod, entryPoint: 'main' } }),
      device.createRenderPipelineAsync({ layout: 'auto', vertex: { module: viMod, entryPoint: 'vs_full' },
        fragment: { module: viMod, entryPoint: 'fs_aa', targets: [{ format: 'rgba8uint' }] }, primitive: tri }),
      device.createRenderPipelineAsync({ layout: 'auto', vertex: { module: viMod, entryPoint: 'vs_full' },
        fragment: { module: viMod, entryPoint: 'fs_scale', targets: [{ format: 'rgba8unorm' }] }, primitive: tri }),
    ]);
    const set = { hd, L, S, rdp, merge, aa, scale };
    const bufE = (binding, buffer) => ({ binding, resource: { buffer } });
    set.target = hd ? device.createBuffer({ size: this.hdWords * 4 * S * S, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST }) : this.fbBuf;
    set.aaTex = device.createTexture({ size: [AA_W * S, AA_H * S], format: 'rgba8uint', usage: T.RENDER_ATTACHMENT | T.TEXTURE_BINDING });
    set.outTex = device.createTexture({ size: [OUT_W * S, OUT_H * S], format: 'rgba8unorm', usage: T.RENDER_ATTACHMENT | T.TEXTURE_BINDING | T.COPY_SRC });
    set.aaView = set.aaTex.createView(); set.outView = set.outTex.createView();
    set.feedbackBuf = device.createBuffer({ size: (12 + 4096 * 12) * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST });
    set.ordered = await device.createComputePipelineAsync({ layout: 'auto', compute: { module: rdpMod, entryPoint: 'ordered_main' } });
    set.resolve = await device.createComputePipelineAsync({ layout: 'auto', compute: { module: rdpMod, entryPoint: 'resolve_feedback' } });
    const orderedEntries = [bufE(0, this.ubuf), bufE(1, set.target), bufE(2, this.primsBuf), bufE(3, this.spansBuf),
      bufE(4, this.tablesBuf), bufE(5, this.tmemBuf), bufE(7, set.feedbackBuf)];
    set.orderedBG = device.createBindGroup({ layout: set.ordered.getBindGroupLayout(0), entries: orderedEntries });
    set.resolveBG = device.createBindGroup({ layout: set.resolve.getBindGroupLayout(0), entries: [bufE(0, this.ubuf), bufE(2, this.primsBuf), bufE(4, this.tablesBuf), bufE(7, set.feedbackBuf)] });
    set.rdpBG = device.createBindGroup({ layout: rdp.getBindGroupLayout(0), entries: [bufE(0, this.ubuf), bufE(1, set.target), bufE(2, this.primsBuf),
      bufE(3, this.spansBuf), bufE(4, this.tablesBuf), bufE(5, this.tmemBuf), bufE(6, this.binsBuf), bufE(7, set.feedbackBuf)] });
    set.mergeBG = device.createBindGroup({ layout: merge.getBindGroupLayout(0),
      entries: hd ? [bufE(0, this.mergeUbuf), bufE(1, this.fbBuf), bufE(2, this.stageBuf), bufE(3, set.target)] : [bufE(0, this.mergeUbuf), bufE(1, this.fbBuf), bufE(2, this.stageBuf)] });
    if (hd) {
      set.fill = await device.createComputePipelineAsync({ layout: 'auto', compute: { module: mergeMod, entryPoint: 'fill' } });
      set.fillBG = device.createBindGroup({ layout: set.fill.getBindGroupLayout(0), entries: [bufE(0, this.mergeUbuf), bufE(1, this.fbBuf), bufE(3, set.target)] });
    }
    set.aaBG = device.createBindGroup({ layout: aa.getBindGroupLayout(0), entries: [bufE(0, this.viUbuf), bufE(1, set.target)] });
    set.scaleBG = device.createBindGroup({ layout: scale.getBindGroupLayout(0), entries: [bufE(0, this.viUbuf), { binding: 2, resource: set.aaView }] });
    set.blitBG = this.samplers.map(s => device.createBindGroup({ layout: this.blitPipe.getBindGroupLayout(0),
      entries: [bufE(0, this.viUbuf), { binding: 3, resource: set.outView }, { binding: 4, resource: s }] }));
    set.lastOutH = 0;
    return set;
  }

  async init(device, core, canvas, shaders) {
    this.device = device; this.core = core; this.ex = core.ex; this.mem = core.ex.memory; this.canvas = canvas; this.shaders = shaders;
    const ex = this.ex;
    const hi = new Uint32Array(this.mem.buffer, ex.n64_host_info(), 24);
    const sp = new Uint32Array(this.mem.buffer, ex.n64_sync_ptrs(), 8);
    const tables = ex.n64_gpu_tables();   // also makes sure the blender LUT exists before it is uploaded below
    this.p = { rdram: hi[0], vi_state: hi[2], b_info: hi[6], b_prims: hi[7], b_spans: hi[8], b_states: hi[9], b_tiles: hi[10], b_tmem: hi[11],
      feedback: ex.n64_rdp_feedback(), feedbackDirty: sp[7], lut: hi[17], stage: sp[0], runs: sp[1], nruns: sp[2], bins: sp[3], syncMax: sp[5], hint: sp[6], tables };
    this.scratchWords = 1 << 21;
    this.scratch = ex.n64_scratch();      // (a fixed area in the core's memory, independent of which ROM is loaded)
    if (!this.scratch) throw new Error('out of memory');
    const B = GPUBufferUsage;
    const mk = (size, usage) => device.createBuffer({ size, usage });
    this.fbBuf = mk(FB_WORDS * 4, B.STORAGE | B.COPY_SRC | B.COPY_DST);
    this.primsBuf = mk(4096 * 72 * 4, B.STORAGE | B.COPY_DST);
    this.spansBuf = mk((65536 + 8) * 8 * 4, B.STORAGE | B.COPY_DST);
    this.tablesBuf = mk((82048 + 32) * 4, B.STORAGE | B.COPY_DST);
    this.tmemBuf = mk(256 * 1024 * 4, B.STORAGE | B.COPY_DST);
    this.binsBuf = mk((2 * 128 * 128 + (1 << 20)) * 4, B.STORAGE | B.COPY_DST);
    this.stageBuf = mk(this.p.syncMax * 4, B.STORAGE | B.COPY_DST);
    this.ubuf = mk(80, B.UNIFORM | B.COPY_DST);
    this.mergeUbuf = mk(16, B.UNIFORM | B.COPY_DST);
    this.viUbuf = mk(144, B.UNIFORM | B.COPY_DST);
    // constant tables
    const q = device.queue, buf = this.mem.buffer;
    q.writeBuffer(this.tablesBuf, 73728 * 4, buf, this.p.lut, 0x8000);
    q.writeBuffer(this.tablesBuf, 81920 * 4, buf, this.p.tables, 160 * 4);

    this.format = navigator.gpu.getPreferredCanvasFormat();
    if (canvas) {
      this.ctx = canvas.getContext('webgpu');
      this.ctx.configure({ device, format: this.format, alphaMode: 'opaque' });
    }
    const blitMod = await this.module('vi', false, 0);
    this.blitPipe = await device.createRenderPipelineAsync({ layout: 'auto', vertex: { module: blitMod, entryPoint: 'vs_blit' },
      fragment: { module: blitMod, entryPoint: 'fs_blit', targets: [{ format: this.format }] }, primitive: { topology: 'triangle-strip' } });
    this.samplers = ['linear', 'nearest'].map(f => device.createSampler({ magFilter: f, minFilter: f, addressModeU: 'clamp-to-edge', addressModeV: 'clamp-to-edge' }));
    this.native = await this.buildSet(false, 0);
    this.hd = null;            // active high-resolution set (or null)
    this.hdSets = new Map();   // built sets by L
    // (tests may shrink hdWords to try 4x on adapters with a small binding limit; memory then aliases)
    this.hdWords = shaders.hdWords || FB_WORDS;
    this.maxScaleLog2 = device.limits.maxStorageBufferBindingSize >= this.hdWords * 64 ? 2 : device.limits.maxStorageBufferBindingSize >= this.hdWords * 16 ? 1 : 0;

    this.touched = new Map(); this.inflight = []; this.pool = [];
    this.exact = true; this.filter = 1;   // 0 nearest, 1 sharp bilinear, 2 linear
    this.speculate = 0;                   // > 0 while the game has recently asked for GPU results
    this.viU = new ArrayBuffer(144); this.viU32 = new Uint32Array(this.viU); this.viF32 = new Float32Array(this.viU);
    this.stats = { batches: 0, uploads: 0, merges: 0, readbacks: 0, syncs: 0, waits: 0, late: 0 };
    this.lost = false; this.pending = 0; this.done = 0;
    device.lost.then(info => { this.lost = true; this.readbackError = new Error('GPU device lost: ' + (info.message || info.reason)); if (this.onLost) this.onLost(info); });
    // (nothing here switches the core to GPU rendering: the owner calls reset() when it wants that)
  }

  // Select the internal resolution: 0 = native (exact), 1 = 2x, 2 = 4x. Builds the pipelines on first use.
  // hdTest: build the "HD" code path at 1x (used to validate the in-shader span setup against the native pass).
  async setScale(L, hdTest) {
    L = Math.min(L | 0, this.maxScaleLog2);
    const seq = this.scaleSeq = (this.scaleSeq | 0) + 1;
    const free = keep => { for (const [k, s] of this.hdSets) if (k !== keep) { s.feedbackBuf.destroy(); s.target.destroy(); s.aaTex.destroy(); s.outTex.destroy(); this.hdSets.delete(k); } };
    if (!L && !hdTest) {
      this.ex.n64_config(7, 0);       // (flushes what is pending while the high-resolution copy is still attached)
      this.hd = null; this.scaleLog2 = 0; free(-1);
      return 0;
    }
    let set = this.hdSets.get(L);
    if (!set) {
      // the big allocations can fail on small GPUs: catch that instead of rendering garbage
      this.device.pushErrorScope('out-of-memory'); this.device.pushErrorScope('validation');
      let err = null;
      try { set = await this.buildSet(true, L); } catch (e) { err = e; }
      const e1 = await this.device.popErrorScope(), e2 = await this.device.popErrorScope();
      err = err || e1 || e2;
      if (err || seq !== this.scaleSeq) {
        if (set) { set.feedbackBuf.destroy(); set.target.destroy(); set.aaTex.destroy(); set.outTex.destroy(); }
        if (err) throw new Error(err.message || String(err));
        return this.scaleLog2 | 0;     // superseded by a newer request
      }
      this.hdSets.set(L, set);
    }
    free(L);
    this.ex.n64_config(7, 1);
    this.hd = set; this.scaleLog2 = L;
    this.fillHd();
    return L;
  }

  // a freshly selected high-resolution copy starts as the emulated-resolution GPU memory, replicated
  fillHd() {
    const q = this.device.queue, set = this.hd;
    q.writeBuffer(this.mergeUbuf, 0, new Uint32Array([0, FB_WORDS, FB_WORDS - 1, 0]));
    const enc = this.device.createCommandEncoder();
    enc.copyBufferToBuffer(this.native.feedbackBuf, 0, set.feedbackBuf, 0, 48);
    const pass = enc.beginComputePass();
    pass.setPipeline(set.fill); pass.setBindGroup(0, set.fillBG); pass.dispatchWorkgroups(FB_WORDS / 256); pass.end();
    q.submit([enc.finish()]);
  }

  // staged halfwords -> GPU (and the high-resolution copy when active)
  merge(idx, count) {
    const q = this.device.queue, set = this.hd || this.native;
    q.writeBuffer(this.stageBuf, 0, this.mem.buffer, this.p.stage, count * 4);
    q.writeBuffer(this.mergeUbuf, 0, new Uint32Array([idx, count, FB_WORDS - 1, 0]));
    const enc = this.device.createCommandEncoder(), pass = enc.beginComputePass();
    pass.setPipeline(set.merge); pass.setBindGroup(0, set.mergeBG); pass.dispatchWorkgroups(Math.ceil(count / 256)); pass.end();
    q.submit([enc.finish()]);
    this.stats.merges++;
  }

  // exact = the game sees what the GPU drew (the core stops and waits for a copy-back when it looks);
  // off = it never waits and reads old contents instead
  setExact(on) { this.exact = !!on; this.ex.n64_config(8, on ? 1 : 0); if (!on) this.ex.n64_sync_done(); }

  // (re)start GPU rendering: GPU memory becomes an exact copy of emulated RDRAM (data + hidden bits)
  reset() {
    if (this.lost) throw this.readbackError || new Error('GPU device lost');
    this.generation = (this.generation || 0) + 1;
    for (const j of this.inflight) j.dead = true;
    this.readbackError = null; this.tail = Promise.resolve();
    this.ex.n64_config(0, 1); this.ex.n64_config(8, this.exact ? 1 : 0); this.ex.n64_config(7, this.hd ? 1 : 0);   // (a loaded save state may carry other settings)
    this.ex.n64_sync_done();
    const n = this.p.syncMax, q = this.device.queue;
    for (let i = 0; i < FB_WORDS; i += n) {
      this.ex.n64_sync_full(i, n);
      if (this.hd) this.merge(i, n); else q.writeBuffer(this.fbBuf, i * 4, this.mem.buffer, this.p.stage, n * 4);
    }
    q.writeBuffer(this.native.feedbackBuf, 0, this.mem.buffer, this.p.feedback, 48);
    if (this.hd) q.writeBuffer(this.hd.feedbackBuf, 0, this.mem.buffer, this.p.feedback, 48);
    new Uint32Array(this.mem.buffer)[this.p.feedbackDirty >> 2] = 0;
    this.touched.clear();
    for (const j of this.inflight) j.dead = true;
  }

  // push CPU-side changes of halfwords [idx, idx + count) to the GPU
  syncRange(idx, count) {
    if (idx < 0) { count += idx; idx = 0; }
    count = Math.min(count, FB_WORDS - idx);
    if (count <= 0) return;
    if (!Number.isInteger(this.p.syncMax) || this.p.syncMax <= 0) throw new Error('Invalid sync capacity');
    while (count > 0) {
      const n = Math.min(count, this.p.syncMax);
      this.syncChunk(idx, n); idx += n; count -= n;
    }
  }
  syncChunk(idx, count) {
    if (!this.ex.n64_sync_scan(idx, count)) return;
    const q = this.device.queue, buf = this.mem.buffer, u32 = new Uint32Array(buf);
    const nruns = u32[this.p.nruns >> 2];
    if (nruns !== 0xFFFFFFFF && !this.hd) {
      const r = this.p.runs >> 2;
      for (let i = 0; i < nruns; i++) {
        const s = u32[r + i * 2], n = u32[r + i * 2 + 1];
        q.writeBuffer(this.fbBuf, (idx + s) * 4, buf, this.p.stage + s * 4, n * 4);
        for (const j of this.inflight) j.excl.push([idx + s, idx + s + n]);
      }
      this.stats.uploads += nruns;
    } else {
      this.merge(idx, count);
      for (const j of this.inflight) j.excl.push([idx, idx + count]);
    }
  }

  // called by the core (host_gpu_flush) with a complete batch in its buffers
  flush() {
    const buf = this.mem.buffer, u32 = new Uint32Array(buf), bi = this.p.b_info >> 2;
    // (not a batch: the core reports that a picture is complete. If the game has been looking at what the GPU
    // draws, start copying it back now so that it is usually there by the time the game asks.)
    if (u32[this.p.hint >> 2]) { if (this.speculate > 0) this.startReadback(); return; }
    const fmt = u32[bi], w = u32[bi + 1], h = u32[bi + 2], addr = u32[bi + 3], depth = u32[bi + 4];
    const np = u32[bi + 8], ns = u32[bi + 9], nst = u32[bi + 10], nts = u32[bi + 11], ntm = u32[bi + 12];
    const tilesX = u32[bi + 16], ntiles = u32[bi + 17], binsWords = u32[bi + 18];
    if (!np || !tilesX || !ntiles) return;
    const pixels = w * h, cIdx = fmt === 4 ? addr * 2 : addr, cCnt = fmt === 4 ? pixels * 2 : pixels;
    const zUse = u32[bi + 15];
    this.syncRange(cIdx, cCnt);
    if (zUse) this.syncRange(depth, pixels);
    const q = this.device.queue;
    if (u32[this.p.feedbackDirty >> 2]) {
      q.writeBuffer(this.native.feedbackBuf, 0, buf, this.p.feedback, 48);
      if (this.hd) q.writeBuffer(this.hd.feedbackBuf, 0, buf, this.p.feedback, 48);
      u32[this.p.feedbackDirty >> 2] = 0;
    }
    q.writeBuffer(this.ubuf, 0, buf, this.p.b_info, 80);
    q.writeBuffer(this.primsBuf, 0, buf, this.p.b_prims, np * 288);
    q.writeBuffer(this.spansBuf, 0, buf, this.p.b_spans, (ns + 1) * 32);
    q.writeBuffer(this.tablesBuf, 0, buf, this.p.b_states, nst * 64);
    q.writeBuffer(this.tablesBuf, 8192 * 4, buf, this.p.b_tiles, nts * 256);
    q.writeBuffer(this.tmemBuf, 0, buf, this.p.b_tmem, ntm * 4096);
    q.writeBuffer(this.binsBuf, 0, buf, this.p.bins, binsWords * 4);
    const enc = this.device.createCommandEncoder(), tilesY = ntiles / tilesX;
    const ordered = !!u32[bi + 19];
    for (const set of [this.native, this.hd].filter(Boolean)) {
      let pass = enc.beginComputePass();
      pass.setPipeline(ordered ? set.ordered : set.rdp); pass.setBindGroup(0, ordered ? set.orderedBG : set.rdpBG);
      pass.dispatchWorkgroups(ordered ? 1 : tilesX << set.L, ordered ? 1 : tilesY << set.L); pass.end();
      if (!ordered) {
        pass = enc.beginComputePass(); pass.setPipeline(set.resolve); pass.setBindGroup(0, set.resolveBG);
        pass.dispatchWorkgroups(1); pass.end();
      }
    }
    q.submit([enc.finish()]);
    this.stats.batches++;
    if (ordered) this.stats.orderedBatches = (this.stats.orderedBatches || 0) + 1;
    this.touch(cIdx, cCnt);
    if (!u32[bi + 13] && (zUse & 2)) this.touch(depth, pixels);
  }
  touch(idx, count) {
    count = Math.min(count, FB_WORDS - idx);
    if (count > 0 && (this.touched.get(idx) | 0) < count) this.touched.set(idx, count);
  }

  // asynchronous GPU -> CPU copy of what has been rendered since the last one (as much as fits in one go)
  startReadback() {
    if (this.readbackError) throw this.readbackError;
    if (!this.touched.size) return;
    const regs = []; let total = 0;
    for (const [idx, cnt0] of this.touched) {
      const cnt = Math.min(cnt0, this.scratchWords);
      if (total + cnt > this.scratchWords) break;
      regs.push({ idx, cnt, off: total }); total += cnt;
      if (cnt < cnt0) { this.touched.delete(idx); this.touched.set(idx + cnt, cnt0 - cnt); break; }
      this.touched.delete(idx);
    }
    if (!total) return;
    const feedbackOffset = (total * 4 + 7) & ~7;
    const bytes = feedbackOffset + 48;
    let sb = null;
    for (let i = 0; i < this.pool.length; i++) if (this.pool[i].size >= bytes) { sb = this.pool.splice(i, 1)[0]; break; }
    try {
      if (!sb) sb = this.device.createBuffer({ size: Math.max(1 << 20, 1 << Math.ceil(Math.log2(bytes))), usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
      const enc = this.device.createCommandEncoder();
      for (const r of regs) enc.copyBufferToBuffer(this.fbBuf, r.idx * 4, sb, r.off * 4, r.cnt * 4);
      enc.copyBufferToBuffer(this.native.feedbackBuf, 0, sb, feedbackOffset, 48);
      this.device.queue.submit([enc.finish()]);
    } catch (error) {
      if (sb) sb.destroy();
      for (const r of regs) this.touch(r.idx, r.cnt);
      this.readbackError = error; throw error;
    }
    const job = { regs, excl: [], dead: false };
    this.inflight.push(job);
    // (results are applied strictly in the order the snapshots were taken, whatever order the maps complete in)
    const prev = this.tail || Promise.resolve();
    let mapped, reusable = false;
    try { mapped = sb.mapAsync(GPUMapMode.READ, 0, bytes); } catch (e) { mapped = Promise.reject(e); }
    job.promise = Promise.allSettled([mapped, prev]).then(results => {
      // Wait for both promises before touching or releasing a staging buffer.
      if (job.dead) return;
      const rejected = results.find(r => r.status === 'rejected');
      if (rejected) throw rejected.reason;
      if (!job.dead) {
        new Uint32Array(this.mem.buffer, this.scratch, total).set(new Uint32Array(sb.getMappedRange(0, total * 4)));
        new Int32Array(this.mem.buffer, this.p.feedback, 12).set(new Int32Array(sb.getMappedRange(feedbackOffset, 48)));
        for (const r of regs) {
          // skip parts the CPU has overwritten (and uploaded) after this snapshot was taken
          let segs = [[r.idx, r.idx + r.cnt]];
          for (const [a, b] of job.excl) {
            const next = [];
            for (const [s, e] of segs) {
              if (b <= s || a >= e) { next.push([s, e]); continue; }
              if (a > s) next.push([s, a]);
              if (b < e) next.push([b, e]);
            }
            segs = next;
            if (!segs.length) break;
          }
          for (const [s, e] of segs) this.ex.n64_readback_apply(s, e - s, this.scratch + (r.off + s - r.idx) * 4);
        }
        this.stats.readbacks++; reusable = true;
      }
    }).catch(error => {
      if (!job.dead) {
        for (const r of regs) this.touch(r.idx, r.cnt);
        this.readbackError = error;
        throw error;
      }
    }).finally(() => {
      const i = this.inflight.indexOf(job); if (i >= 0) this.inflight.splice(i, 1);
      sb.unmap();
      if (reusable && !job.dead && !this.readbackError && this.pool.length < 4) this.pool.push(sb); else sb.destroy();
    });
    this.tail = job.promise;
    // Speculative jobs have no awaiting caller yet; syncNow observes the latch.
    job.promise.catch(() => {});
  }

  // run the VI for the field that just ended and (optionally) present it
  present(toCanvas = true) {
    const ex = this.ex, set = this.hd || this.native, S = set.S;
    ex.n64_vi_decode();
    const u32 = new Uint32Array(this.mem.buffer), i32 = new Int32Array(this.mem.buffer), vs = this.p.vi_state >> 2;
    const status = u32[vs], origin = u32[vs + 1], width = u32[vs + 2], maxX = i32[vs + 13], maxY = i32[vs + 14];
    const serrate = u32[vs + 15], outH = Math.min(u32[vs + 17], OUT_H), valid = u32[vs + 18];  // (the scale texture is OUT_H tall)
    const W = maxX + 2, H = maxY + 2;
    if (valid) {
      if ((status & 3) === 3) this.syncRange(((origin >> 2) - width - 3) * 2, ((H + 2) * width + 6) * 2);
      else this.syncRange((origin >> 1) - width - 3, (H + 2) * width + 6);
    }
    const U = this.viU32, F = this.viF32;
    U.set(u32.subarray(vs, vs + 20));
    U[20] = RDRAM_MASK; U[21] = 1;
    if (this.ctx && toCanvas) {
      const cw = this.canvas.width, ch = this.canvas.height;
      // 4:3 letterbox
      let dw = cw, dh = ch;
      if (!this.stretch) { if (cw * 3 > ch * 4) dw = Math.round(ch * 4 / 3); else dh = Math.round(cw * 3 / 4); }
      F[24] = -dw / cw; F[25] = -dh / ch; F[26] = dw / cw; F[27] = dh / ch;
      F[28] = 1; F[29] = outH / OUT_H; F[30] = this.filter === 1 ? 1 : 0; F[31] = 0;
      F[32] = Math.max(1, Math.floor(dw / (OUT_W * S))); F[33] = Math.max(1, Math.floor(dh / (outH * S)));
    }
    const q = this.device.queue;
    q.writeBuffer(this.viUbuf, 0, this.viU);
    const enc = this.device.createCommandEncoder();
    if (valid) {
      const aw = Math.min(W + 2, AA_W) * S, ah = Math.min(H, AA_H) * S;
      const p1 = enc.beginRenderPass({ colorAttachments: [{ view: set.aaView, loadOp: 'load', storeOp: 'store' }] });
      p1.setViewport(0, 0, aw, ah, 0, 1); p1.setScissorRect(0, 0, aw, ah);
      p1.setPipeline(set.aa); p1.setBindGroup(0, set.aaBG); p1.draw(3); p1.end();
    }
    const keep = serrate && outH === set.lastOutH;
    set.lastOutH = outH; this.outH = outH;
    const p2 = enc.beginRenderPass({ colorAttachments: [{ view: set.outView, loadOp: keep ? 'load' : 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
    p2.setViewport(0, 0, OUT_W * S, outH * S, 0, 1); p2.setScissorRect(0, 0, OUT_W * S, outH * S);
    p2.setPipeline(set.scale); p2.setBindGroup(0, set.scaleBG); p2.draw(3); p2.end();
    if (this.ctx && toCanvas) {
      const p3 = enc.beginRenderPass({ colorAttachments: [{ view: this.ctx.getCurrentTexture().createView(), loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
      p3.setPipeline(this.blitPipe); p3.setBindGroup(0, set.blitBG[this.filter === 0 ? 1 : 0]); p3.draw(4); p3.end();
    }
    q.submit([enc.finish()]);
    if (this.speculate > 0) { this.speculate--; this.startReadback(); }   // (also covers what the end of the field flushed)
    // back-pressure signal for the main loop: how many presented frames the GPU has not finished yet
    this.pending++;
    q.onSubmittedWorkDone().then(() => { this.pending--; this.done++; }, () => { this.pending--; this.done++; });
  }

  // Bring emulated RDRAM fully up to date with the GPU: called when the core stops because the game is about to
  // look at something the GPU drew (n64_frame() returned 1), before save states and when leaving GPU mode.
  async syncNow() {
    if (this.lost) throw this.readbackError || new Error('GPU device lost');
    const generation = this.generation;
    if (this.readbackError) throw this.readbackError;
    // waits: the copy-back was not there yet; late: it had not even been started (the guess at picture completion missed)
    if (this.touched.size || this.inflight.length) this.stats.waits++;
    if (this.touched.size) this.stats.late++;
    while (this.touched.size || this.inflight.length) {
      this.startReadback();
      await Promise.all(this.inflight.map(j => j.promise));
      if (generation !== this.generation) throw new Error('Obsolete renderer barrier');
    }
    if (this.readbackError) throw this.readbackError;
    this.ex.n64_sync_done();
    this.stats.syncs++;
    this.speculate = 600;
  }
  syncToCpu() { return this.syncNow(); }

  // ---- debugging / test helpers ----
  async readBuf(src, offset, bytes) {
    const sb = this.device.createBuffer({ size: bytes, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const enc = this.device.createCommandEncoder();
    enc.copyBufferToBuffer(src, offset, sb, 0, bytes);
    this.device.queue.submit([enc.finish()]);
    await sb.mapAsync(GPUMapMode.READ);
    const out = new Uint32Array(sb.getMappedRange()).slice();
    sb.unmap(); sb.destroy();
    return out;
  }
  readFb(idx, count) { return this.readBuf(this.fbBuf, idx * 4, count * 4); }
  readHd(idx, count) { const set = this.hd; if (!set) throw new Error('readHd needs an HD set (call setScale first)'); const n = set.S * set.S; return this.readBuf(set.target, (idx & (this.hdWords - 1)) * 4 * n, count * 4 * n); }
  async readOutput() {
    const set = this.hd || this.native, S = set.S, h = (this.outH || 240) * S, w = OUT_W * S;
    const sb = this.device.createBuffer({ size: w * 4 * h, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const enc = this.device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: set.outTex }, { buffer: sb, bytesPerRow: w * 4 }, [w, h]);
    this.device.queue.submit([enc.finish()]);
    await sb.mapAsync(GPUMapMode.READ);
    const out = new Uint8Array(sb.getMappedRange()).slice();
    sb.unmap(); sb.destroy();
    return { data: out, width: w, height: h };
  }
}
