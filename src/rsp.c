// Low-level RSP: scalar unit + vector unit (SIMD via compiler vector extensions).
#include "core.h"

typedef s16 v16 __attribute__((vector_size(16)));
typedef u16 vu16 __attribute__((vector_size(16)));
typedef s32 v32 __attribute__((vector_size(32)));
typedef u32 vu32 __attribute__((vector_size(32)));
typedef union { v16 v; s16 e[8]; u16 u[8]; u8 b[16]; } VReg;

#define SX(x) __builtin_convertvector((v16)(x), v32)
#define ZX(x) __builtin_convertvector((vu16)(x), v32)
#define NARROW(x) __builtin_convertvector((x), v16)
#define SEL(m, a, b) ((v16)(((v16)(m) & (v16)(a)) | (~(v16)(m) & (v16)(b))))
#define VB(r, i) ((r).b[((i) & 15) ^ 1])

typedef struct {
  u32 r[32];
  u32 pc, npc;
  VReg v[32];
  VReg acc_l;
  v32 acc_hm;                 // upper 32 bits of each 48-bit accumulator lane
  VReg vco_l, vco_h, vcc_l, vcc_h, vce;   // lane masks (0 / -1)
  s16 divin; s16 divout; u32 divdp;
  u64 sync;
  u64 icount;
  u16 rcp[512], rsq[512];
} RSP;

RSP rsp __attribute__((aligned(32)));

#define DMEM spmem
#define IMEM (spmem + 0x1000)
#define SLICE 3000
#define SP_HALT 1u
#define SP_BROKE 2u

static inline u8 dm_r8(u32 a) { return DMEM[(a & 0xFFF) ^ 3]; }
static inline void dm_w8(u32 a, u8 v) { DMEM[(a & 0xFFF) ^ 3] = v; }
static inline u32 dm_r16(u32 a) { a &= 0xFFF; if (!(a & 1)) return *(u16 *)(DMEM + (a ^ 2)); return (dm_r8(a) << 8) | dm_r8(a + 1); }
static inline u32 dm_r32(u32 a) { a &= 0xFFF; if (!(a & 3)) return *(u32 *)(DMEM + a); return (dm_r8(a) << 24) | (dm_r8(a + 1) << 16) | (dm_r8(a + 2) << 8) | dm_r8(a + 3); }
static inline void dm_w16(u32 a, u32 v) { a &= 0xFFF; if (!(a & 1)) *(u16 *)(DMEM + (a ^ 2)) = v; else { dm_w8(a, v >> 8); dm_w8(a + 1, v); } }
static inline void dm_w32(u32 a, u32 v) { a &= 0xFFF; if (!(a & 3)) *(u32 *)(DMEM + a) = v; else { dm_w8(a, v >> 24); dm_w8(a + 1, v >> 16); dm_w8(a + 2, v >> 8); dm_w8(a + 3, v); } }

// ---------------------------------------------------------------------------
// SP / DP registers
// ---------------------------------------------------------------------------
// DMA registers are double-buffered: writes land in "pending", reads return the transfer's "current" state.
static struct { u32 mem, dram, len, count, skip; } sp_pend, sp_cur;

static u32 dma_blocked, rsp_paused_left;
static void sp_dma(u32 v, int to_rsp) {
  // reading memory the GPU has drawn into: the host has to copy it back first, then the transfer is repeated
  if (to_rsp && unlikely(gpu_stale_n)) {
    u32 len = (v & 0xFF8) + 8, cnt = (v >> 12) & 0xFF, skip = (v >> 20) & 0xFF8;
    if (gpu_range_stale(sp_pend.dram, (cnt + 1) * len + cnt * skip)) { gpu_request_sync(); dma_blocked = 1; gpu_sync_cause[0] = 3; gpu_sync_cause[1] = sp_pend.dram; return; }
  }
  sp_pend.len = v & 0xFF8; sp_pend.count = (v >> 12) & 0xFF; sp_pend.skip = (v >> 20) & 0xFF8;
  sp_cur = sp_pend;
  for (;;) {
    for (u32 i = 0; i <= sp_cur.len; i += 8) {
      u32 m = sp_cur.mem & 0x1FF8, d = sp_cur.dram & 0xFFFFF8;
      for (u32 k = 0; k < 8; k += 4) {
        if (to_rsp) *(u32 *)(spmem + m + k) = d + k < sys.rdram_size ? RDRAM32(d + k) : 0;
        else if (d + k < sys.rdram_size) { RDRAM32(d + k) = *(u32 *)(spmem + m + k); if (unlikely(gpu_watch[(d + k) >> 12])) gpu_mark_dirty(d + k, 4); }
      }
      sp_cur.dram = (sp_cur.dram + 8) & 0xFFFFFF;
      sp_cur.mem = (sp_cur.mem & 0x1000) | ((sp_cur.mem + 8) & 0xFFF);
    }
    if (!sp_cur.count) break;
    sp_cur.count--;
    sp_cur.dram = (sp_cur.dram + sp_cur.skip) & 0xFFFFFF;
  }
  sp_cur.len = 0xFF8;
}

static u32 sp_read(u32 r) {
  switch (r) {
    case 0: return sp_cur.mem;
    case 1: return sp_cur.dram;
    case 2: case 3: return sp_cur.len | (sp_cur.count << 12) | (sp_cur.skip << 20);
    case 4: return sys.sp_status;
    case 5: case 6: return 0;
    case 7: { u32 v = sys.sp_sema; sys.sp_sema = 1; return v; }
    case 8: return rsp.pc & 0xFFC;
    default: return 0;
  }
}

static void sp_write(u32 r, u32 v) {
  switch (r) {
    case 0: sp_pend.mem = v & 0x1FF8; break;
    case 1: sp_pend.dram = v & 0xFFFFF8; break;
    case 2: sp_dma(v, 1); break;
    case 3: sp_dma(v, 0); break;
    case 4: {
      u32 s = sys.sp_status, was_halted = s & SP_HALT;
      if ((v & 1) && !(v & 2)) s &= ~SP_HALT;
      if ((v & 2) && !(v & 1)) s |= SP_HALT;
      if (v & 4) s &= ~SP_BROKE;
      if ((v & 8) && !(v & 0x10)) mi_lower(MI_SP);
      if ((v & 0x10) && !(v & 8)) mi_raise(MI_SP);
      for (int i = 0; i < 10; i++) {   // sstep, intr_break, sig0..7
        u32 clr = v & (0x20u << (2 * i)), set = v & (0x40u << (2 * i));
        if (clr && !set) s &= ~(0x20u << i);
        if (set && !clr) s |= (0x20u << i);
      }
      sys.sp_status = s;
      if (was_halted && !(s & SP_HALT)) { rsp.sync = cpu.cycles; ev_set(EV_RSP, cpu.cycles + SLICE); }
      break;
    }
    case 7: sys.sp_sema = 0; break;
    case 8: rsp.pc = v & 0xFFC; rsp.npc = (rsp.pc + 4) & 0xFFC; break;
    default: break;
  }
}

