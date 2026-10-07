// Debug helper for the high-resolution pass: capture the batches that produced the displayed frame, find
// pixels of the high-resolution copy that look like holes, and explain them by re-evaluating spans in JS.
window.runCustom = async function (cfg) {
  const wasm = await (await fetch('/out/n64.wasm')).arrayBuffer();
  const rom = new Uint8Array(await (await fetch(cfg.rom)).arrayBuffer());
  const sh = {};
  for (const n of ['rdp', 'vi', 'merge']) sh[n] = await (await fetch(`/src/web/${n}.wgsl`)).text();
  let gpu = null, cap = false; const batches = [];
  const A = await mkCore(wasm, rom.slice(), () => {
    if (cap) {
      const buf = A.ex.memory.buffer, u32 = new Uint32Array(buf), bi = gpu.p.b_info >> 2;
      const info = Array.from(u32.subarray(bi, bi + 20));
      batches.push({ frame: cur, info, prims: new Int32Array(buf, gpu.p.b_prims, info[8] * 72).slice(), spans: new Uint32Array(buf, gpu.p.b_spans, (info[9] + 1) * 8).slice(),
        bins: new Uint32Array(buf, gpu.p.bins, info[18]).slice(), states: new Uint32Array(buf, gpu.p.b_states, info[10] * 16).slice() });
    }
    gpu.flush();
  });
  let cur = 0;
  sh.allowFallback = true; if (cfg.hdWords) sh.hdWords = cfg.hdWords;
  gpu = await N64Gpu.create(A, null, sh);
  gpu.reset();
  const L = cfg.hd || 1, S = 1 << L;
  await gpu.setScale(L);
  const inputs = (cfg.inputs || '').split(',').filter(Boolean).map(s => { const [f, b, d] = s.split(':'); return { f: +f, b, d: +(d || 1) }; });
  const target = cfg.checks[0];
  let gpuOn = true;
  for (let i = 0; i <= target; i++) {
    cur = i;
    let b = 0, sx = 0, sy = 0;
    for (const e of inputs) if (i >= e.f && i < e.f + e.d) for (const k of e.b.split('+')) { if (k.startsWith('X=')) sx = +k.slice(2); else if (k.startsWith('Y=')) sy = +k.slice(2); else b |= BT[k] || 0; }
    A.ex.n64_input(0, b, sx, sy);
    const near = i >= target - 6;
    if (near && !gpuOn) { gpu.reset(); gpuOn = true; } else if (!near && gpuOn) { A.ex.n64_config(6, 0); await gpu.syncToCpu(); A.ex.n64_config(0, 0); gpuOn = false; }
    cap = near;
    while (A.ex.n64_frame()) await gpu.syncNow();
    if (gpuOn) gpu.present(false); else A.ex.n64_vi_decode();
    if (gpuOn) await gpu.device.queue.onSubmittedWorkDone();
  }
  A.ex.n64_config(6, 0);
  await gpu.device.queue.onSubmittedWorkDone();
  const vs = new Uint32Array(A.ex.memory.buffer, A.hi[2], 20), origin = vs[1], width = vs[2];
  const fbAddr = (origin >> 1) - 0;   // halfword index of the displayed buffer (approx. start)
  // batches of the displayed buffer: the last contiguous render to the address closest below origin
  const addrs = [...new Set(batches.map(b => b.info[3]))];
  let disp = addrs.filter(a => a <= fbAddr).sort((a, b) => b - a)[0];
  const mine = batches.filter(b => b.info[3] === disp);
  const lastFrame = mine[mine.length - 1].frame;
  const pass = mine.filter(b => b.frame >= lastFrame - 1);
  const W = pass[0].info[1], H = 240;
  const n = S * S;
  const hd = await gpu.readHd(disp, W * H);
  const nat = await gpu.readFb(disp, W * H);
  const px = (x, y) => hd[((y >> L) * W + (x >> L)) * n + (((y & (S - 1)) << L) | (x & (S - 1)))] & 0xFFFF;
  const rgb = v => [(v >> 8) & 0xF8, (v >> 3) & 0xF8, (v << 2) & 0xF8];
  const out = { disp: disp.toString(16), W, batches: pass.length, prims: pass.reduce((a, b) => a + b.info[8], 0), frames: [...new Set(pass.map(b => b.frame))] };
  // ---- JS model of the span setup at scale S for one line
  const sext = (v, bits) => (v << (32 - bits)) >> (32 - bits);
  function span(p, o, y, S, Lg) {
    const flags = p[o] & 0xFF, flip = flags & 1;
    const xh0 = p[o + 56] << Lg, xm0 = p[o + 57] << Lg, xl0 = p[o + 58] << Lg, dxh = p[o + 59], dxm = p[o + 60], dxl = p[o + 61];
    const yhn = p[o + 62], yh = yhn * S, ym = p[o + 63] * S, yl = p[o + 64] * S, yhb = (yhn & ~3) * S;
    const scx = p[o + 65] >>> 0, scy = p[o + 66] >>> 0;
    const ylo = Math.max(yh, (scy & 0xFFFF) * S), yhi = Math.min(yl, (scy >>> 16) * S), losc = (scx & 0xFFFF) << (1 + Lg), hisc = (scx >>> 16) << (1 + Lg);
    const xl_ = [], xr_ = []; let allInv = true, over = true, under = true;
    for (let i = 0; i < 4; i++) {
      const ys = y * 4 + i, clip = ys < ylo || ys >= yhi;
      let xh = (xh0 + Math.imul(ys - yhb, dxh)) | 0, xm = (xm0 + Math.imul(ys - yhb, dxm)) | 0, xl = (xl0 + Math.imul(ys - ym, dxl)) | 0;
      if (ys < ym) xl = xm;
      xl = sext(xl, 27 + Lg); xh = sext(xh, 27 + Lg);
      const xhs = (xh >> 12) | ((xh & 0xFFF) ? 1 : 0), xls = (xl >> 12) | ((xl & 0xFFF) ? 1 : 0);
      let l = flip ? xhs : xls, r = flip ? xls : xhs, inv = (l >> 1) > (r >> 1);
      if (!(Math.min(l, r) >= hisc)) over = false;
      if (!(Math.max(l, r) < losc)) under = false;
      l = Math.min(Math.max(l, losc), hisc); r = Math.min(Math.max(r, losc), hisc);
      inv = inv || clip;
      if (inv) { l = 0xFFFF; r = 0; } else allInv = false;
      xl_.push(l & 0xFFFF); xr_.push(r & 0xFFFF);
    }
    return { xl: xl_, xr: xr_, valid: !allInv && !over && !under, start: Math.min(...xl_) >> 3, end: Math.max(...xr_) >> 3 };
  }
  function cov(sp, x) {
    const b = (x << 3) & 0xFFFF, s = [b, (b + 4) & 0xFFFF, (b + 2) & 0xFFFF, (b + 6) & 0xFFFF]; let c = 0;
    const t = (v, i) => v >= sp.xl[i] && v < sp.xr[i];
    if (t(s[0], 0)) c |= 1; if (t(s[1], 0)) c |= 2; if (t(s[2], 1)) c |= 4; if (t(s[3], 1)) c |= 8;
    if (t(s[0], 2)) c |= 16; if (t(s[1], 2)) c |= 32; if (t(s[2], 3)) c |= 64; if (t(s[3], 3)) c |= 128;
    return c;
  }
  out.explain = [];
  const explain = (X, Y) => {
    const xn = X >> L, yn = Y >> L, r = { hd: [X, Y], color: rgb(px(X, Y)), native: rgb(nat[yn * W + xn] & 0xFFFF), prims: [] };
    let gi = 0;
    for (const b of pass) {
      const tilesX = b.info[16], ntiles = b.info[17], tile = (xn >> 3) + (yn >> 3) * tilesX;
      const cnt = b.bins[ntiles + tile], list = 2 * ntiles + b.bins[tile];
      const inBin = new Set(); for (let k = 0; k < cnt; k++) inBin.add(b.bins[list + k]);
      for (let i = 0; i < b.info[8]; i++, gi++) {
        const o = i * 72, p = b.prims;
        if (yn < p[o + 2] || yn > p[o + 3]) continue;
        const SP = (p[o + 1] + (yn - p[o + 2])) * 8, sf = b.spans[SP + 7];
        const nsp = span(p, o, yn, 1, 0), hsp = span(p, o, Y, S, L);
        const nc = nsp.valid && xn >= nsp.start && xn <= nsp.end ? cov(nsp, xn) : 0, hc = hsp.valid && X >= hsp.start && X <= hsp.end ? cov(hsp, X) : 0;
        if (!nc && !hc) continue;
        const consLo = (sf >> 2) & 0x1FFF, consHi = (sf >> 15) & 0x1FFF;
        const st = b.states.subarray(p[o + 4] * 16, p[o + 4] * 16 + 16);
        r.prims.push({ gi, i, ncov: nc.toString(2), hcov: hc.toString(2), inBin: inBin.has(i), sf: sf & 3, cons: [consLo, consHi], consOk: (sf & 2) && xn >= consLo && xn <= consHi, flags: (p[o] & 0xFF).toString(16),
          sflags: st[0].toString(16), db: st[6].toString(16), cvgz: st[9].toString(16), z: p[o + 24 + 2] >> 16, dz: (p[o + 51] & 0xFFFF), shade: [p[o + 8] >> 16, p[o + 9] >> 16, p[o + 10] >> 16] });
      }
    }
    out.explain.push(r);
  };
  for (const s of (cfg.pts || [])) explain(s[0], s[1]);
  // dump a region as RGB for viewing
  if (cfg.region) {
    const [x0, y0, w, h] = cfg.region; const data = new Uint8Array(w * h * 4);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const c = rgb(px(x0 + x, y0 + y)); data.set([c[0], c[1], c[2], 255], (y * w + x) * 4); }
    out.images = [{ frame: 1, w, h, data: b64(data) }];
  }
  return out;
};
