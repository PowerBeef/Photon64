// VR4300 interpreter: MIPS III integer unit, COP0 (TLB, exceptions), COP1 (FPU).
#include "core.h"

CPU cpu;
uintptr_t map_r[1 << 20], map_w[1 << 20];

#define ST_IE 1u
#define ST_EXL 2u
#define ST_ERL 4u
#define ST_FR (1u << 26)
#define ST_CU1 (1u << 29)
#define ST_BEV (1u << 22)

// ---------------------------------------------------------------------------
// TLB
// ---------------------------------------------------------------------------
static void map_range(u32 va, u32 size, u32 pa, int valid, int dirty) {
  for (u32 off = 0; off < size; off += 0x1000) {
    u32 page = (va + off) >> 12;
    u32 p = pa + off;
    if (page >= 0x80000 && page < 0xC0000) continue;   // never touch kseg0/kseg1
    if (valid && p < sys.rdram_size) {
      map_r[page] = gpu_stale_pg[p >> 12] ? 0 : (uintptr_t)(rdram + p);
      map_w[page] = (dirty && !gpu_watch[p >> 12]) ? (uintptr_t)(rdram + p) : 0;
    } else {
      map_r[page] = 0; map_w[page] = 0;
    }
  }
}

static void tlb_map_entry(int i, int on) {
  TLBEntry *e = &cpu.tlb[i];
  u32 pm = e->mask | 0x1FFF;
  u32 half = (pm + 1) >> 1;
  u32 va = (u32)e->hi & ~pm;
  u32 asid = cpu.cp0[C0_ENTRYHI] & 0xFF;
  int match = e->g || ((e->hi & 0xFF) == asid);
  if (half > 0x1000000) half = 0x1000000;
  for (int k = 0; k < 2; k++) {
    u32 lo = k ? e->lo1 : e->lo0;
    u32 pa = ((lo >> 6) << 12) & ~(half - 1) & 0x3FFFFFFF;
    map_range(va + (k ? half : 0), half, pa, on && match && (lo & 2), lo & 4);
  }
}

void tlb_remap_all(void) {
  for (int i = 31; i >= 0; i--) tlb_map_entry(i, 0);
  for (int i = 31; i >= 0; i--) tlb_map_entry(i, 1);
}

static void tlb_write(int i) {
  TLBEntry *e = &cpu.tlb[i];
  tlb_map_entry(i, 0);
  u32 mask = cpu.cp0[C0_PAGEMASK] & 0x01FFE000;
  e->mask = mask;
  e->hi = cpu.cp0[C0_ENTRYHI] & ~(u64)mask & 0xC00000FFFFFFE0FFull;
  e->g = (cpu.cp0[C0_ENTRYLO0] & cpu.cp0[C0_ENTRYLO1] & 1);
  e->lo0 = cpu.cp0[C0_ENTRYLO0] & 0x03FFFFFE;
  e->lo1 = cpu.cp0[C0_ENTRYLO1] & 0x03FFFFFE;
  // re-establish everything (cheap enough; keeps overlap priority correct)
  for (int k = 31; k >= 0; k--) tlb_map_entry(k, 1);
}