static u32 dp_read(u32 r) {
  switch (r) {
    case 0: return sys.dp_start;
    case 1: return sys.dp_end;
    case 2: return sys.dp_current;
    case 3: return (sys.dp_status & 0x407) | 0x80;      // xbus, freeze, flush, cbuf ready, start valid
    case 4: return sys.dp_clock;
    default: return 0;
  }
}

#ifdef DP_TRACE   // (test builds: log every write to the RDP's registers)
int dp_trace_on; static int dp_trace_cpu;
#endif
static void dp_write(u32 r, u32 v) {
#ifdef DP_TRACE
  if (dp_trace_on) printf("  [dp] %s write reg%u = %x   (start=%x end=%x cur=%x status=%x, frame %u, rsp pc=%x halted=%u)\n", dp_trace_cpu ? "CPU" : "RSP", r, v, sys.dp_start, sys.dp_end, sys.dp_current, sys.dp_status, sys.frames, rsp.pc, sys.sp_status & 1);
#endif
  switch (r) {
    // START is only latched: the read pointer moves to it when END is written next. (Rare's microcode writes START
    // and END separately while the CPU may unfreeze the RDP in between; moving the pointer early would make the RDP
    // run through a whole buffer of old commands at that moment.)
    case 0: if (!(sys.dp_status & 0x400)) sys.dp_start = v & 0xFFFFF8; sys.dp_status |= 0x400; break;
    case 1:
      sys.dp_end = v & 0xFFFFF8;
      if (sys.dp_status & 0x400) { sys.dp_current = sys.dp_start; sys.dp_status &= ~0x400u; }
      if (!(sys.dp_status & 2)) rdp_process();
      break;
    case 3:
      if (v & 1) sys.dp_status &= ~1u;
      if (v & 2) sys.dp_status |= 1;
      if (v & 4) { sys.dp_status &= ~2u; rdp_process(); }
      if (v & 8) sys.dp_status |= 2;
      if (v & 0x10) sys.dp_status &= ~4u;
      if (v & 0x20) sys.dp_status |= 4;
      if (v & 0x200) sys.dp_clock = 0;
      break;
    default: break;
  }
}

// CPU-side accessors: bring the RSP up to date first
// (if the RSP had to stop for a GPU copy-back, the CPU access is not performed: its instruction is repeated later)
u32 sp_reg_read(u32 r) { rsp_sync(); if (unlikely(cpu_restart)) return 0; return sp_read(r); }
void sp_reg_write(u32 r, u32 v) {
  rsp_sync(); if (unlikely(cpu_restart)) return;
  sp_write(r, v);
  if (unlikely(dma_blocked)) { dma_blocked = 0; cpu_restart = 1; }
}
u32 dp_reg_read(u32 r) { rsp_sync(); if (unlikely(cpu_restart)) return 0; return dp_read(r); }
void dp_reg_write(u32 r, u32 v) {
  rsp_sync(); if (unlikely(cpu_restart)) return;
#ifdef DP_TRACE
  dp_trace_cpu = 1; dp_write(r, v); dp_trace_cpu = 0; return;
#endif
  dp_write(r, v);
}

static s32 rsp_run_n(s32 n);
// Bring the RSP up to the CPU's time. Returns the number of instructions that are still owed because the RSP had
// to stop for a GPU copy-back (they run when this is called again at the same CPU time).
u32 rsp_sync(void) {
  rsp_paused_left = 0;
  if (sys.sp_status & SP_HALT) return 0;
  if (unlikely(gpu_sync_request)) { cpu_restart = 1; return 0; }
  u64 now = cpu.cycles;
  // A CPU MMIO access may already have synchronized this exact cycle before
  // the dispatcher consumes EV_RSP. Keep a running RSP scheduled on early returns.
  if (cpu.ev[EV_RSP] == EV_NEVER) ev_set(EV_RSP, now + SLICE);
  if (now <= rsp.sync) return 0;
  s64 n = (s64)(now - rsp.sync) * 2 / 3;
  if (n <= 0) return 0;
  if (n > 1000000) n = 1000000;
  rsp.sync = now;
  s32 left = rsp_run_n((s32)n);
  if (unlikely(gpu_sync_request)) {
    cpu_restart = 1;
    if (left > 0) { rsp.sync = now - ((u64)left * 3 + 1) / 2; rsp_paused_left = (u32)left; return (u32)left; }   // (so that the same call owes exactly `left`)
  }
  if (!(sys.sp_status & SP_HALT)) { if (cpu.ev[EV_RSP] > now + SLICE) ev_set(EV_RSP, now + SLICE); }
  else cpu.ev[EV_RSP] = EV_NEVER;
  return 0;
}

// ---------------------------------------------------------------------------
// Vector helpers
// ---------------------------------------------------------------------------
#ifdef __wasm_simd128__
// element broadcast as one byte shuffle through a mask table (no branch on the element specifier)
typedef s8 v8 __attribute__((vector_size(16)));
#define BC(a, b, c, d, e, f, g, h) { 2*a, 2*a+1, 2*b, 2*b+1, 2*c, 2*c+1, 2*d, 2*d+1, 2*e, 2*e+1, 2*f, 2*f+1, 2*g, 2*g+1, 2*h, 2*h+1 }
static const v8 bc_mask[16] = {
  BC(0, 1, 2, 3, 4, 5, 6, 7), BC(0, 1, 2, 3, 4, 5, 6, 7), BC(0, 0, 2, 2, 4, 4, 6, 6), BC(1, 1, 3, 3, 5, 5, 7, 7),
  BC(0, 0, 0, 0, 4, 4, 4, 4), BC(1, 1, 1, 1, 5, 5, 5, 5), BC(2, 2, 2, 2, 6, 6, 6, 6), BC(3, 3, 3, 3, 7, 7, 7, 7),
  BC(0, 0, 0, 0, 0, 0, 0, 0), BC(1, 1, 1, 1, 1, 1, 1, 1), BC(2, 2, 2, 2, 2, 2, 2, 2), BC(3, 3, 3, 3, 3, 3, 3, 3),
  BC(4, 4, 4, 4, 4, 4, 4, 4), BC(5, 5, 5, 5, 5, 5, 5, 5), BC(6, 6, 6, 6, 6, 6, 6, 6), BC(7, 7, 7, 7, 7, 7, 7, 7) };
