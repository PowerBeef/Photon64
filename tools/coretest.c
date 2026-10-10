// Audit semantic vectors, shared verbatim between native sanitizer and WASM runs.
#include "../src/n64.c"
#ifndef __wasm__
#include <stdio.h>
#endif
void host_log(const char *s, u32 a, u32 b) { (void)s; (void)a; (void)b; }
void host_gpu_flush(void) {}
static u32 checks, failures, first_failure;
static void check(int ok) { checks++; if (!ok) { failures++; if (!first_failure) first_failure = checks; } }
static void setup(u32 op) {
  memset(&sys, 0, sizeof sys); memset(rdram, 0, RDRAM_MAX);
  sys.rdram_size = RDRAM_MAX; cpu_reset();
  gpu_unwatch(1); cpu_restart = cpu_resume_one = 0;
  RDRAM32(0x1000) = op; cpu.pc = 0x80001000; cpu.npc = cpu.pc + 4;
  cpu.next_ev = cpu.cycles + cpu.cpi;
}
#define OP(fn) ((8u << 21) | (9u << 16) | (10u << 11) | (fn))
EXPORT(run_tests) u32 run_tests(void) {
  checks = failures = first_failure = 0;
  setup(OP(0x2D)); cpu.r[8] = 0x7FFFFFFFFFFFFFFFll; cpu.r[9] = 1; cpu_run(); check((u64)cpu.r[10] == 0x8000000000000000ull);
  setup(OP(0x2F)); cpu.r[8] = (s64)0x8000000000000000ull; cpu.r[9] = 1; cpu_run(); check((u64)cpu.r[10] == 0x7FFFFFFFFFFFFFFFull);
  setup(OP(0x38) | (1u << 6)); cpu.r[9] = -1; cpu_run(); check(cpu.r[10] == -2);
  setup(OP(0x3C) | (1u << 6)); cpu.r[9] = -1; cpu_run(); check((u64)cpu.r[10] == 0xFFFFFFFE00000000ull);
  setup(OP(0x14)); cpu.r[8] = 63; cpu.r[9] = -1; cpu_run(); check((u64)cpu.r[10] == 0x8000000000000000ull);
  setup((25u << 26) | (8u << 21) | (9u << 16) | 1); cpu.r[8] = 0x7FFFFFFFFFFFFFFFll; cpu_run(); check((u64)cpu.r[9] == 0x8000000000000000ull);
  setup((25u << 26) | (8u << 21) | (9u << 16) | 0xFFFF); cpu.r[8] = (s64)0x8000000000000000ull; cpu_run(); check((u64)cpu.r[9] == 0x7FFFFFFFFFFFFFFFull);
  setup(0x4501FFFF); cpu.fcr31 = 1u << 23; cpu.cp0[C0_STATUS] |= ST_CU1; cpu_run(); check(cpu.npc == 0x80001000);
  for (u32 op = 42; op <= 46; op++) {
    if (op == 43) continue;
    for (u32 kind = 0; kind < 3; kind++) {
      setup((op << 26) | (8u << 21) | (9u << 16) | 3); cpu.r[8] = 0x4000;
      if (kind) { cpu.tlb[0].hi = 0x4000; cpu.tlb[0].lo0 = kind == 2 ? 2 : 0; cpu.tlb[0].lo1 = 0; }
      cpu_run();
      check(((cpu.cp0[C0_CAUSE] >> 2) & 31) == (kind == 2 ? EXC_MOD : EXC_TLBS));
      check((u32)cpu.cp0[C0_BADVADDR] == 0x4003); check((u32)cpu.cp0[C0_EPC] == 0x80001000);
    }
    const u32 bytes = (op == 44 || op == 45) ? 8 : 4;
    const u64 value = bytes == 8 ? 0x1122334455667788ull : 0x11223344;
    for (u32 offset = 0; offset < bytes; offset++) {
      setup((op << 26) | (8u << 21) | (9u << 16) | offset);
      cpu.r[8] = 0x80002000; cpu.r[9] = (s64)value;
      memset(rdram + 0x2000, 0xAA, bytes); cpu_run();
      for (u32 i = 0; i < bytes; i++) {
        u8 want = 0xAA;
        if (op == 42 || op == 44) { if (i >= offset) want = value >> (8 * (bytes - 1 - (i - offset))); }
        else if (i <= offset) want = value >> (8 * (offset - i));
        check(rdram[0x2000 + (i ^ 3)] == want);
      }
      check(!(cpu.cp0[C0_STATUS] & ST_EXL));
    }
    // Delay-slot fault records branch EPC, BD and original unaligned VA.
    setup((op << 26) | (8u << 21) | (9u << 16) | 3); cpu.r[8] = 0x4000; cpu.branch = 1; cpu_run();
    check((cpu.cp0[C0_CAUSE] & 0x80000000u) != 0); check((u32)cpu.cp0[C0_EPC] == 0x80000FFC);
  }
  // Infinite inputs are not finite overflow, even with overflow enabled.
  for (u32 fmt = 16; fmt <= 17; fmt++) for (u32 fn = 0; fn <= 7; fn++) {
    if (fn == 6) continue;
    setup((17u << 26) | (fmt << 21) | (1u << 16) | (2u << 11) | (3u << 6) | fn);
    cpu.cp0[C0_STATUS] |= ST_CU1 | ST_FR;
    if (fmt == 16) { cpu.f[2].lo = 0x7F800000; cpu.f[1].f = 1.0f; }
    else { cpu.f[2].u = 0x7FF0000000000000ull; cpu.f[1].d = 1.0; }
    cpu.fcr31 = 1u << 9; cpu_run();
    check(cpu.fcr31 == (1u << 9)); check(!(cpu.cp0[C0_STATUS] & ST_EXL));
    check(fmt == 16 ? cpu.f[3].lo == (fn == 7 ? 0xFF800000u : 0x7F800000u) : cpu.f[3].u == (fn == 7 ? 0xFFF0000000000000ull : 0x7FF0000000000000ull));
  }
  TriSetup edge = { .dxhdy = 67108863, .dxmdy = 67108863, .dxldy = 67108863, .yl = 1024, .ym = 32 };
  u32 out[8]; span_setup(&edge, 10, out); check(1); // UBSan guards widened edge intermediates.
  return failures;
}
EXPORT(test_count) u32 test_count(void) { return checks; }
EXPORT(test_first_failure) u32 test_first_failure(void) { return first_failure; }
#ifndef __wasm__
int main(void) { run_tests(); printf("%u checks, %u failures (first %u)\n", checks, failures, first_failure); return failures != 0; }
#endif
