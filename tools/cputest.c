// CPU semantics tests: SRA/SRAV shift the low 32 bits (not the full 64-bit GPR),
// and an exception between LL and SC invalidates the link (like ERET does).
// Drives cpu_run directly with hand-encoded instructions; no ROM needed.
//   cc -O2 -o out/cputest tools/cputest.c && ./out/cputest
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../src/n64.c"

void host_log(const char *msg, u32 a, u32 b) { (void)msg; (void)a; (void)b; }
void host_gpu_flush(void) {}

static int t_bad = 0, t_n = 0;
static void t_check(const char *name, int ok) { t_n++; printf("%s %s\n", ok ? "ok  " : "FAIL", name); if (!ok) t_bad++; }

#define T0 8
#define T1 9
#define T2 10
#define T3 11
#define LUI(rt, imm) ((u32)((15u << 26) | ((rt) << 16) | ((imm) & 0xFFFF)))
#define ORI(rs, rt, imm) ((u32)((13u << 26) | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFF)))
#define LW(rt, off, base) ((u32)((35u << 26) | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFF)))
#define LLD(rt, off, base) ((u32)((52u << 26) | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFF)))
#define LL(rt, off, base) ((u32)((48u << 26) | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFF)))
#define SC(rt, off, base) ((u32)((56u << 26) | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFF)))
#define SRA(rd, rt, sa) ((u32)(((rt) << 16) | ((rd) << 11) | ((sa) << 6) | 0x03))
#define SRAV(rd, rt, rs) ((u32)(((rs) << 21) | ((rt) << 16) | ((rd) << 11) | 0x07))
#define ERET 0x42000018u

#define CODE 0x80001000u
static void setup(const u32 *ops, int nops) {
  memset(&sys, 0, sizeof sys);
  sys.rdram_size = 4 << 20;
  memset(rdram, 0, sys.rdram_size);
  cpu_reset();
  for (int i = 0; i < nops; i++) RDRAM32(0x1000 + i * 4) = ops[i];
  cpu.pc = CODE; cpu.npc = CODE + 4;  // (pc/npc advance after fetch)
  cpu.next_ev = cpu.cycles + 4 * 64;  // the test code, then NOP tail
}

int main(void) {
  // --- SRA with a dirty upper half: only the low 32 bits shift ---
  // bit31=0 but bits[35:32]=0xF: a 64-bit shift leaks 0xF into the result
  u32 sra[] = { LUI(T2, 0x8001), LLD(T0, 0, T2), SRA(T1, T0, 4) };
  setup(sra, 3);
  RDRAM32(0x10000) = 0x0000000Fu; RDRAM32(0x10004) = 0x40000000u;  // (setup zeroes rdram)
  cpu_run();
  t_check("SRA ignores upper half", cpu.r[T1] == (s64)(s32)0x04000000u);

  u32 srav[] = { LUI(T2, 0x8001), ORI(0, T3, 4), LLD(T0, 0, T2), SRAV(T1, T0, T3) };
  setup(srav, 4);
  RDRAM32(0x10000) = 0x0000000Fu; RDRAM32(0x10004) = 0x40000000u;
  cpu_run();
  t_check("SRAV ignores upper half", cpu.r[T1] == (s64)(s32)0x04000000u);

  u32 sra_neg[] = { LUI(T2, 0x8001), LLD(T0, 0, T2), SRA(T1, T0, 4) };
  setup(sra_neg, 3);
  RDRAM32(0x10000) = 0xFFFFFFFFu; RDRAM32(0x10004) = 0x80000000u;
  cpu_run();
  t_check("SRA sign-extends negatives", cpu.r[T1] == (s64)(s32)0xF8000000u);

  // --- LL, faulting LW (KUSEG va=0: TLB miss), link must die ---
  u32 fault[] = { LUI(T2, 0x8001), LL(T0, 0, T2), LW(T1, 0, 0) };
  setup(fault, 3);
  RDRAM32(0x10000) = 0x12345678u;
  cpu_run();
  t_check("fault raises TLB exception at the LW", (u32)cpu.cp0[C0_EPC] == CODE + 8 && (cpu.cp0[C0_STATUS] & ST_EXL) != 0);
  t_check("exception clears llbit", cpu.llbit == 0);

  // --- LL/SC success path still works (SC's rt is both source and status) ---
  u32 sc_ok[] = { LUI(T2, 0x8001), ORI(0, T1, 0xBE), SC(T1, 0, T2) };
  setup(sc_ok, 3);
  cpu.llbit = 1;  // SC honors a live link...
  cpu_run();
  t_check("SC succeeds with live link", cpu.r[T1] == 1 && RDRAM32(0x10000) == 0xBEu);

  u32 sc_ll[] = { LUI(T2, 0x8001), LL(T0, 0, T2), ORI(0, T1, 0xEF), SC(T1, 0, T2) };
  setup(sc_ll, 4);
  RDRAM32(0x10000) = 0x11111111u;
  cpu_run();
  t_check("LL..SC sequence stores", cpu.r[T1] == 1 && RDRAM32(0x10000) == 0xEFu);

  // --- ERET keeps clearing llbit ---
  u32 eret[] = { ERET };
  setup(eret, 1);
  cpu.llbit = 1;
  cpu.cp0[C0_EPC] = CODE + 4;
  cpu_run();
  t_check("ERET clears llbit", cpu.llbit == 0);

  printf("%d checks, %d failures\n", t_n, t_bad);
  return t_bad != 0;
}