static inline v16 vbroadcast(v16 v, u32 e) { return (v16)__builtin_wasm_swizzle_i8x16((v8)v, bc_mask[e]); }
#else
static inline v16 vbroadcast(v16 v, u32 e) {
  switch (e) {
    default: return v;
    case 2: return __builtin_shufflevector(v, v, 0, 0, 2, 2, 4, 4, 6, 6);
    case 3: return __builtin_shufflevector(v, v, 1, 1, 3, 3, 5, 5, 7, 7);
    case 4: return __builtin_shufflevector(v, v, 0, 0, 0, 0, 4, 4, 4, 4);
    case 5: return __builtin_shufflevector(v, v, 1, 1, 1, 1, 5, 5, 5, 5);
    case 6: return __builtin_shufflevector(v, v, 2, 2, 2, 2, 6, 6, 6, 6);
    case 7: return __builtin_shufflevector(v, v, 3, 3, 3, 3, 7, 7, 7, 7);
    case 8: return __builtin_shufflevector(v, v, 0, 0, 0, 0, 0, 0, 0, 0);
    case 9: return __builtin_shufflevector(v, v, 1, 1, 1, 1, 1, 1, 1, 1);
    case 10: return __builtin_shufflevector(v, v, 2, 2, 2, 2, 2, 2, 2, 2);
    case 11: return __builtin_shufflevector(v, v, 3, 3, 3, 3, 3, 3, 3, 3);
    case 12: return __builtin_shufflevector(v, v, 4, 4, 4, 4, 4, 4, 4, 4);
    case 13: return __builtin_shufflevector(v, v, 5, 5, 5, 5, 5, 5, 5, 5);
    case 14: return __builtin_shufflevector(v, v, 6, 6, 6, 6, 6, 6, 6, 6);
    case 15: return __builtin_shufflevector(v, v, 7, 7, 7, 7, 7, 7, 7, 7);
  }
}

#endif

static inline v16 clamp_s16(v32 x) {
#ifdef __wasm_simd128__
  // one saturating narrow instead of min / max / truncate
  typedef s32 v32h __attribute__((vector_size(16)));
  v32h lo4 = __builtin_shufflevector(x, x, 0, 1, 2, 3), hi4 = __builtin_shufflevector(x, x, 4, 5, 6, 7);
  return (v16)__builtin_wasm_narrow_s_i16x8_i32x4(lo4, hi4);
#else
  const v32 lo = { -32768, -32768, -32768, -32768, -32768, -32768, -32768, -32768 };
  const v32 hi = { 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767 };
  v32 under = x < lo, over = x > hi;
  return NARROW((x & ~(under | over)) | (lo & under) | (hi & over));
#endif
}
// unsigned-style saturation of the low slice (VMADL/VMADN)
static inline v16 clamp_acc_l(v32 hm, v16 l) {
  v16 neg = NARROW(hm < -32768), pos = NARROW(hm > 32767);
  return (l & ~neg) | pos;
}
// VMULU/VMACU output
static inline v16 clamp_u(v32 hm) {
  v16 neg = NARROW(hm < 0), pos = NARROW(hm > 32767);
  return (NARROW(hm) & ~neg) | pos;
}

static void vec_slow(u32 op);

