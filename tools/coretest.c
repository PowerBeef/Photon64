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
#include "fpu_vectors.h"
#include "rdp_vectors.h"
#include "cic_vectors.h"
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
  // Original merge-load fault addresses, including delay slots and invalid TLBs.
  const u32 merge_ops[] = {26, 27, 34, 38};
  for (u32 k = 0; k < 4; k++) for (u32 off = 0; off < (k < 2 ? 8u : 4u); off++) for (u32 invalid = 0; invalid < 2; invalid++) for (u32 delay = 0; delay < 2; delay++) {
    setup((merge_ops[k] << 26) | (8u << 21) | (9u << 16) | off); cpu.r[8] = 0x4000; cpu.branch = delay;
    if (invalid) { cpu.tlb[0].hi = 0x4000; cpu.tlb[0].lo0 = 0; }
    cpu_run(); check((u32)cpu.cp0[C0_BADVADDR] == 0x4000 + off);
    check(((cpu.cp0[C0_CAUSE] >> 2) & 31) == EXC_TLBL);
    check((u32)cpu.cp0[C0_EPC] == (delay ? 0x80000FFCu : 0x80001000u)); check(!!(cpu.cp0[C0_CAUSE] & 0x80000000u) == delay);
  }
  // Execute through a permitted user TLB page, then access forbidden segments.
  for (u32 mode = 1; mode <= 2; mode++) for (u32 op = 35; op <= 43; op += 8) for (u32 segment = 0; segment < 3; segment++) {
    setup((op << 26) | (8u << 21) | (9u << 16));
    cpu.tlb[0] = (TLBEntry){ .hi = 0x4000, .lo0 = (1u << 6) | 7, .lo1 = 7, .g = 1 }; tlb_remap_all();
    cpu.pc = 0x4000; cpu.npc = 0x4004; cpu.r[8] = (u32[]){0x80002000u,0xA0002000u,0xE0002000u}[segment]; cpu.cp0[C0_STATUS] = mode << 3;
    cpu_run(); check(((cpu.cp0[C0_CAUSE] >> 2) & 31) == (op == 35 ? EXC_ADEL : EXC_ADES)); check((u32)cpu.cp0[C0_BADVADDR] == (u32)cpu.r[8]);
  }
  setup(0); cpu.cp0[C0_STATUS] = 16; cpu_run(); check(((cpu.cp0[C0_CAUSE] >> 2) & 31) == EXC_ADEL); check((u32)cpu.cp0[C0_BADVADDR] == 0x80001000u);
  setup(0); gpu_exact = 1; gpu_watch_region(RDRAM_MAX / 2 - 2, 4); gpu_mark_stale(RDRAM_MAX / 2 - 2, 4);
  check(gpu_watch[0] && gpu_watch[(RDRAM_MAX >> 12) - 1]); check(gpu_range_stale(0, 4)); check(gpu_range_stale(RDRAM_MAX - 4, 8)); check(!gpu_range_stale(16, 4));
  gpu_mark_dirty(RDRAM_MAX - 3, 6); check((rdp_hidden[0] & 128) && (rdp_hidden[1] & 128) && (rdp_hidden[RDRAM_MAX / 2 - 1] & 128)); gpu_unwatch(1);
  setup(0); sys.save_dirty = 7; eeprom[0] = 0xA5; sys_reset(); check(sys.save_dirty == 7 && eeprom[0] == 0xA5);
  // Exercise high bytes through real ROM and cartridge/PIF/RSP packing.
  static u8 small_rom[4096]; memset(small_rom, 0, sizeof small_rom); small_rom[0] = 0x80; small_rom[1] = 0x37; small_rom[2] = 0x12; small_rom[3] = 0x40;
  check(n64_load_rom(small_rom, sizeof small_rom)); sys.save_type = SAVE_SRAM; savemem[0] = 0x80; check(cart_read32(0x08000000) == 0x80000000u);
  sys.pif[0] = 0x80; check(bus_read32(0x1FC007C0) == 0x80000000u);
  memset(DMEM, 0, 16); dm_w8(1, 0x80); check(dm_r32(1) == 0x80000000u);
  for (u32 i = 0; i < sizeof cic_vectors / sizeof *cic_vectors; i++) {
    memset(sys.pif, 0, 64); sys.cic = 6105; memcpy(sys.pif + 0x30, cic_vectors[i].input, 15); sys.pif[63] = 2; sys.pif[0x2E] = sys.pif[0x2F] = 0xFF;
    pif_control(); check(memcmp(sys.pif + 0x30, cic_vectors[i].response, 15) == 0); check(sys.pif[63] == 0 && sys.pif[0x2E] == 0 && sys.pif[0x2F] == 0);
    pif_process(); check(memcmp(sys.pif + 0x30, cic_vectors[i].response, 15) == 0);
    sys.cic = 6102; memcpy(sys.pif + 0x30, cic_vectors[i].input, 15); sys.pif[63] = 2; pif_control();
    for (u32 k = 0; k < 15; k++) check(sys.pif[0x30+k] == (u8)~cic_vectors[i].input[k]);
    sys.pif[63] = 0; pif_control(); check(!sys.pif_challenge);
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
  // Exact-rational corpus exercises all four rounding modes, flags and traps.
  for (u32 i = 0; i < sizeof fpu_vectors / sizeof fpu_vectors[0]; i++) {
    memset(&cpu, 0, sizeof cpu); cpu.fmask = 31;
    cpu.cp0[C0_STATUS] = ST_CU1 | ST_FR; cpu.pc = 0x80001004; cpu.npc = cpu.pc + 4;
    cpu.f[2].u = fpu_vectors[i].a; cpu.f[1].u = fpu_vectors[i].b;
    cpu.f[3].u = 0x123456789ABCDEF0ull; cpu.fcr31 = fpu_vectors[i].control;
    cop1_exec((17u << 26) | (fpu_vectors[i].fmt << 21) | (1u << 16) | (2u << 11) | (3u << 6) | fpu_vectors[i].fn);
#ifndef __wasm__
    if ((cpu.f[3].u != fpu_vectors[i].result || cpu.fcr31 != fpu_vectors[i].fcr) && failures < 10)
      printf("FPU vector %u: result=%llx expected=%llx fcr=%x expected=%x\n", i, (unsigned long long)cpu.f[3].u, (unsigned long long)fpu_vectors[i].result, cpu.fcr31, fpu_vectors[i].fcr);
#endif
    check(cpu.f[3].u == fpu_vectors[i].result);
    check(cpu.fcr31 == fpu_vectors[i].fcr);
    check(!!(cpu.cp0[C0_STATUS] & ST_EXL) == fpu_vectors[i].trap);
  }
  memset(&cpu, 0, sizeof cpu); memset(&rsp, 0, sizeof rsp);
  sys.sp_status = 0; cpu.cycles = rsp.sync = 100; cpu.ev[EV_RSP] = EV_NEVER;
  gpu_sync_request = 0; check(rsp_sync() == 0); check(cpu.ev[EV_RSP] == 100 + SLICE);
  sys.sp_status = SP_HALT;
  check(ram_ranges_overlap(RDRAM_MAX - 4, 8, 0, 4));
  check(ram_ranges_overlap(0, 4, RDRAM_MAX - 4, 8));
  check(!ram_ranges_overlap(16, 8, 24, 8));
  check(ram_ranges_overlap(16, 8, 23, 8));
  check(ram_ranges_overlap(0, RDRAM_MAX, 32, 1));
  check(!ram_ranges_overlap(0, 0, 0, 4));
  check(normalize_dzpix(1) == 3);
  check(shift_coord(0x10000, 0, 0) == 0);
  check(shift_coord(0xFFFF, 0, 0) == -1);
  check(shift_coord(0x8000, 0, 0) == -32768);
  check(shift_coord(0x8000, 0, 15) == 0);
  // These authored RDP vectors run under ASan/UBSan and both WASM optimizer
  // levels as well as the independent oracle and actual WGSL vector runner.
  memset(rdram, 0, RDRAM_MAX); rdp_reset(); rdp_gpu_mode = 0;
  for (u32 i = 0; i < sizeof renderer_vectors / sizeof *renderer_vectors; i++) rdp_exec(renderer_vectors[i]);
  rdp_flush();
  check(VRAM16(0x8000 / 2 + 8) == 0xF801); // logical X=4 aliases next row
  check(pipeline_combined[3] == 255);
  check(b_info.num_prims == 0);
  // A software fallback can change hidden bits while repeating the same data.
  // Its upload must preserve RDP hidden bits rather than apply the CPU LSB rule.
  rdp_gpu_mode = 1; b_info.fb_fmt = FB_I8;
  VRAM16(0x40000) = gpu_shadow16[0x40000] = 0;
  hidden_set(0x40000, 3); check(hidden_get(0x40000) == 3);
  check(n64_sync_scan(0x40000, 1) == 1);
  check(sync_stage[0] == (0x80000000u | (3u << 16)));
  rdp_gpu_mode = 0;
  s32 ps, pt;
  check(!perspective_divide(-11048, -174, 7016, &ps, &pt));
  check(ps == -51599 && pt == -813); // preserve 17-bit coordinates for LOD
  check(texture_coord(ps) == -32768 && texture_coord(pt) == -813);
  check(texture_coord(32768) == 32767);
  check(texture_coord(-32769) == -32768);
  check(texture_coord(32767) == 32767 && texture_coord(-32768) == -32768);
  TriSetup edge = { .dxhdy = 67108863, .dxmdy = 67108863, .dxldy = 67108863, .yl = 1024, .ym = 32 };
  u32 out[8]; span_setup(&edge, 10, out); check(1); // UBSan guards widened edge intermediates.
  return failures;
}
EXPORT(test_count) u32 test_count(void) { return checks; }
EXPORT(test_first_failure) u32 test_first_failure(void) { return first_failure; }
#ifndef __wasm__
int main(void) { run_tests(); printf("%u checks, %u failures (first %u)\n", checks, failures, first_failure); return failures != 0; }
#endif