// returns 1 and *pa on hit+valid; 0: miss, -1: invalid, -2: not dirty (when write)
static int tlb_lookup(u32 va, int write, u32 *pa) {
  u32 asid = cpu.cp0[C0_ENTRYHI] & 0xFF;
  for (int i = 0; i < 32; i++) {
    TLBEntry *e = &cpu.tlb[i];
    u32 pm = e->mask | 0x1FFF;
    if (((va ^ (u32)e->hi) & ~pm) != 0) continue;
    if (!e->g && (e->hi & 0xFF) != asid) continue;
    u32 half = (pm + 1) >> 1;
    u32 lo = (va & half) ? e->lo1 : e->lo0;
    if (!(lo & 2)) return -1;
    if (write && !(lo & 4)) return -2;
    *pa = ((((lo >> 6) << 12) & ~(half - 1)) | (va & (half - 1))) & 0x3FFFFFFF;
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Exceptions / interrupts
// ---------------------------------------------------------------------------
static void exc_enter(u32 code, u32 ce, u32 vec_off) {
  u32 st = cpu.cp0[C0_STATUS];
  u32 cause = (cpu.cp0[C0_CAUSE] & 0x0000FF00) | (code << 2) | (ce << 28);
  if (!(st & ST_EXL)) {
    if (cpu.delay) { cpu.cp0[C0_EPC] = (s64)(s32)(cpu.ipc - 4); cause |= 0x80000000u; }
    else cpu.cp0[C0_EPC] = (s64)(s32)cpu.ipc;
  } else {
    cause |= cpu.cp0[C0_CAUSE] & 0x80000000u;
    vec_off = 0x180;
  }
  cpu.cp0[C0_CAUSE] = cause;
  cpu.cp0[C0_STATUS] = st | ST_EXL;
  u32 base = (st & ST_BEV) ? 0xBFC00200u : 0x80000000u;
  cpu.pc = base + vec_off;
  cpu.npc = cpu.pc + 4;
  cpu.branch = 0;
}

void cpu_exception(u32 code, u32 ce) { exc_enter(code, ce, 0x180); }

static void tlb_exception(u32 va, u32 code, int miss) {
  cpu.cp0[C0_BADVADDR] = (s64)(s32)va;
  cpu.cp0[C0_CONTEXT] = (cpu.cp0[C0_CONTEXT] & ~0x7FFFF0ull) | ((va >> 9) & 0x7FFFF0);
  cpu.cp0[C0_XCONTEXT] = (cpu.cp0[C0_XCONTEXT] & ~0x1FFFFFFF0ull) | ((va >> 9) & 0x7FFFF0) | ((u64)(va >> 31) * 0x3 << 31);
  cpu.cp0[C0_ENTRYHI] = (cpu.cp0[C0_ENTRYHI] & 0xFF) | ((s64)(s32)va & 0xC00000FFFFFFE000ull);
  // XTLB refill vector when the current mode uses 64-bit addressing
  u32 st = cpu.cp0[C0_STATUS];
  u32 ksu = (st & (ST_EXL | ST_ERL)) ? 0 : ((st >> 3) & 3);
  u32 x64 = ksu == 0 ? (st & 0x80) : ksu == 1 ? (st & 0x40) : (st & 0x20);
  exc_enter(code, 0, miss ? (x64 ? 0x080 : 0x000) : 0x180);
}

static void addr_exception(u32 va, u32 code) {
  cpu.cp0[C0_BADVADDR] = (s64)(s32)va;
  cpu.cp0[C0_CONTEXT] = (cpu.cp0[C0_CONTEXT] & ~0x7FFFF0ull) | ((va >> 9) & 0x7FFFF0);
  cpu.cp0[C0_ENTRYHI] = (cpu.cp0[C0_ENTRYHI] & 0xFF) | ((s64)(s32)va & 0xC00000FFFFFFE000ull);
  exc_enter(code, 0, 0x180);
}

void cpu_check_irq(void) {
  u32 st = cpu.cp0[C0_STATUS];
  if ((st & 7) == 1 && (cpu.cp0[C0_CAUSE] & st & 0xFF00)) { cpu.irq_poll = 1; cpu.next_ev = 0; }
}

// Called between instructions by the scheduler loop.
static void cpu_take_irq(void) {
  u32 st = cpu.cp0[C0_STATUS];
  if ((st & 7) == 1 && (cpu.cp0[C0_CAUSE] & st & 0xFF00)) {
    cpu.ipc = cpu.pc;
    cpu.delay = cpu.branch;
    exc_enter(EXC_INT, 0, 0x180);
  }
}

// ---------------------------------------------------------------------------
// Memory slow paths
// ---------------------------------------------------------------------------
static int translate(u32 va, int write, u32 *pa) {
  if ((va & 0xC0000000u) == 0x80000000u) { *pa = va & 0x1FFFFFFF; return 1; }
  int r = tlb_lookup(va, write, pa);
  if (r == 1) return 1;
  if (r == -2) tlb_exception(va, EXC_MOD, 0);
  else tlb_exception(va, write ? EXC_TLBS : EXC_TLBL, r == 0);
  return 0;
}

// The access of the instruction being executed cannot be completed until the host has copied GPU results back:
// undo the instruction's bookkeeping so that it is executed again, and leave the run loop.
u32 cpu_restart, cpu_resume_one;
static NOINLINE int restart_insn(void) {
  cpu_restart = 0; cpu_resume_one = 1;
  cpu.npc = cpu.pc; cpu.pc = cpu.ipc; cpu.branch = cpu.delay; cpu.cycles -= cpu.cpi; cpu.next_ev = 0;
  return 0;
}

static NOINLINE int rd_slow(u32 va, u32 size, u64 *out) {
  u32 pa;
  if (va & (size - 1)) { addr_exception(va, EXC_ADEL); return 0; }
  if (!translate(va, 0, &pa)) return 0;
  if (size == 8) { u64 h = bus_read32(pa); *out = (h << 32) | bus_read32(pa + 4); return unlikely(cpu_restart) ? restart_insn() : 1; }
  // The PI bus latches (addr & ~1) and always returns 32 bits from there; the CPU then picks its bytes.
  u32 w = (pa >= 0x05000000 && pa < 0x1FC00000) ? bus_read32(pa) : bus_read32(pa & ~3u);
  if (unlikely(cpu_restart)) return restart_insn();
  if (size == 4) *out = w;
  else if (size == 2) *out = (w >> (8 * (2 - (pa & 2)))) & 0xFFFF;
  else *out = (w >> (8 * (3 - (pa & 3)))) & 0xFF;
  return 1;
}

static NOINLINE int wr_slow(u32 va, u32 size, u64 val) {
  u32 pa;
  if (va & (size - 1)) { addr_exception(va, EXC_ADES); return 0; }
  if (!translate(va, 1, &pa)) return 0;
  if (size == 8) { bus_write32(pa, val >> 32, 0xFFFFFFFF); bus_write32(pa + 4, (u32)val, 0xFFFFFFFF); }
  else if (size == 4) bus_write32(pa, (u32)val, 0xFFFFFFFF);
  else if (size == 2) { u32 sh = 8 * (2 - (pa & 2)); bus_write32(pa & ~3u, (u32)(val & 0xFFFF) << sh, 0xFFFFu << sh); }
  else { u32 sh = 8 * (3 - (pa & 3)); bus_write32(pa & ~3u, (u32)(val & 0xFF) << sh, 0xFFu << sh); }
  if (unlikely(cpu_restart)) return restart_insn();
  return 1;
}

static NOINLINE int fetch_slow(u32 pc, u32 *op) {
  u32 pa;
  if (pc & 3) { addr_exception(pc, EXC_ADEL); return 0; }
  if (!translate(pc, 0, &pa)) return 0;
  *op = bus_read32(pa);
  if (unlikely(cpu_restart)) { cpu_restart = 0; cpu_resume_one = 1; cpu.branch = cpu.delay; cpu.next_ev = 0; return 0; }   // (nothing else done yet)
  return 1;
}

// ---------------------------------------------------------------------------
// COP0
// ---------------------------------------------------------------------------
static u32 cp0_count(void) { return (u32)((cpu.cycles - cpu.count_base) >> 1); }

static void cp0_sched_compare(void) {
  u32 delta = (u32)cpu.cp0[C0_COMPARE] - cp0_count();
  u64 d = delta ? delta : 0x100000000ull;
  ev_set(EV_COMPARE, ((cpu.cycles - cpu.count_base) & ~1ull) + cpu.count_base + d * 2);
}

static u64 cp0_read(u32 reg) {
  switch (reg) {
    case C0_RANDOM: {
      u32 wired = cpu.cp0[C0_WIRED] & 31;
      u32 n = (u32)(cpu.cycles / cpu.cpi);
      return 31 - (n % (32 - wired));
    }
    case C0_COUNT: return cp0_count();
    case C0_7: case C0_21: case C0_22: case C0_23: case C0_24: case C0_25: case C0_31: return cpu.cp0_latch;
    default: return cpu.cp0[reg];
  }
}

static void cp0_write(u32 reg, u64 v) {
  cpu.cp0_latch = v;
  switch (reg) {
    case C0_INDEX: cpu.cp0[reg] = v & 0x8000003F; break;
    case C0_RANDOM: break;
    case C0_ENTRYLO0: case C0_ENTRYLO1: cpu.cp0[reg] = v & 0x3FFFFFFF; break;
    case C0_CONTEXT: cpu.cp0[reg] = (v & ~0x7FFFFFull) | (cpu.cp0[reg] & 0x7FFFF0); break;
    case C0_PAGEMASK: cpu.cp0[reg] = v & 0x01FFE000; break;
    case C0_WIRED: cpu.cp0[reg] = v & 0x3F; break;
    case C0_BADVADDR: break;
    case C0_COUNT: cpu.count_base = cpu.cycles - ((u64)(u32)v << 1); cp0_sched_compare(); break;
    case C0_ENTRYHI: {
      u64 old = cpu.cp0[reg];
      cpu.cp0[reg] = v & 0xC00000FFFFFFE0FFull;
      if ((old ^ v) & 0xFF) tlb_remap_all();
      break;
    }
    case C0_COMPARE: cpu.cp0[reg] = (u32)v; cpu.cp0[C0_CAUSE] &= ~0x8000ull; cp0_sched_compare(); break;
    case C0_STATUS: {
      cpu.cp0[reg] = v & 0xFF57FFFF;
      cpu.fmask = (v & ST_FR) ? 31 : 30;
      cpu_check_irq();
      break;
    }
    case C0_CAUSE: cpu.cp0[reg] = (cpu.cp0[reg] & ~0x300ull) | (v & 0x300); cpu_check_irq(); break;
    case C0_EPC: cpu.cp0[reg] = v; break;
    case C0_PRID: break;
    case C0_CONFIG: cpu.cp0[reg] = (v & 0x0F00800F) | 0x7006E460; break;
    case C0_LLADDR: cpu.cp0[reg] = (u32)v; break;
    case C0_WATCHLO: cpu.cp0[reg] = v & 0xFFFFFFFB; break;
    case C0_WATCHHI: cpu.cp0[reg] = v & 0xF; break;
    case C0_XCONTEXT: cpu.cp0[reg] = (v & 0xFFFFFFFE00000000ull) | (cpu.cp0[reg] & 0x1FFFFFFFFull); break;
    case C0_PERR: cpu.cp0[reg] = v & 0xFF; break;
    case C0_CACHEERR: break;
    case C0_TAGLO: cpu.cp0[reg] = v & 0x0FFFFFC0; break;
    case C0_TAGHI: cpu.cp0[reg] = 0; break;
    case C0_ERROREPC: cpu.cp0[reg] = v; break;
    default: break;
  }
}

static void cop0_exec(u32 op) {
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  switch (rs) {
    case 0: cpu.r[rt] = (s64)(s32)(u32)cp0_read(rd); break;       // MFC0
    case 1: cpu.r[rt] = (s64)cp0_read(rd); break;                 // DMFC0
    case 4: cp0_write(rd, (u64)(s64)(s32)(u32)cpu.r[rt]); break;  // MTC0
    case 5: cp0_write(rd, (u64)cpu.r[rt]); break;                 // DMTC0
    default:
      if (rs & 0x10) {
        switch (op & 0x3F) {
          case 1: {  // TLBR
            TLBEntry *e = &cpu.tlb[cpu.cp0[C0_INDEX] & 31];
            cpu.cp0[C0_PAGEMASK] = e->mask;
            cpu.cp0[C0_ENTRYHI] = e->hi;
            cpu.cp0[C0_ENTRYLO0] = e->lo0 | e->g;
            cpu.cp0[C0_ENTRYLO1] = e->lo1 | e->g;
            tlb_remap_all();
            break;
          }
          case 2: tlb_write(cpu.cp0[C0_INDEX] & 31); break;                 // TLBWI
          case 6: tlb_write((u32)cp0_read(C0_RANDOM) & 31); break;          // TLBWR
          case 8: {  // TLBP
            u32 asid = cpu.cp0[C0_ENTRYHI] & 0xFF;
            cpu.cp0[C0_INDEX] = 0x80000000u;
            for (int i = 0; i < 32; i++) {
              TLBEntry *e = &cpu.tlb[i];
              u32 pm = e->mask | 0x1FFF;
              if ((((u32)cpu.cp0[C0_ENTRYHI] ^ (u32)e->hi) & ~pm) == 0 && (e->g || (e->hi & 0xFF) == asid)) { cpu.cp0[C0_INDEX] = i; break; }
            }
            break;
          }
          case 0x18: {  // ERET
            u32 st = cpu.cp0[C0_STATUS];
            if (st & ST_ERL) { cpu.pc = (u32)cpu.cp0[C0_ERROREPC]; cpu.cp0[C0_STATUS] = st & ~ST_ERL; }
            else { cpu.pc = (u32)cpu.cp0[C0_EPC]; cpu.cp0[C0_STATUS] = st & ~ST_EXL; }
            cpu.npc = cpu.pc + 4;
            cpu.branch = 0;
            cpu.llbit = 0;
            cpu_check_irq();
            break;
          }
          default: break;
        }
      } else cpu_exception(EXC_RI, 0);
  }
}

// ---------------------------------------------------------------------------
// COP1
// ---------------------------------------------------------------------------
#ifdef __wasm__
#define f_sqrt __builtin_sqrt
#define f_sqrtf __builtin_sqrtf
#define f_rint __builtin_rint
#define f_trunc __builtin_trunc
#define f_floor __builtin_floor
#define f_ceil __builtin_ceil
#else
#include <math.h>
#define f_sqrt sqrt
#define f_sqrtf sqrtf
#define f_rint nearbyint
#define f_trunc trunc
#define f_floor floor
#define f_ceil ceil
#endif

#define XFER32(i) (((u32 *)&cpu.f[(i) & cpu.fmask]) + ((i) & ~cpu.fmask & 1))
#define XFER64(i) (&cpu.f[(i) & cpu.fmask])
#define SRC_S(i) (cpu.f[(i) & cpu.fmask].f)
#define SRC_D(i) (cpu.f[(i) & cpu.fmask].d)
#define SRC_W(i) ((s32)cpu.f[(i) & cpu.fmask].lo)
#define SRC_L(i) (cpu.f[(i) & cpu.fmask].s)
#define DST_S(i, v) do { float _v = (v); cpu.f[i].u = 0; cpu.f[i].f = _v; } while (0)
#define DST_D(i, v) do { cpu.f[i].d = (v); } while (0)
#define DST_W(i, v) do { s32 _v = (v); cpu.f[i].u = (u32)_v; } while (0)
#define DST_L(i, v) do { cpu.f[i].s = (v); } while (0)

// FCR31 exception model (cause 12-17: I U O Z V E, enables 7-11, flags 2-6), following hardware behaviour:
// subnormal / quiet-NaN inputs and out-of-range conversions raise the unmaskable "unimplemented operation".
enum { FE_I, FE_U, FE_O, FE_Z, FE_V };
static inline int fpe_set(int c) { cpu.fcr31 |= 1u << (12 + c); if (cpu.fcr31 & (1u << (7 + c))) return 1; cpu.fcr31 |= 1u << (2 + c); return 0; }
#define FPE_UNIMPL() do { cpu.fcr31 |= 1u << 17; cpu_exception(EXC_FPE, 0); return; } while (0)
#define FPE_IF(c) do { if (c) { cpu_exception(EXC_FPE, 0); return; } } while (0)

static inline double fp_round(double d, u32 mode) {
  switch (mode & 3) {
    case 0: return f_rint(d);
    case 1: return f_trunc(d);
    case 2: return f_ceil(d);
    default: return f_floor(d);
  }
}
static inline u32 f32_bits(float f) { union { float f; u32 u; } v; v.f = f; return v.u; }
static inline u64 f64_bits(double d) { union { double d; u64 u; } v; v.d = d; return v.u; }
static inline int f32_class(float f) {   // 0 normal/zero/inf, 1 subnormal, 2 quiet NaN (MIPS legacy), 3 signaling NaN
  u32 u = f32_bits(f), e = (u >> 23) & 0xFF, m = u & 0x7FFFFF;
  if (e == 0) return m ? 1 : 0;
  if (e == 0xFF && m) return (u & 0x400000) ? 3 : 2;
  return 0;
}
static inline int f64_class(double d) {
  u64 u = f64_bits(d); u32 e = (u32)(u >> 52) & 0x7FF; u64 m = u & 0xFFFFFFFFFFFFFull;
  if (e == 0) return m ? 1 : 0;
  if (e == 0x7FF && m) return (u & 0x8000000000000ull) ? 3 : 2;
  return 0;
}
static inline void two_prod(double a, double b, double *p, double *e) {
  const double C = 134217729.0;
  double ac = a * C, ah = ac - (ac - a), al = a - ah;
  double bc = b * C, bh = bc - (bc - b), bl = b - bh;
  *p = a * b; *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}
static inline float fp_flush_f(float f, u32 rm) {
  int neg = (f32_bits(f) >> 31) != 0;
  union { u32 u; float f; } mn = { 0x00800000u | (neg ? 0x80000000u : 0) }, z = { neg ? 0x80000000u : 0 };
  if ((rm & 3) == 2) return neg ? z.f : mn.f;
  if ((rm & 3) == 3) return neg ? mn.f : z.f;
  return z.f;
}
static inline double fp_flush_d(double d, u32 rm) {
  int neg = (f64_bits(d) >> 63) != 0;
  union { u64 u; double d; } mn = { 0x0010000000000000ull | (neg ? 0x8000000000000000ull : 0) }, z = { neg ? 0x8000000000000000ull : 0 };
  if ((rm & 3) == 2) return neg ? z.d : mn.d;
  if ((rm & 3) == 3) return neg ? mn.d : z.d;
  return z.d;
}

static inline void fp_cmp(u32 cond, int unord, int eq, int lt) {
  int r = ((cond & 1) && unord) || ((cond & 2) && eq) || ((cond & 4) && lt);
  if (r) cpu.fcr31 |= 1u << 23; else cpu.fcr31 &= ~(1u << 23);
}

// Shared tail of arithmetic results: maps IEEE outcomes onto the VR4300's flags/traps. Returns 1 if a trap was taken.
static int fp_finish_s(float *res, int nan_in, int divz, int inexact, int tiny) {
  float f = *res;
  u32 u = f32_bits(f), e = (u >> 23) & 0xFF, m = u & 0x7FFFFF;
  int trap = 0;
  if (divz) trap |= fpe_set(FE_Z);
  if (e == 0xFF && !divz && !nan_in) {
    if (m) trap |= fpe_set(FE_V); else { trap |= fpe_set(FE_O); trap |= fpe_set(FE_I); }
  } else if ((e == 0 && m) || tiny) {
    if (!(cpu.fcr31 & (1u << 24)) || (cpu.fcr31 & 0x180)) { cpu.fcr31 |= 1u << 17; cpu_exception(EXC_FPE, 0); return 1; }
    fpe_set(FE_U); fpe_set(FE_I);
    if (e == 0 && m) *res = fp_flush_f(f, cpu.fcr31);
  } else if (inexact) trap |= fpe_set(FE_I);
  if (trap) { cpu_exception(EXC_FPE, 0); return 1; }
  if (e == 0xFF && m) { union { u32 u; float f; } n = { 0x7FBFFFFF }; *res = n.f; }
  return 0;
}
static int fp_finish_d(double *res, int nan_in, int divz, int inexact, int tiny) {
  double d = *res;
  u64 u = f64_bits(d); u32 e = (u32)(u >> 52) & 0x7FF; u64 m = u & 0xFFFFFFFFFFFFFull;
  int trap = 0;
  if (divz) trap |= fpe_set(FE_Z);
  if (e == 0x7FF && !divz && !nan_in) {
    if (m) trap |= fpe_set(FE_V); else { trap |= fpe_set(FE_O); trap |= fpe_set(FE_I); }
  } else if ((e == 0 && m) || tiny) {
    if (!(cpu.fcr31 & (1u << 24)) || (cpu.fcr31 & 0x180)) { cpu.fcr31 |= 1u << 17; cpu_exception(EXC_FPE, 0); return 1; }
    fpe_set(FE_U); fpe_set(FE_I);
    if (e == 0 && m) *res = fp_flush_d(d, cpu.fcr31);
  } else if (inexact) trap |= fpe_set(FE_I);
  if (trap) { cpu_exception(EXC_FPE, 0); return 1; }
  if (e == 0x7FF && m) { union { u64 u; double d; } n = { 0x7FF7FFFFFFFFFFFFull }; *res = n.d; }
  return 0;
}

// input screening; returns 1 if trapped. *nan set when an (untrapped) signaling NaN is present.
static int fp_in1(int cls, int *nan) {
  if (cls == 1 || cls == 2) { cpu.fcr31 |= 1u << 17; cpu_exception(EXC_FPE, 0); return 1; }
  if (cls == 3) { if (fpe_set(FE_V)) { cpu_exception(EXC_FPE, 0); return 1; } *nan = 1; }
  return 0;
}
static int fp_in2(int c1, int c2, int *nan) {
  if (c1 == 2 || c2 == 2 || c1 == 1 || c2 == 1) { cpu.fcr31 |= 1u << 17; cpu_exception(EXC_FPE, 0); return 1; }
  if (c1 == 3 || c2 == 3) { if (fpe_set(FE_V)) { cpu_exception(EXC_FPE, 0); return 1; } *nan = 1; }
  return 0;
}

// float -> integer conversions
static int fp_conv_w(double d, int cls, u32 mode, s32 *out) {
  u64 u = f64_bits(d);
  if (cls || ((u >> 52) & 0x7FF) == 0x7FF || !(d < 2147483648.0 && d >= -2147483648.0)) { cpu.fcr31 |= 1u << 17; cpu_exception(EXC_FPE, 0); return 1; }
  double r = fp_round(d, mode);
  if (!(r < 2147483648.0 && r >= -2147483648.0)) { cpu.fcr31 |= 1u << 17; cpu_exception(EXC_FPE, 0); return 1; }
  if (r != d && fpe_set(FE_I)) { cpu_exception(EXC_FPE, 0); return 1; }
  *out = (s32)r;
  return 0;
}
static int fp_conv_l(double d, int cls, u32 mode, s64 *out) {
  u64 u = f64_bits(d);
  if (cls || ((u >> 52) & 0x7FF) == 0x7FF || !(d < 9007199254740992.0 && d > -9007199254740992.0)) { cpu.fcr31 |= 1u << 17; cpu_exception(EXC_FPE, 0); return 1; }
  double r = fp_round(d, mode);
  if (r != d && fpe_set(FE_I)) { cpu_exception(EXC_FPE, 0); return 1; }
  *out = (s64)r;
  return 0;
}

static NOINLINE void cop1_exec(u32 op) {
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, fs = (op >> 11) & 31, fd = (op >> 6) & 31, fn = op & 0x3F;
  switch (rs) {
    case 0: cpu.r[rt] = (s64)(s32)*XFER32(fs); return;                 // MFC1
    case 1: cpu.r[rt] = XFER64(fs)->s; return;                         // DMFC1
    case 2:                                                            // CFC1
      if (fs == 31) cpu.r[rt] = (s64)(s32)cpu.fcr31;
      else if (fs == 0) cpu.r[rt] = 0xA00;
      else cpu.r[rt] = 0;
      return;
    case 4: *XFER32(fs) = (u32)cpu.r[rt]; return;                      // MTC1
    case 5: XFER64(fs)->s = cpu.r[rt]; return;                         // DMTC1
    case 6:                                                            // CTC1
      if (fs == 31) {
        cpu.fcr31 = (u32)cpu.r[rt] & 0x0183FFFF;
        if ((cpu.fcr31 & (1u << 17)) || (((cpu.fcr31 >> 12) & 0x1F) & ((cpu.fcr31 >> 7) & 0x1F))) cpu_exception(EXC_FPE, 0);
      }
      return;
    case 8: {                                                          // BC1
      int c = (cpu.fcr31 >> 23) & 1;
      int want = rt & 1, likely = rt & 2;
      u32 target = cpu.pc + ((s32)(s16)op << 2);
      cpu.fcr31 &= ~0x3F000u;
      if (c == want) { cpu.npc = target; cpu.branch = 1; }
      else if (likely) { cpu.pc += 4; cpu.npc += 4; }
      else cpu.branch = 1;
      return;
    }
    case 16: {                                                         // S
      float a = SRC_S(fs), b = SRC_S(rt), r;
      int nan = 0;
      if (fn == 6) { cpu.f[fd].u = XFER64(fs)->u; return; }           // MOV.S copies the full register
      cpu.fcr31 &= ~0x3F000u;
      if (fn >= 48) {
        int na = a != a, nb = b != b;
        if (na || nb) {
          int quiet = !(fn & 8);
          if (na && (!quiet || f32_class(a) == 3) && fpe_set(FE_V)) { cpu_exception(EXC_FPE, 0); return; }
          if (nb && (!quiet || f32_class(b) == 3) && fpe_set(FE_V)) { cpu_exception(EXC_FPE, 0); return; }
        }
        fp_cmp(fn & 15, na || nb, a == b, a < b);
        return;
      }
      switch (fn) {
        case 0: case 1: case 2: case 3: {
          if (fp_in2(f32_class(a), f32_class(b), &nan)) return;
          double da = a, db = b, d; int inexact = 0, divz = 0, tiny = 0;
          if (fn == 0 || fn == 1) {
            if (fn == 1) db = -db;
            d = da + db; r = (float)d;
            double bb = d - da, err = (da - (d - bb)) + (db - bb);
            inexact = (double)r != d || err != 0;
          } else if (fn == 2) { d = da * db; r = (float)d; inexact = (double)r != d; tiny = r == 0 && a != 0 && b != 0; }
          else {
            d = da / db; r = (float)d;
            divz = b == 0 && a != 0 && (f32_bits(a) & 0x7F800000) != 0x7F800000;
            inexact = !divz && (f32_bits(r) & 0x7F800000) != 0x7F800000 && (double)r * db != da;
            tiny = r == 0 && a != 0 && (f32_bits(b) & 0x7F800000) != 0x7F800000;
          }
          if (fp_finish_s(&r, nan, divz, inexact, tiny)) return;
          DST_S(fd, r); return;
        }
        case 4:
          if (fp_in1(f32_class(a), &nan)) return;
          r = f_sqrtf(a);
          if (fp_finish_s(&r, nan, 0, r == r && (double)r * (double)r != (double)a, 0)) return;
          DST_S(fd, r); return;
        case 5: case 7:
          if (fp_in1(f32_class(a), &nan)) return;
          r = fn == 5 ? __builtin_fabsf(a) : -a;
          if (fp_finish_s(&r, nan, 0, 0, 0)) return;
          DST_S(fd, r); return;
        case 8: case 9: case 10: case 11: case 37: { s64 v; if (fp_conv_l(a, f32_class(a), fn == 37 ? cpu.fcr31 : fn, &v)) return; DST_L(fd, v); return; }
        case 12: case 13: case 14: case 15: case 36: { s32 v; if (fp_conv_w(a, f32_class(a), fn == 36 ? cpu.fcr31 : fn, &v)) return; DST_W(fd, v); return; }
        case 33: { if (fp_in1(f32_class(a), &nan)) return; double d = (double)a; if (fp_finish_d(&d, nan, 0, 0, 0)) return; DST_D(fd, d); return; }
        default: FPE_UNIMPL();
      }
    }
    case 17: {                                                         // D
      double a = SRC_D(fs), b = SRC_D(rt), r;
      int nan = 0;
      if (fn == 6) { cpu.f[fd].u = XFER64(fs)->u; return; }
      cpu.fcr31 &= ~0x3F000u;
      if (fn >= 48) {
        int na = a != a, nb = b != b;
        if (na || nb) {
          int quiet = !(fn & 8);
          if (na && (!quiet || f64_class(a) == 3) && fpe_set(FE_V)) { cpu_exception(EXC_FPE, 0); return; }
          if (nb && (!quiet || f64_class(b) == 3) && fpe_set(FE_V)) { cpu_exception(EXC_FPE, 0); return; }
        }
        fp_cmp(fn & 15, na || nb, a == b, a < b);
        return;
      }
      switch (fn) {
        case 0: case 1: case 2: case 3: {
          if (fp_in2(f64_class(a), f64_class(b), &nan)) return;
          int inexact = 0, divz = 0, tiny = 0;
          int afin = (f64_bits(a) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull, bfin = (f64_bits(b) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
          if (fn == 0 || fn == 1) {
            double db = fn == 1 ? -b : b;
            r = a + db;
            double bb = r - a, err = (a - (r - bb)) + (db - bb);
            inexact = err != 0;
          } else if (fn == 2) {
            double p, e; two_prod(a, b, &p, &e); r = p;
            inexact = afin && bfin && e != 0; tiny = r == 0 && a != 0 && b != 0;
          } else {
            r = a / b;
            divz = b == 0 && a != 0 && afin;
            if (!divz && afin && bfin && r == r) { double p, e; two_prod(r, b, &p, &e); inexact = ((a - p) - e) != 0; }
            tiny = r == 0 && a != 0 && bfin;
          }
          if (fp_finish_d(&r, nan, divz, inexact, tiny)) return;
          DST_D(fd, r); return;
        }
        case 4: {
          if (fp_in1(f64_class(a), &nan)) return;
          r = f_sqrt(a);
          int inexact = 0;
          if (r == r && (f64_bits(a) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull) { double p, e; two_prod(r, r, &p, &e); inexact = ((a - p) - e) != 0; }
          if (fp_finish_d(&r, nan, 0, inexact, 0)) return;
          DST_D(fd, r); return;
        }
        case 5: case 7:
          if (fp_in1(f64_class(a), &nan)) return;
          r = fn == 5 ? __builtin_fabs(a) : -a;
          if (fp_finish_d(&r, nan, 0, 0, 0)) return;
          DST_D(fd, r); return;
        case 8: case 9: case 10: case 11: case 37: { s64 v; if (fp_conv_l(a, f64_class(a), fn == 37 ? cpu.fcr31 : fn, &v)) return; DST_L(fd, v); return; }
        case 12: case 13: case 14: case 15: case 36: { s32 v; if (fp_conv_w(a, f64_class(a), fn == 36 ? cpu.fcr31 : fn, &v)) return; DST_W(fd, v); return; }
        case 32: {
          if (fp_in1(f64_class(a), &nan)) return;
          float f = (float)a;
          if (fp_finish_s(&f, nan, 0, (double)f != a, f == 0 && a != 0)) return;
          DST_S(fd, f); return;
        }
        default: FPE_UNIMPL();
      }
    }
    case 20: {                                                         // W
      s32 a = SRC_W(fs);
      cpu.fcr31 &= ~0x3F000u;
      if (fn == 32) { float f = (float)a; if ((s64)f != (s64)a && fpe_set(FE_I)) { cpu_exception(EXC_FPE, 0); return; } DST_S(fd, f); }
      else if (fn == 33) DST_D(fd, (double)a);
      else FPE_UNIMPL();
      return;
    }
    case 21: {                                                         // L
      s64 a = SRC_L(fs);
      cpu.fcr31 &= ~0x3F000u;
      if (fn != 32 && fn != 33) FPE_UNIMPL();
      if (a >= (s64)0x0080000000000000ll || a < (s64)0xFF80000000000000ull) FPE_UNIMPL();
      if (fn == 32) { float f = (float)a; if ((s64)f != a && fpe_set(FE_I)) { cpu_exception(EXC_FPE, 0); return; } DST_S(fd, f); }
      else { double d = (double)a; if ((s64)d != a && fpe_set(FE_I)) { cpu_exception(EXC_FPE, 0); return; } DST_D(fd, d); }
      return;
    }
    default: return;
  }
}

// ---------------------------------------------------------------------------
// Integer helpers
// ---------------------------------------------------------------------------
static void mulu128(u64 a, u64 b, u64 *hi, u64 *lo) {
  u64 a0 = (u32)a, a1 = a >> 32, b0 = (u32)b, b1 = b >> 32;
  u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  u64 mid = (p00 >> 32) + (u32)p01 + (u32)p10;
  *lo = (mid << 32) | (u32)p00;
  *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

#define R cpu.r
#define RSI ((op >> 21) & 31)
#define RTI ((op >> 16) & 31)
#define RDI ((op >> 11) & 31)
#define SAI ((op >> 6) & 31)
#define SIMM ((s32)(s16)op)
#define UIMMV ((u64)(op & 0xFFFF))
#define EA ((u32)R[RSI] + (u32)SIMM)

#define BRANCH(cond) do { if (cond) { cpu.npc = cpu.pc + ((u32)SIMM << 2); } cpu.branch = 1; } while (0)
#define BRANCHL(cond) do { if (cond) { cpu.npc = cpu.pc + ((u32)SIMM << 2); cpu.branch = 1; } else { cpu.pc += 4; cpu.npc += 4; } } while (0)

#define LOAD(size, expr_fast, T) do { \
    u32 va = EA; uintptr_t e = map_r[va >> 12]; \
    if (likely(e && !(va & (size - 1)))) { u8 *p = (u8 *)e; u32 o = va & 0xFFF; (void)p; (void)o; R[RTI] = (T)(expr_fast); } \
    else { u64 v; if (rd_slow(va, size, &v)) R[RTI] = (T)v; } } while (0)

#define STORE(size, stmt_fast) do { \
    u32 va = EA; uintptr_t e = map_w[va >> 12]; \
    if (likely(e && !(va & (size - 1)))) { u8 *p = (u8 *)e; u32 o = va & 0xFFF; (void)p; (void)o; stmt_fast; } \
    else wr_slow(va, size, (u64)R[RTI]); } while (0)

static inline int rd64u(u32 va, u64 *v) {   // aligned dword read for LDL/LDR etc.
  uintptr_t e = map_r[va >> 12];
  if (likely(e)) { u32 *p = (u32 *)(e + (va & 0xFF8)); *v = ((u64)p[0] << 32) | p[1]; return 1; }
  return rd_slow(va, 8, v);
}
static inline int rd32u(u32 va, u32 *v) {
  uintptr_t e = map_r[va >> 12];
  if (likely(e)) { *v = *(u32 *)(e + (va & 0xFFC)); return 1; }
  u64 t; if (!rd_slow(va, 4, &t)) return 0; *v = (u32)t; return 1;
}

static NOINLINE void exec_special_slow(u32 op);

void cpu_run(void) {
  while (cpu.cycles < cpu.next_ev) {
    u32 pc = cpu.pc, op;
    uintptr_t fe = map_r[pc >> 12];
    cpu.ipc = pc;
    cpu.delay = cpu.branch; cpu.branch = 0;
    if (likely(fe && !(pc & 3))) op = *(u32 *)(fe + (pc & 0xFFC));
    else if (!fetch_slow(pc, &op)) continue;
    R[0] = 0;
    cpu.pc = cpu.npc; cpu.npc += 4;
    cpu.cycles += cpu.cpi;
    switch (op >> 26) {
      case 0:
        switch (op & 0x3F) {
          case 0x00: R[RDI] = (s64)(s32)((u32)R[RTI] << SAI); break;                       // SLL
          case 0x02: R[RDI] = (s64)(s32)((u32)R[RTI] >> SAI); break;                       // SRL
          case 0x03: R[RDI] = (s64)(s32)(R[RTI] >> SAI); break;                            // SRA (uses 64-bit source)
          case 0x04: R[RDI] = (s64)(s32)((u32)R[RTI] << (R[RSI] & 31)); break;             // SLLV
          case 0x06: R[RDI] = (s64)(s32)((u32)R[RTI] >> (R[RSI] & 31)); break;             // SRLV
          case 0x07: R[RDI] = (s64)(s32)(R[RTI] >> (R[RSI] & 31)); break;                  // SRAV
          case 0x08: cpu.npc = (u32)R[RSI]; cpu.branch = 1; break;                         // JR
          case 0x09: { u32 t = (u32)R[RSI]; R[RDI] = (s64)(s32)(pc + 8); cpu.npc = t; cpu.branch = 1; break; } // JALR
          case 0x10: R[RDI] = cpu.hi; break;
          case 0x11: cpu.hi = R[RSI]; break;
          case 0x12: R[RDI] = cpu.lo; break;
          case 0x13: cpu.lo = R[RSI]; break;
          case 0x18: { s64 p = (s64)(s32)R[RSI] * (s64)(s32)R[RTI]; cpu.lo = (s32)p; cpu.hi = (s32)(p >> 32); break; }   // MULT
          case 0x19: { u64 p = (u64)(u32)R[RSI] * (u64)(u32)R[RTI]; cpu.lo = (s32)p; cpu.hi = (s32)(p >> 32); break; }   // MULTU
          case 0x21: R[RDI] = (s64)(s32)((u32)R[RSI] + (u32)R[RTI]); break;                // ADDU
          case 0x23: R[RDI] = (s64)(s32)((u32)R[RSI] - (u32)R[RTI]); break;                // SUBU
          case 0x24: R[RDI] = R[RSI] & R[RTI]; break;
          case 0x25: R[RDI] = R[RSI] | R[RTI]; break;
          case 0x26: R[RDI] = R[RSI] ^ R[RTI]; break;
          case 0x27: R[RDI] = ~(R[RSI] | R[RTI]); break;
          case 0x2A: R[RDI] = R[RSI] < R[RTI]; break;
          case 0x2B: R[RDI] = (u64)R[RSI] < (u64)R[RTI]; break;
          case 0x2D: R[RDI] = R[RSI] + R[RTI]; break;                                      // DADDU
          case 0x2F: R[RDI] = R[RSI] - R[RTI]; break;                                      // DSUBU
          case 0x38: R[RDI] = R[RTI] << SAI; break;                                        // DSLL
          case 0x3A: R[RDI] = (s64)((u64)R[RTI] >> SAI); break;                            // DSRL
          case 0x3B: R[RDI] = R[RTI] >> SAI; break;                                        // DSRA
          case 0x3C: R[RDI] = R[RTI] << (SAI + 32); break;                                 // DSLL32
          case 0x3E: R[RDI] = (s64)((u64)R[RTI] >> (SAI + 32)); break;                     // DSRL32
          case 0x3F: R[RDI] = R[RTI] >> (SAI + 32); break;                                 // DSRA32
          default: exec_special_slow(op); break;
        }
        break;
      case 1: {
        s64 v = R[RSI];
        switch (RTI) {
          case 0x00: BRANCH(v < 0); break;
          case 0x01: BRANCH(v >= 0); break;
          case 0x02: BRANCHL(v < 0); break;
          case 0x03: BRANCHL(v >= 0); break;
          case 0x08: if (v >= (s64)SIMM) cpu_exception(EXC_TR, 0); break;
          case 0x09: if ((u64)v >= (u64)(s64)SIMM) cpu_exception(EXC_TR, 0); break;
          case 0x0A: if (v < (s64)SIMM) cpu_exception(EXC_TR, 0); break;
          case 0x0B: if ((u64)v < (u64)(s64)SIMM) cpu_exception(EXC_TR, 0); break;
          case 0x0C: if (v == (s64)SIMM) cpu_exception(EXC_TR, 0); break;
          case 0x0E: if (v != (s64)SIMM) cpu_exception(EXC_TR, 0); break;
          case 0x10: R[31] = (s64)(s32)(pc + 8); BRANCH(v < 0); break;
          case 0x11: R[31] = (s64)(s32)(pc + 8); BRANCH(v >= 0); break;
          case 0x12: R[31] = (s64)(s32)(pc + 8); BRANCHL(v < 0); break;
          case 0x13: R[31] = (s64)(s32)(pc + 8); BRANCHL(v >= 0); break;
          default: cpu_exception(EXC_RI, 0); break;
        }
        break;
      }
      case 2: {                                                                             // J
        u32 t = (cpu.pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2);
        cpu.npc = t; cpu.branch = 1;
        if (unlikely(t == pc)) { u32 ds; if (rd32u(pc + 4, &ds) && ds == 0 && cpu.cycles < cpu.next_ev) { cpu.idle_skipped += cpu.next_ev - cpu.cycles; cpu.cycles += (cpu.next_ev - cpu.cycles + cpu.cpi - 1) / cpu.cpi * cpu.cpi; } }
        break;
      }
      case 3: R[31] = (s64)(s32)(pc + 8); cpu.npc = (cpu.pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2); cpu.branch = 1; break;   // JAL
      case 4:                                                                               // BEQ
        if (R[RSI] == R[RTI]) {
          cpu.npc = cpu.pc + ((u32)SIMM << 2);
          if (unlikely((op & 0xFFFF) == 0xFFFF && RSI == RTI)) {   // branch-to-self idle loop
            u32 ds;
            if (rd32u(pc + 4, &ds) && ds == 0 && cpu.cycles < cpu.next_ev) { cpu.idle_skipped += cpu.next_ev - cpu.cycles; cpu.cycles += (cpu.next_ev - cpu.cycles + cpu.cpi - 1) / cpu.cpi * cpu.cpi; }
          }
        }
        cpu.branch = 1;
        break;
      case 5: BRANCH(R[RSI] != R[RTI]); break;
      case 6: BRANCH(R[RSI] <= 0); break;
      case 7: BRANCH(R[RSI] > 0); break;
      case 8: { s32 a = (s32)R[RSI], res; if (__builtin_add_overflow(a, SIMM, &res)) cpu_exception(EXC_OV, 0); else R[RTI] = res; break; }  // ADDI
      case 9: R[RTI] = (s64)(s32)((u32)R[RSI] + (u32)SIMM); break;                          // ADDIU
      case 10: R[RTI] = R[RSI] < (s64)SIMM; break;
      case 11: R[RTI] = (u64)R[RSI] < (u64)(s64)SIMM; break;
      case 12: R[RTI] = R[RSI] & UIMMV; break;
      case 13: R[RTI] = R[RSI] | UIMMV; break;
      case 14: R[RTI] = R[RSI] ^ UIMMV; break;
      case 15: R[RTI] = (s64)(s32)(op << 16); break;
      case 16: if (!(cpu.cp0[C0_STATUS] & 0x10000006) && (cpu.cp0[C0_STATUS] & 0x18)) cpu_exception(EXC_CPU, 0); else cop0_exec(op); break;
      case 17: if (unlikely(!(cpu.cp0[C0_STATUS] & ST_CU1))) cpu_exception(EXC_CPU, 1); else cop1_exec(op); break;
      case 18: if (!(cpu.cp0[C0_STATUS] & (1u << 30))) cpu_exception(EXC_CPU, 2); else if (RSI > 6) cpu_exception(EXC_RI, 2); break;
      case 19: cpu_exception(EXC_RI, 0); break;
      case 20: BRANCHL(R[RSI] == R[RTI]); break;
      case 21: BRANCHL(R[RSI] != R[RTI]); break;
      case 22: BRANCHL(R[RSI] <= 0); break;
      case 23: BRANCHL(R[RSI] > 0); break;
      case 24: { s64 res; if (__builtin_add_overflow(R[RSI], (s64)SIMM, &res)) cpu_exception(EXC_OV, 0); else R[RTI] = res; break; }       // DADDI
      case 25: R[RTI] = R[RSI] + (s64)SIMM; break;                                          // DADDIU
      case 26: {                                                                            // LDL
        u32 va = EA; u64 m; if (!rd64u(va & ~7u, &m)) break;
        u32 sh = 8 * (va & 7);
        R[RTI] = (s64)((m << sh) | ((u64)R[RTI] & (sh ? (~0ull >> (64 - sh)) : 0)));
        break;
      }
      case 27: {                                                                            // LDR
        u32 va = EA; u64 m; if (!rd64u(va & ~7u, &m)) break;
        u32 sh = 8 * (7 - (va & 7));
        R[RTI] = (s64)((m >> sh) | ((u64)R[RTI] & (sh ? (~0ull << (64 - sh)) : 0)));
        break;
      }
      case 32: LOAD(1, p[o ^ 3], s8); break;                                                // LB
      case 33: LOAD(2, *(u16 *)(p + (o ^ 2)), s16); break;                                  // LH
      case 34: {                                                                            // LWL
        u32 va = EA, m; if (!rd32u(va & ~3u, &m)) break;
        u32 sh = 8 * (va & 3);
        R[RTI] = (s64)(s32)((m << sh) | ((u32)R[RTI] & (sh ? (0xFFFFFFFFu >> (32 - sh)) : 0)));
        break;
      }
      case 35: LOAD(4, *(u32 *)(p + o), s32); break;                                        // LW
      case 36: LOAD(1, p[o ^ 3], u8); break;                                                // LBU
      case 37: LOAD(2, *(u16 *)(p + (o ^ 2)), u16); break;                                  // LHU
      case 38: {                                                                            // LWR
        u32 va = EA, m; if (!rd32u(va & ~3u, &m)) break;
        u32 sh = 8 * (3 - (va & 3));
        u32 res = (m >> sh) | ((u32)R[RTI] & (sh ? (0xFFFFFFFFu << (32 - sh)) : 0));
        R[RTI] = (s64)(s32)res;
        break;
      }
      case 39: LOAD(4, *(u32 *)(p + o), u32); break;                                        // LWU
      case 40: STORE(1, p[o ^ 3] = (u8)R[RTI]); break;                                      // SB
      case 41: STORE(2, *(u16 *)(p + (o ^ 2)) = (u16)R[RTI]); break;                        // SH
      case 42: {                                                                            // SWL
        u32 va = EA, m; if (!rd32u(va & ~3u, &m)) break;
        u32 sh = 8 * (va & 3);
        u32 v = ((u32)R[RTI] >> sh) | (m & (sh ? (0xFFFFFFFFu << (32 - sh)) : 0));
        uintptr_t e = map_w[va >> 12];
        if (likely(e)) *(u32 *)(e + (va & 0xFFC)) = v; else wr_slow(va & ~3u, 4, v);
        break;
      }
      case 43: STORE(4, *(u32 *)(p + o) = (u32)R[RTI]); break;                              // SW
      case 44: {                                                                            // SDL
        u32 va = EA; u64 m; if (!rd64u(va & ~7u, &m)) break;
        u32 sh = 8 * (va & 7);
        u64 v = ((u64)R[RTI] >> sh) | (m & (sh ? (~0ull << (64 - sh)) : 0));
        wr_slow(va & ~7u, 8, v);
        break;
      }
      case 45: {                                                                            // SDR
        u32 va = EA; u64 m; if (!rd64u(va & ~7u, &m)) break;
        u32 sh = 8 * (7 - (va & 7));
        u64 v = ((u64)R[RTI] << sh) | (m & (sh ? (~0ull >> (64 - sh)) : 0));
        wr_slow(va & ~7u, 8, v);
        break;
      }
      case 46: {                                                                            // SWR
        u32 va = EA, m; if (!rd32u(va & ~3u, &m)) break;
        u32 sh = 8 * (3 - (va & 3));
        u32 v = ((u32)R[RTI] << sh) | (m & (sh ? (0xFFFFFFFFu >> (32 - sh)) : 0));
        uintptr_t e = map_w[va >> 12];
        if (likely(e)) *(u32 *)(e + (va & 0xFFC)) = v; else wr_slow(va & ~3u, 4, v);
        break;
      }
      case 47: break;                                                                       // CACHE
      case 48: { u32 va = EA; u64 v; if (rd_slow(va, 4, &v)) { R[RTI] = (s64)(s32)v; cpu.llbit = 1; u32 pa; if (translate(va, 0, &pa)) cpu.cp0[C0_LLADDR] = pa >> 4; } break; }   // LL
      case 49:                                                                              // LWC1
        if (unlikely(!(cpu.cp0[C0_STATUS] & ST_CU1))) cpu_exception(EXC_CPU, 1);
        else { u32 va = EA, v; if (unlikely(va & 3)) { u64 t; rd_slow(va, 4, &t); } else if (rd32u(va, &v)) *XFER32(RTI) = v; }
        break;
      case 52: { u32 va = EA; u64 v; if (rd_slow(va, 8, &v)) { R[RTI] = (s64)v; cpu.llbit = 1; u32 pa; if (translate(va, 0, &pa)) cpu.cp0[C0_LLADDR] = pa >> 4; } break; }        // LLD
      case 53:                                                                              // LDC1
        if (unlikely(!(cpu.cp0[C0_STATUS] & ST_CU1))) cpu_exception(EXC_CPU, 1);
        else { u32 va = EA; u64 v; if (unlikely(va & 7)) { rd_slow(va, 8, &v); break; } if (rd64u(va, &v)) XFER64(RTI)->u = v; }
        break;
      case 55: {                                                                            // LD
        u32 va = EA; uintptr_t e = map_r[va >> 12];
        if (likely(e && !(va & 7))) { u32 *p = (u32 *)(e + (va & 0xFFF)); R[RTI] = (s64)(((u64)p[0] << 32) | p[1]); }
        else { u64 v; if (rd_slow(va, 8, &v)) R[RTI] = (s64)v; }
        break;
      }
      case 56: if (cpu.llbit) { u32 va = EA; if (wr_slow(va, 4, (u32)R[RTI])) R[RTI] = 1; } else R[RTI] = 0; break;   // SC
      case 57:                                                                              // SWC1
        if (unlikely(!(cpu.cp0[C0_STATUS] & ST_CU1))) cpu_exception(EXC_CPU, 1);
        else { u32 va = EA; uintptr_t e = map_w[va >> 12]; u32 v = *XFER32(RTI);
          if (likely(e && !(va & 3))) *(u32 *)(e + (va & 0xFFF)) = v; else wr_slow(va, 4, v); }
        break;
      case 60: if (cpu.llbit) { u32 va = EA; if (wr_slow(va, 8, (u64)R[RTI])) R[RTI] = 1; } else R[RTI] = 0; break;   // SCD
      case 61:                                                                              // SDC1
        if (unlikely(!(cpu.cp0[C0_STATUS] & ST_CU1))) cpu_exception(EXC_CPU, 1);
        else { u32 va = EA; uintptr_t e = map_w[va >> 12]; u64 v = XFER64(RTI)->u;
          if (likely(e && !(va & 7))) { u32 *p = (u32 *)(e + (va & 0xFFF)); p[0] = (u32)(v >> 32); p[1] = (u32)v; } else wr_slow(va, 8, v); }
        break;
      case 63: {                                                                            // SD
        u32 va = EA; uintptr_t e = map_w[va >> 12]; u64 v = (u64)R[RTI];
        if (likely(e && !(va & 7))) { u32 *p = (u32 *)(e + (va & 0xFFF)); p[0] = (u32)(v >> 32); p[1] = (u32)v; } else wr_slow(va, 8, v);
        break;
      }
      default: cpu_exception(EXC_RI, 0); break;
    }
  }
}

static NOINLINE void exec_special_slow(u32 op) {
  switch (op & 0x3F) {
    case 0x0C: cpu_exception(EXC_SYS, 0); break;
    case 0x0D: cpu_exception(EXC_BP, 0); break;
    case 0x0F: break;                                                                       // SYNC
    case 0x14: R[RDI] = R[RTI] << (R[RSI] & 63); break;                                     // DSLLV
    case 0x16: R[RDI] = (s64)((u64)R[RTI] >> (R[RSI] & 63)); break;                         // DSRLV
    case 0x17: R[RDI] = R[RTI] >> (R[RSI] & 63); break;                                     // DSRAV
    case 0x1A: {                                                                            // DIV
      s32 a = (s32)R[RSI], b = (s32)R[RTI];
      if (b == 0) { cpu.lo = a < 0 ? 1 : -1; cpu.hi = a; }
      else if (a == (s32)0x80000000 && b == -1) { cpu.lo = a; cpu.hi = 0; }
      else { cpu.lo = a / b; cpu.hi = a % b; }
      break;
    }
    case 0x1B: {                                                                            // DIVU
      u32 a = (u32)R[RSI], b = (u32)R[RTI];
      if (b == 0) { cpu.lo = -1; cpu.hi = (s32)a; }
      else { cpu.lo = (s32)(a / b); cpu.hi = (s32)(a % b); }
      break;
    }
    case 0x1C: {                                                                            // DMULT
      s64 a = R[RSI], b = R[RTI]; u64 hi, lo;
      mulu128((u64)a, (u64)b, &hi, &lo);
      if (a < 0) hi -= (u64)b;
      if (b < 0) hi -= (u64)a;
      cpu.lo = (s64)lo; cpu.hi = (s64)hi;
      break;
    }
    case 0x1D: { u64 hi, lo; mulu128((u64)R[RSI], (u64)R[RTI], &hi, &lo); cpu.lo = (s64)lo; cpu.hi = (s64)hi; break; }   // DMULTU
    case 0x1E: {                                                                            // DDIV
      s64 a = R[RSI], b = R[RTI];
      if (b == 0) { cpu.lo = a < 0 ? 1 : -1; cpu.hi = a; }
      else if (a == (s64)0x8000000000000000ull && b == -1) { cpu.lo = a; cpu.hi = 0; }
      else { cpu.lo = a / b; cpu.hi = a % b; }
      break;
    }
    case 0x1F: {                                                                            // DDIVU
      u64 a = (u64)R[RSI], b = (u64)R[RTI];
      if (b == 0) { cpu.lo = -1; cpu.hi = (s64)a; }
      else { cpu.lo = (s64)(a / b); cpu.hi = (s64)(a % b); }
      break;
    }
    case 0x20: { s32 res; if (__builtin_add_overflow((s32)R[RSI], (s32)R[RTI], &res)) cpu_exception(EXC_OV, 0); else R[RDI] = res; break; }   // ADD
    case 0x22: { s32 res; if (__builtin_sub_overflow((s32)R[RSI], (s32)R[RTI], &res)) cpu_exception(EXC_OV, 0); else R[RDI] = res; break; }   // SUB
    case 0x2C: { s64 res; if (__builtin_add_overflow(R[RSI], R[RTI], &res)) cpu_exception(EXC_OV, 0); else R[RDI] = res; break; }             // DADD
    case 0x2E: { s64 res; if (__builtin_sub_overflow(R[RSI], R[RTI], &res)) cpu_exception(EXC_OV, 0); else R[RDI] = res; break; }             // DSUB
    case 0x30: if (R[RSI] >= R[RTI]) cpu_exception(EXC_TR, 0); break;
    case 0x31: if ((u64)R[RSI] >= (u64)R[RTI]) cpu_exception(EXC_TR, 0); break;
    case 0x32: if (R[RSI] < R[RTI]) cpu_exception(EXC_TR, 0); break;
    case 0x33: if ((u64)R[RSI] < (u64)R[RTI]) cpu_exception(EXC_TR, 0); break;
    case 0x34: if (R[RSI] == R[RTI]) cpu_exception(EXC_TR, 0); break;
    case 0x36: if (R[RSI] != R[RTI]) cpu_exception(EXC_TR, 0); break;
    default: cpu_exception(EXC_RI, 0); break;
  }
}

void cpu_reset(void) {
  memset(&cpu, 0, sizeof cpu);
  cpu.cpi = 4;
  cpu.fmask = 30;
  cpu.fcr0 = 0xA00;
  cpu.cp0[C0_RANDOM] = 31;
  cpu.cp0[C0_STATUS] = 0x241000E0;
  cpu.cp0[C0_CAUSE] = 0x30000000;
  cpu.fmask = 31;
  cpu.cp0[C0_PRID] = 0x00000B22;
  cpu.cp0[C0_CONFIG] = 0x7006E463;
  cpu.cp0[C0_EPC] = 0xFFFFFFFFFFFFFFFFull;
  cpu.cp0[C0_ERROREPC] = 0xFFFFFFFFFFFFFFFFull;
  cpu.cp0[C0_BADVADDR] = 0xFFFFFFFFFFFFFFFFull;
  cpu.cp0[C0_CONTEXT] = 0x007FFFF0;
  for (int i = 0; i < 32; i++) { cpu.tlb[i].hi = 0x80000000u; }   // unmatched
  for (int i = 0; i < EV_MAX; i++) cpu.ev[i] = EV_NEVER;
  memset(map_r, 0, sizeof map_r); memset(map_w, 0, sizeof map_w);
  for (u32 p = 0; p < sys.rdram_size; p += 0x1000) {
    uintptr_t h = (uintptr_t)(rdram + p);
    map_r[(0x80000000u + p) >> 12] = h; map_w[(0x80000000u + p) >> 12] = h;
    map_r[(0xA0000000u + p) >> 12] = h; map_w[(0xA0000000u + p) >> 12] = h;
  }
}

// Stale pages (see core.h) lose their fast load mapping.
void cpu_stale_page(u32 pg, int on) {
  u32 p = pg << 12;
  uintptr_t h = on ? 0 : (uintptr_t)(rdram + p);
  if (p >= sys.rdram_size) return;
  map_r[(0x80000000u + p) >> 12] = h; map_r[(0xA0000000u + p) >> 12] = h;
}

// (Un)watch one RDRAM page for the GPU renderer: watched pages lose their fast store mapping.
void cpu_watch_page(u32 pg, int on) {
  u32 p = pg << 12;
  uintptr_t h = on ? 0 : (uintptr_t)(rdram + p);
  if (p >= sys.rdram_size) return;
  map_w[(0x80000000u + p) >> 12] = h; map_w[(0xA0000000u + p) >> 12] = h;
}

void cpu_poll_irq(void) { cpu_take_irq(); }
void cpu_compare_resched(void) { cp0_sched_compare(); }