static inline void rsp_vec(u32 op) {
  u32 e = (op >> 21) & 15;
  VReg *vd = &rsp.v[(op >> 6) & 31];
  v16 vs = rsp.v[(op >> 11) & 31].v;
  v16 vt = vbroadcast(rsp.v[(op >> 16) & 31].v, e);
  const v16 zero = {0};
  const v32 mask16 = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
  switch (op & 0x3F) {
    case 0x00: case 0x01: {   // VMULF / VMULU
      v32 t = SX(vs) * SX(vt) + 0x4000;
      rsp.acc_hm = t >> 15;
      rsp.acc_l.v = NARROW((v32)((vu32)t << 1));
      if (op & 1) { v16 neg = NARROW(rsp.acc_hm < 0), m = NARROW(rsp.acc_hm); vd->v = (m | (m >> 15)) & ~neg; }
      else vd->v = clamp_s16(rsp.acc_hm);
      break;
    }
    case 0x04: {              // VMUDL
      vu32 p = (vu32)ZX(vs) * (vu32)ZX(vt);
      rsp.acc_l.v = NARROW((v32)(p >> 16));
      rsp.acc_hm = (v32){0};
      vd->v = rsp.acc_l.v;
      break;
    }
    case 0x05: {              // VMUDM
      v32 p = SX(vs) * ZX(vt);
      rsp.acc_l.v = NARROW(p);
      rsp.acc_hm = p >> 16;
      vd->v = NARROW(rsp.acc_hm);
      break;
    }
    case 0x06: {              // VMUDN
      v32 p = ZX(vs) * SX(vt);
      rsp.acc_l.v = NARROW(p);
      rsp.acc_hm = p >> 16;
      vd->v = rsp.acc_l.v;
      break;
    }
    case 0x07: {              // VMUDH
      rsp.acc_hm = SX(vs) * SX(vt);
      rsp.acc_l.v = zero;
      vd->v = clamp_s16(rsp.acc_hm);
      break;
    }
    case 0x08: case 0x09: {   // VMACF / VMACU
      v32 p = SX(vs) * SX(vt);
      v32 suml = ZX(rsp.acc_l.v) + ((v32)((vu32)p << 1) & mask16);
      rsp.acc_l.v = NARROW(suml);
      rsp.acc_hm = rsp.acc_hm + (p >> 15) + (v32)((vu32)suml >> 16);
      vd->v = (op & 1) ? clamp_u(rsp.acc_hm) : clamp_s16(rsp.acc_hm);
      break;
    }
    case 0x0C: {              // VMADL
      vu32 p = ((vu32)ZX(vs) * (vu32)ZX(vt)) >> 16;
      v32 suml = ZX(rsp.acc_l.v) + (v32)p;
      rsp.acc_l.v = NARROW(suml);
      rsp.acc_hm = rsp.acc_hm + (v32)((vu32)suml >> 16);
      vd->v = clamp_acc_l(rsp.acc_hm, rsp.acc_l.v);
      break;
    }
    case 0x0D: {              // VMADM
      v32 p = SX(vs) * ZX(vt);
      v32 suml = ZX(rsp.acc_l.v) + (p & mask16);
      rsp.acc_l.v = NARROW(suml);
      rsp.acc_hm = rsp.acc_hm + (p >> 16) + (v32)((vu32)suml >> 16);
      vd->v = clamp_s16(rsp.acc_hm);
      break;
    }
    case 0x0E: {              // VMADN
      v32 p = ZX(vs) * SX(vt);
      v32 suml = ZX(rsp.acc_l.v) + (p & mask16);
      rsp.acc_l.v = NARROW(suml);
      rsp.acc_hm = rsp.acc_hm + (p >> 16) + (v32)((vu32)suml >> 16);
      vd->v = clamp_acc_l(rsp.acc_hm, rsp.acc_l.v);
      break;
    }
    case 0x0F: {              // VMADH
      rsp.acc_hm = rsp.acc_hm + SX(vs) * SX(vt);
      vd->v = clamp_s16(rsp.acc_hm);
      break;
    }
    case 0x10: {              // VADD
      v32 r = SX(vs) + SX(vt) - SX(rsp.vco_l.v);
      rsp.acc_l.v = NARROW(r);
      vd->v = clamp_s16(r);
      rsp.vco_l.v = zero; rsp.vco_h.v = zero;
      break;
    }
    case 0x11: {              // VSUB
      v32 r = SX(vs) - SX(vt) + SX(rsp.vco_l.v);
      rsp.acc_l.v = NARROW(r);
      vd->v = clamp_s16(r);
      rsp.vco_l.v = zero; rsp.vco_h.v = zero;
      break;
    }
    case 0x13: {              // VABS
      v16 neg = vs < zero, pos = vs > zero;
      v16 r = ((vt ^ neg) - neg) & (neg | pos);
      rsp.acc_l.v = r;
      vd->v = r ^ (neg & (vt == (v16){ -32768, -32768, -32768, -32768, -32768, -32768, -32768, -32768 }));
      break;
    }
    case 0x14: {              // VADDC
      v16 sum = vs + vt;
      rsp.vco_l.v = (v16)((vu16)sum < (vu16)vs);
      rsp.vco_h.v = zero;
      rsp.acc_l.v = sum; vd->v = sum;
      break;
    }
    case 0x15: {              // VSUBC
      v16 dif = vs - vt;
      rsp.vco_l.v = (v16)((vu16)vs < (vu16)vt);
      rsp.vco_h.v = (v16)(vs != vt);
      rsp.acc_l.v = dif; vd->v = dif;
      break;
    }
    case 0x1D: {              // VSAR
      if (e == 8) vd->v = NARROW(rsp.acc_hm >> 16);
      else if (e == 9) vd->v = NARROW(rsp.acc_hm);
      else if (e == 10) vd->v = rsp.acc_l.v;
      else vd->v = zero;
      break;
    }
    case 0x20: {              // VLT
      v16 c = (vs < vt) | ((vs == vt) & rsp.vco_l.v & rsp.vco_h.v);
      rsp.vcc_l.v = c; rsp.vcc_h.v = zero; rsp.vco_l.v = zero; rsp.vco_h.v = zero;
      rsp.acc_l.v = SEL(c, vs, vt); vd->v = rsp.acc_l.v;
      break;
    }
    case 0x21: {              // VEQ
      v16 c = (vs == vt) & ~rsp.vco_h.v;
      rsp.vcc_l.v = c; rsp.vcc_h.v = zero; rsp.vco_l.v = zero; rsp.vco_h.v = zero;
      rsp.acc_l.v = SEL(c, vs, vt); vd->v = rsp.acc_l.v;
      break;
    }
    case 0x22: {              // VNE
      v16 c = (vs != vt) | rsp.vco_h.v;
      rsp.vcc_l.v = c; rsp.vcc_h.v = zero; rsp.vco_l.v = zero; rsp.vco_h.v = zero;
      rsp.acc_l.v = SEL(c, vs, vt); vd->v = rsp.acc_l.v;
      break;
    }
    case 0x23: {              // VGE
      v16 c = (vs > vt) | ((vs == vt) & ~(rsp.vco_l.v & rsp.vco_h.v));
      rsp.vcc_l.v = c; rsp.vcc_h.v = zero; rsp.vco_l.v = zero; rsp.vco_h.v = zero;
      rsp.acc_l.v = SEL(c, vs, vt); vd->v = rsp.acc_l.v;
      break;
    }
    case 0x24: {              // VCL
      v16 col = rsp.vco_l.v, coh = rsp.vco_h.v, vce = rsp.vce.v;
      v16 sum = vs + vt;
      v16 carry = (v16)((vu16)sum < (vu16)vs);
      v16 sumz = (sum == zero);
      v16 lecond = (vce & (sumz | ~carry)) | (~vce & (sumz & ~carry));
      v16 ccl = SEL(col & ~coh, lecond, rsp.vcc_l.v);
      v16 gecond = (v16)((vu16)vs >= (vu16)vt);
      v16 cch = SEL(~col & ~coh, gecond, rsp.vcc_h.v);
      v16 r = SEL(col, SEL(ccl, -vt, vs), SEL(cch, vt, vs));
      rsp.vcc_l.v = ccl; rsp.vcc_h.v = cch;
      rsp.vco_l.v = zero; rsp.vco_h.v = zero; rsp.vce.v = zero;
      rsp.acc_l.v = r; vd->v = r;
      break;
    }
    case 0x25: {              // VCH
      v16 sign = (vs ^ vt) < zero;
      v16 sum = vs + vt, dif = vs - vt;
      v16 le = sum <= zero, ge = dif >= zero, vtn = vt < zero;
      v16 neq = (vs != ~vt);
      rsp.vcc_l.v = SEL(sign, le, vtn);
      rsp.vcc_h.v = SEL(sign, vtn, ge);
      rsp.vco_l.v = sign;
      rsp.vco_h.v = SEL(sign, (sum != zero) & neq, (dif != zero) & neq);
      rsp.vce.v = sign & (sum == (v16){ -1, -1, -1, -1, -1, -1, -1, -1 });
      v16 r = SEL(sign, SEL(le, -vt, vs), SEL(ge, vt, vs));
      rsp.acc_l.v = r; vd->v = r;
      break;
    }
    case 0x26: {              // VCR
      v16 sign = (vs ^ vt) < zero;
      v16 le = NARROW((SX(vs) + SX(vt) + 1) <= 0), ge = NARROW((SX(vs) - SX(vt)) >= 0), vtn = vt < zero;
      rsp.vcc_l.v = SEL(sign, le, vtn);
      rsp.vcc_h.v = SEL(sign, vtn, ge);
      v16 r = SEL(sign, SEL(le, ~vt, vs), SEL(ge, vt, vs));
      rsp.vco_l.v = zero; rsp.vco_h.v = zero; rsp.vce.v = zero;
      rsp.acc_l.v = r; vd->v = r;
      break;
    }
    case 0x27: {              // VMRG
      rsp.acc_l.v = SEL(rsp.vcc_l.v, vs, vt); vd->v = rsp.acc_l.v;
      rsp.vco_l.v = zero; rsp.vco_h.v = zero;
      break;
    }
    case 0x28: rsp.acc_l.v = vs & vt; vd->v = rsp.acc_l.v; break;
    case 0x29: rsp.acc_l.v = ~(vs & vt); vd->v = rsp.acc_l.v; break;
    case 0x2A: rsp.acc_l.v = vs | vt; vd->v = rsp.acc_l.v; break;
    case 0x2B: rsp.acc_l.v = ~(vs | vt); vd->v = rsp.acc_l.v; break;
    case 0x2C: rsp.acc_l.v = vs ^ vt; vd->v = rsp.acc_l.v; break;
    case 0x2D: rsp.acc_l.v = ~(vs ^ vt); vd->v = rsp.acc_l.v; break;
    case 0x37: case 0x3F: break;   // VNOP / VNULL
    default: vec_slow(op); break;
  }
}

static s32 rcp_calc(s32 input, int sqrt_mode) {
  s32 mask = input >> 31;
  s32 data = input ^ mask;
  if (input > -32768) data -= mask;
  if (data == 0) return 0x7FFFFFFF;
  if (input == -32768) return (s32)0xFFFF0000;
  u32 shift = __builtin_clz(data);
  u32 index = (u32)((((u64)(u32)data << shift) & 0x7FC00000) >> 22);
  s32 result;
  if (sqrt_mode) {
    result = rsp.rsq[(index & 0x1FE) | (shift & 1)];
    result = (0x10000 | result) << 14;
    result = (result >> ((31 - shift) >> 1)) ^ mask;
  } else {
    result = rsp.rcp[index];
    result = (0x10000 | result) << 14;
    result = (result >> (31 - shift)) ^ mask;
  }
  return result;
}

static NOINLINE void vec_slow(u32 op) {
  u32 e = (op >> 21) & 15, de = (op >> 11) & 7;
  VReg *vd = &rsp.v[(op >> 6) & 31];
  VReg *vsr = &rsp.v[(op >> 11) & 31];
  VReg *vtr = &rsp.v[(op >> 16) & 31];
  VReg vt; vt.v = vbroadcast(vtr->v, e);
  u32 fn = op & 0x3F;
  switch (fn) {
    case 0x02: case 0x0A: {   // VRNDP / VRNDN
      u32 vsn = (op >> 11) & 31;
      for (int n = 0; n < 8; n++) {
        s32 product = vt.e[n];
        if (vsn & 1) product = (s32)((u32)product << 16);
        s64 acc = ((s64)rsp.acc_hm[n] << 16) | rsp.acc_l.u[n];
        if (fn == 0x0A && acc < 0) acc = (s64)((u64)(acc + product) << 16) >> 16;
        if (fn == 0x02 && acc >= 0) acc = (s64)((u64)(acc + product) << 16) >> 16;
        rsp.acc_hm[n] = (s32)(acc >> 16);
        rsp.acc_l.u[n] = (u16)acc;
        s32 t = (s32)(acc >> 16);
        vd->e[n] = t < -32768 ? -32768 : t > 32767 ? 32767 : t;
      }
      break;
    }
    case 0x03: {              // VMULQ
      for (int n = 0; n < 8; n++) {
        s32 product = (s32)vsr->e[n] * (s32)vt.e[n];
        if (product < 0) product += 31;
        rsp.acc_hm[n] = product;
        rsp.acc_l.u[n] = 0;
        s32 t = product >> 1;
        vd->e[n] = (t < -32768 ? -32768 : t > 32767 ? 32767 : t) & ~15;
      }
      break;
    }
    case 0x0B: {              // VMACQ
      for (int n = 0; n < 8; n++) {
        s32 product = rsp.acc_hm[n];
        if (product < 0 && !(product & (1 << 5))) product += 32;
        else if (product >= 32 && !(product & (1 << 5))) product -= 32;
        rsp.acc_hm[n] = product;
        s32 t = product >> 1;
        vd->e[n] = (t < -32768 ? -32768 : t > 32767 ? 32767 : t) & ~15;
      }
      break;
    }
    case 0x30: case 0x31: case 0x34: case 0x35: {   // VRCP / VRCPL / VRSQ / VRSQL
      int lmode = fn & 1;
      s32 input = (lmode && rsp.divdp) ? (s32)(((u32)(u16)rsp.divin << 16) | vtr->u[e & 7]) : (s32)vtr->e[e & 7];
      s32 result = rcp_calc(input, fn >= 0x34);
      rsp.divdp = 0;
      rsp.divout = (s16)(result >> 16);
      rsp.acc_l.v = vt.v;
      vd->e[de] = (s16)result;
      break;
    }
    case 0x32: case 0x36: {   // VRCPH / VRSQH
      rsp.acc_l.v = vt.v;
      rsp.divdp = 1;
      rsp.divin = vtr->e[e & 7];
      vd->e[de] = rsp.divout;
      break;
    }
    case 0x33: {              // VMOV
      vd->u[de] = vt.u[de];
      rsp.acc_l.v = vt.v;
      break;
    }
    default: {                // VZERO-style reserved ops
      rsp.acc_l.v = vsr->v + vt.v;
      vd->v = (v16){0};
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// LWC2 / SWC2
// ---------------------------------------------------------------------------
// Fast paths for the common case of whole 16-bit elements at even addresses (no wrap): move halfwords, not bytes.
static inline int ld_fast(VReg *vt, u32 addr, u32 e, u32 n) {
  addr &= 0xFFF;
  if (((e | addr) & 1) || e + n > 16 || addr + n > 0x1000) return 0;
  for (u32 i = 0; i < n; i += 2) vt->u[(e + i) >> 1] = *(u16 *)(DMEM + ((addr + i) ^ 2));
  return 1;
}
static inline int st_fast(const VReg *vt, u32 addr, u32 e, u32 n) {
  addr &= 0xFFF;
  if (((e | addr) & 1) || e + n > 16 || addr + n > 0x1000) return 0;
  for (u32 i = 0; i < n; i += 2) *(u16 *)(DMEM + ((addr + i) ^ 2)) = vt->u[(e + i) >> 1];
  return 1;
}

static NOINLINE void rsp_lwc2(u32 op) {
  u32 vtn = (op >> 16) & 31, e = (op >> 7) & 15;
  s32 imm = ((s32)(op << 25)) >> 25;
  u32 base = rsp.r[(op >> 21) & 31];
  VReg *vt = &rsp.v[vtn];
  u32 addr;
  switch ((op >> 11) & 31) {
    case 0x00: VB(*vt, e) = dm_r8(base + imm); break;                                           // LBV
    case 0x01: addr = base + imm * 2; if (ld_fast(vt, addr, e, 2)) break; for (u32 o = e; o < e + 2 && o < 16; o++) VB(*vt, o) = dm_r8(addr++); break;   // LSV
    case 0x02: addr = base + imm * 4; if (ld_fast(vt, addr, e, 4)) break; for (u32 o = e; o < e + 4 && o < 16; o++) VB(*vt, o) = dm_r8(addr++); break;   // LLV
    case 0x03: addr = base + imm * 8; if (ld_fast(vt, addr, e, 8)) break; for (u32 o = e; o < e + 8 && o < 16; o++) VB(*vt, o) = dm_r8(addr++); break;   // LDV
    case 0x04: {                                                                                // LQV
      addr = (base + imm * 16) & 0xFFF;
      if (e == 0 && !(addr & 15)) { v16 t = *(v16 *)(DMEM + addr); vt->v = __builtin_shufflevector(t, t, 1, 0, 3, 2, 5, 4, 7, 6); break; }
      u32 end = 16 + e - (addr & 15); if (end > 16) end = 16;
      if (end > e && ld_fast(vt, addr, e, end - e)) break;
      for (u32 o = e; o < end; o++) VB(*vt, o) = dm_r8(addr++);
      break;
    }
    case 0x05: {                                                                                // LRV
      addr = base + imm * 16;
      u32 start = 16 - ((addr & 15) - e);
      addr &= ~15u;
      if (start < 16 && ld_fast(vt, addr, start, 16 - start)) break;
      for (u32 o = start; o < 16; o++) VB(*vt, o) = dm_r8(addr++);
      break;
    }
    case 0x06: {                                                                                // LPV
      addr = base + imm * 8; u32 idx = (addr & 7) - e; addr &= ~7u;
      for (u32 o = 0; o < 8; o++) vt->u[o] = dm_r8(addr + ((idx + o) & 15)) << 8;
      break;
    }
    case 0x07: {                                                                                // LUV
      addr = base + imm * 8; u32 idx = (addr & 7) - e; addr &= ~7u;
      for (u32 o = 0; o < 8; o++) vt->u[o] = dm_r8(addr + ((idx + o) & 15)) << 7;
      break;
    }
    case 0x08: {                                                                                // LHV
      addr = base + imm * 16; u32 idx = (addr & 7) - e; addr &= ~7u;
      for (u32 o = 0; o < 8; o++) vt->u[o] = dm_r8(addr + ((idx + o * 2) & 15)) << 7;
      break;
    }
    case 0x09: {                                                                                // LFV
      addr = base + imm * 16; u32 idx = (addr & 7) - e; addr &= ~7u;
      VReg tmp;
      for (u32 o = 0; o < 4; o++) {
        tmp.u[o] = dm_r8(addr + ((idx + o * 4) & 15)) << 7;
        tmp.u[o + 4] = dm_r8(addr + ((idx + o * 4 + 8) & 15)) << 7;
      }
      u32 end = e + 8; if (end > 16) end = 16;
      for (u32 o = e; o < end; o++) VB(*vt, o) = VB(tmp, o);
      break;
    }
    case 0x0B: {                                                                                // LTV
      addr = base + imm * 16;
      u32 begin = addr & ~7u;
      addr = begin + ((e + (addr & 8)) & 15);
      u32 vtbase = vtn & ~7u, vtoff = e >> 1;
      for (u32 i = 0; i < 8; i++) {
        VB(rsp.v[vtbase + vtoff], i * 2) = dm_r8(addr++); if (addr == begin + 16) addr = begin;
        VB(rsp.v[vtbase + vtoff], i * 2 + 1) = dm_r8(addr++); if (addr == begin + 16) addr = begin;
        vtoff = (vtoff + 1) & 7;
      }
      break;
    }
    default: break;
  }
}

static NOINLINE void rsp_swc2(u32 op) {
  u32 vtn = (op >> 16) & 31, e = (op >> 7) & 15;
  s32 imm = ((s32)(op << 25)) >> 25;
  u32 base = rsp.r[(op >> 21) & 31];
  VReg *vt = &rsp.v[vtn];
  u32 addr;
  switch ((op >> 11) & 31) {
    case 0x00: dm_w8(base + imm, VB(*vt, e)); break;                                            // SBV
    case 0x01: addr = base + imm * 2; if (st_fast(vt, addr, e, 2)) break; for (u32 o = e; o < e + 2; o++) dm_w8(addr++, VB(*vt, o)); break;   // SSV
    case 0x02: addr = base + imm * 4; if (st_fast(vt, addr, e, 4)) break; for (u32 o = e; o < e + 4; o++) dm_w8(addr++, VB(*vt, o)); break;   // SLV
    case 0x03: addr = base + imm * 8; if (st_fast(vt, addr, e, 8)) break; for (u32 o = e; o < e + 8; o++) dm_w8(addr++, VB(*vt, o)); break;   // SDV
    case 0x04: {                                                                                // SQV
      addr = (base + imm * 16) & 0xFFF;
      if (e == 0 && !(addr & 15)) { v16 t = vt->v; *(v16 *)(DMEM + addr) = __builtin_shufflevector(t, t, 1, 0, 3, 2, 5, 4, 7, 6); break; }
      u32 end = e + (16 - (addr & 15));
      if (st_fast(vt, addr, e, end - e)) break;
      for (u32 o = e; o < end; o++) dm_w8(addr++, VB(*vt, o));
      break;
    }
    case 0x05: {                                                                                // SRV
      addr = base + imm * 16;
      u32 end = e + (addr & 15), b = 16 - (addr & 15);
      addr &= ~15u;
      for (u32 o = e; o < end; o++) dm_w8(addr++, VB(*vt, o + b));
      break;
    }
    case 0x06: {                                                                                // SPV
      addr = base + imm * 8;
      for (u32 o = e; o < e + 8; o++) {
        if ((o & 15) < 8) dm_w8(addr++, VB(*vt, (o & 7) << 1));
        else dm_w8(addr++, vt->u[o & 7] >> 7);
      }
      break;
    }
    case 0x07: {                                                                                // SUV
      addr = base + imm * 8;
      for (u32 o = e; o < e + 8; o++) {
        if ((o & 15) < 8) dm_w8(addr++, vt->u[o & 7] >> 7);
        else dm_w8(addr++, VB(*vt, (o & 7) << 1));
      }
      break;
    }
    case 0x08: {                                                                                // SHV
      addr = base + imm * 16; u32 idx = addr & 7; addr &= ~7u;
      for (u32 o = 0; o < 8; o++) {
        u32 b = e + o * 2;
        u8 val = (VB(*vt, b) << 1) | (VB(*vt, b + 1) >> 7);
        dm_w8(addr + ((idx + o * 2) & 15), val);
      }
      break;
    }
    case 0x09: {                                                                                // SFV
      addr = base + imm * 16; u32 b = addr & 7; addr &= ~7u;
      static const s8 map[16][4] = {
        {0, 1, 2, 3}, {6, 7, 4, 5}, {-1}, {-1}, {1, 2, 3, 0}, {7, 4, 5, 6}, {-1}, {-1},
        {4, 5, 6, 7}, {-1}, {-1}, {3, 0, 1, 2}, {5, 6, 7, 4}, {-1}, {-1}, {0, 1, 2, 3} };
      for (u32 k = 0; k < 4; k++) {
        u8 val = map[e][0] < 0 ? 0 : (u8)(vt->u[map[e][k]] >> 7);
        dm_w8(addr + ((b + k * 4) & 15), val);
      }
      break;
    }
    case 0x0A: {                                                                                // SWV
      addr = base + imm * 16; u32 b = addr & 7; addr &= ~7u;
      for (u32 o = e; o < e + 16; o++) dm_w8(addr + (b++ & 15), VB(*vt, o));
      break;
    }
    case 0x0B: {                                                                                // STV
      addr = base + imm * 16;
      u32 start = vtn & ~7u, el = 16 - (e & ~1u), b = (addr & 7) - (e & ~1u);
      addr &= ~7u;
      for (u32 o = start; o < start + 8; o++) {
        dm_w8(addr + (b++ & 15), VB(rsp.v[o], el++));
        dm_w8(addr + (b++ & 15), VB(rsp.v[o], el++));
      }
      break;
    }
    default: break;
  }
}

static u32 flags_get(VReg *lo, VReg *hi) {
  u32 r = 0;
  for (int n = 0; n < 8; n++) {
    if (lo->e[n]) r |= 1u << n;
    if (hi && hi->e[n]) r |= 1u << (8 + n);
  }
  return r;
}
static void flags_set(VReg *lo, VReg *hi, u32 v) {
  for (int n = 0; n < 8; n++) {
    lo->e[n] = (v & (1u << n)) ? -1 : 0;
    if (hi) hi->e[n] = (v & (1u << (8 + n))) ? -1 : 0;
  }
}

static NOINLINE void rsp_cop2_move(u32 op) {
  u32 rt = (op >> 16) & 31, rd = (op >> 11) & 31, e = (op >> 7) & 15;
  switch ((op >> 21) & 31) {
    case 0: rsp.r[rt] = (u32)(s32)(s16)((VB(rsp.v[rd], e) << 8) | VB(rsp.v[rd], e + 1)); break;   // MFC2
    case 2:                                                                                       // CFC2
      switch (rd & 3) {
        case 0: rsp.r[rt] = (u32)(s32)(s16)flags_get(&rsp.vco_l, &rsp.vco_h); break;
        case 1: rsp.r[rt] = (u32)(s32)(s16)flags_get(&rsp.vcc_l, &rsp.vcc_h); break;
        default: rsp.r[rt] = flags_get(&rsp.vce, 0); break;
      }
      break;
    case 4: VB(rsp.v[rd], e) = rsp.r[rt] >> 8; if (e != 15) VB(rsp.v[rd], e + 1) = rsp.r[rt]; break;   // MTC2
    case 6:                                                                                       // CTC2
      switch (rd & 3) {
        case 0: flags_set(&rsp.vco_l, &rsp.vco_h, rsp.r[rt]); break;
        case 1: flags_set(&rsp.vcc_l, &rsp.vcc_h, rsp.r[rt]); break;
        default: flags_set(&rsp.vce, 0, rsp.r[rt]); break;
      }
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Scalar unit
// ---------------------------------------------------------------------------
#define RR rsp.r
#define RS_ ((op >> 21) & 31)
#define RT_ ((op >> 16) & 31)
#define RD_ ((op >> 11) & 31)
#define SA_ ((op >> 6) & 31)
#define SI_ ((s32)(s16)op)
#define RBR(c) do { if (c) npc = (pc + 4 + ((u32)SI_ << 2)) & 0xFFC; } while (0)

void rsp_run(s32 n) { rsp_run_n(n); }
static s32 rsp_run_n(s32 n) {
  // pc / npc live in locals; only BREAK and MTC0 can halt the processor, so the status is re-checked just there
  u32 next = rsp.pc, npc = rsp.npc;
  s32 left = n;
  if (sys.sp_status & SP_HALT) return 0;
  while (left > 0) {
    u32 pc = next;
    u32 op = *(u32 *)(IMEM + pc);
    next = npc;
    npc = (npc + 4) & 0xFFC;
    left--;
    RR[0] = 0;
    switch (op >> 26) {
      case 0:
        switch (op & 0x3F) {
          case 0x00: RR[RD_] = RR[RT_] << SA_; break;
          case 0x02: RR[RD_] = RR[RT_] >> SA_; break;
          case 0x03: RR[RD_] = (u32)((s32)RR[RT_] >> SA_); break;
          case 0x04: RR[RD_] = RR[RT_] << (RR[RS_] & 31); break;
          case 0x06: RR[RD_] = RR[RT_] >> (RR[RS_] & 31); break;
          case 0x07: RR[RD_] = (u32)((s32)RR[RT_] >> (RR[RS_] & 31)); break;
          case 0x08: npc = RR[RS_] & 0xFFC; break;
          case 0x09: { u32 t = RR[RS_] & 0xFFC; RR[RD_] = (pc + 8) & 0xFFF; npc = t; break; }
          case 0x0D:
            sys.sp_status |= SP_HALT | SP_BROKE;
            if (sys.sp_status & 0x40) mi_raise(MI_SP);
            goto out;
          case 0x20: case 0x21: RR[RD_] = RR[RS_] + RR[RT_]; break;
          case 0x22: case 0x23: RR[RD_] = RR[RS_] - RR[RT_]; break;
          case 0x24: RR[RD_] = RR[RS_] & RR[RT_]; break;
          case 0x25: RR[RD_] = RR[RS_] | RR[RT_]; break;
          case 0x26: RR[RD_] = RR[RS_] ^ RR[RT_]; break;
          case 0x27: RR[RD_] = ~(RR[RS_] | RR[RT_]); break;
          case 0x2A: RR[RD_] = (s32)RR[RS_] < (s32)RR[RT_]; break;
          case 0x2B: RR[RD_] = RR[RS_] < RR[RT_]; break;
          default: break;
        }
        break;
      case 1:
        switch (RT_) {
          case 0x00: RBR((s32)RR[RS_] < 0); break;
          case 0x01: RBR((s32)RR[RS_] >= 0); break;
          case 0x10: { int c = (s32)RR[RS_] < 0; RR[31] = (pc + 8) & 0xFFF; RBR(c); break; }
          case 0x11: { int c = (s32)RR[RS_] >= 0; RR[31] = (pc + 8) & 0xFFF; RBR(c); break; }
          default: break;
        }
        break;
      case 2: npc = (op << 2) & 0xFFC; break;
      case 3: RR[31] = (pc + 8) & 0xFFF; npc = (op << 2) & 0xFFC; break;
      case 4: RBR(RR[RS_] == RR[RT_]); break;
      case 5: RBR(RR[RS_] != RR[RT_]); break;
      case 6: RBR((s32)RR[RS_] <= 0); break;
      case 7: RBR((s32)RR[RS_] > 0); break;
      case 8: case 9: RR[RT_] = RR[RS_] + (u32)SI_; break;
      case 10: RR[RT_] = (s32)RR[RS_] < SI_; break;
      case 11: RR[RT_] = RR[RS_] < (u32)SI_; break;
      case 12: RR[RT_] = RR[RS_] & (op & 0xFFFF); break;
      case 13: RR[RT_] = RR[RS_] | (op & 0xFFFF); break;
      case 14: RR[RT_] = RR[RS_] ^ (op & 0xFFFF); break;
      case 15: RR[RT_] = op << 16; break;
      case 16: {
        u32 rd = RD_ & 15;
        if (RS_ == 0) { u32 v = rd < 8 ? sp_read(rd) : dp_read(rd - 8); RR[RT_] = v; }
        else if (RS_ == 4) {
          if (rd < 8) sp_write(rd, RR[RT_]); else dp_write(rd - 8, RR[RT_]);
          if (unlikely(gpu_sync_request)) {   // stop here until the host has copied GPU results back
            if (dma_blocked) { dma_blocked = 0; npc = next; next = pc; left++; }   // the transfer has not happened: repeat it
            goto out;
          }
          if (sys.sp_status & SP_HALT) goto out;
        }
        break;
      }
      case 18:
        if (op & (1u << 25)) rsp_vec(op); else rsp_cop2_move(op);
        break;
      case 32: RR[RT_] = (u32)(s32)(s8)dm_r8(RR[RS_] + SI_); break;
      case 33: RR[RT_] = (u32)(s32)(s16)dm_r16(RR[RS_] + SI_); break;
      case 35: case 39: RR[RT_] = dm_r32(RR[RS_] + SI_); break;
      case 36: RR[RT_] = dm_r8(RR[RS_] + SI_); break;
      case 37: RR[RT_] = dm_r16(RR[RS_] + SI_); break;
      case 40: dm_w8(RR[RS_] + SI_, RR[RT_]); break;
      case 41: dm_w16(RR[RS_] + SI_, RR[RT_]); break;
      case 43: dm_w32(RR[RS_] + SI_, RR[RT_]); break;
      case 50: rsp_lwc2(op); break;
      case 58: rsp_swc2(op); break;
      default: break;
    }
  }
out:
  rsp.pc = next; rsp.npc = npc;
  rsp.icount += (u64)(n - left);
  return left;
}

void rsp_reset(void) {
  memset(&rsp, 0, sizeof rsp);
  memset(&sp_pend, 0, sizeof sp_pend); memset(&sp_cur, 0, sizeof sp_cur);
  rsp.npc = 4;
  rsp.rcp[0] = 0xFFFF;
  for (u32 i = 1; i < 512; i++) { u64 a = i + 512; u64 b = (1ull << 34) / a; rsp.rcp[i] = (u16)((b + 1) >> 8); }
  for (u32 i = 0; i < 512; i++) {
    u64 a = (i + 512) >> (i & 1);
    u64 b = (u64)__builtin_sqrt((double)(1ull << 44) / (double)a);   // close estimate, then settle exactly
    if (b < (1 << 17)) b = 1 << 17;
    while (b > (1 << 17) && a * b * b >= (1ull << 44)) b--;
    while (a * (b + 1) * (b + 1) < (1ull << 44)) b++;
    rsp.rsq[i] = (u16)(b >> 1);
  }
}
