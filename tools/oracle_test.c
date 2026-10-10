// Actual guest writes must update the alternate image without erasing untouched
// renderer differences. Requires the same pinned reference as the oracle build.
#define main oracle_entry
#include "oracle.c"
#undef main
static unsigned checks, failed;
static void check(int ok) { checks++; if (!ok) { printf("FAIL oracle check %u\n", checks); failed++; } }
void photon_reference_coords(int s, int t, int w, int *os, int *ot, int *overflow);
unsigned photon_reference_hidden(unsigned pa);
int main(void) {
  // Every positive W plus zero/negative W, both axes, signed extrema and
  // deterministic arbitrary inputs. Expected samples come from the separate
  // pinned reference implementation, including its saturation flags.
  u32 rng = 0x31415926;
  for (int w = -32768; w <= 32767; w++) for (int k = 0; k < 3; k++) {
    rng = rng * 1664525u + 1013904223u;
    int s = k == 0 ? -32768 : k == 1 ? 32767 : (s16)rng;
    int t = k == 0 ? 32767 : k == 1 ? -32768 : (s16)(rng >> 16);
    int os, ot, rs, rt, overflow;
    int mine_overflow = perspective_divide(s, t, w, &os, &ot);
    photon_reference_coords(s, t, w, &rs, &rt, &overflow);
    check(texture_coord(os) == rs); check(texture_coord(ot) == rt);
    check(mine_overflow == overflow);
  }
  printf("reference texture coordinates: %u checks, %u failed\n", checks, failed);
  sys.rdram_size = RDRAM_MAX; cpu_reset(); rdp_reset(); reference_init(); need_resync = 0;
  // A queued fill must land before a masked CPU write. The untouched red
  // halfword keeps its RDP coverage, while the written half becomes CPU-owned.
  static const u32 fill[][2] = {
    {0x3F100001, 0x8000}, {0x2D000000, (8u << 12) | 4},
    {0x2F300000, 0}, {0x37000000, 0xF801F801}, {0x36004000, 0}
  };
  for (u32 i = 0; i < sizeof fill / sizeof fill[0]; i++) { hook_command(fill[i], 2); rdp_exec(fill[i]); }
  check(b_info.num_prims == 1);
  check(*(u32 *)(alt_rdram + 0x8000) == 0);
  bus_write32(0x8000, 0x1122, 0xFFFF);
  check(*(u32 *)(alt_rdram + 0x8000) == 0xF8011122);
  check(RDRAM32(0x8000) == 0xF8011122);
  check(photon_reference_hidden(0x8000) == 3);
  check(photon_reference_hidden(0x8002) == 0);
  RDRAM32(0x100) = 0x11223344;
  *(u32 *)(alt_rdram + 0x100) = 0x99AABBCC;
  bus_write32(0x100, 0x11003300, 0xFF00FF00);
  check(RDRAM32(0x100) == 0x11223344);
  check(*(u32 *)(alt_rdram + 0x100) == 0x11AA33CC);

  check(!map_w[0x80000] && !map_w[0xA0000]);
  cpu_watch_page(0, 0); check(!map_w[0x80000]);
  map_range(0x4000, 0x1000, 0x3000, 1, 1); check(!map_w[4]);
  RDRAM32(0x1000) = (43u << 26) | (8u << 21) | (9u << 16);
  cpu.r[8] = 0x80000200; cpu.r[9] = 0xCAFEBABE;
  cpu.pc = 0x80001000; cpu.npc = cpu.pc + 4; cpu.next_ev = cpu.cycles + cpu.cpi;
  cpu_run(); check(RDRAM32(0x200) == 0xCAFEBABE);
  check(*(u32 *)(alt_rdram + 0x200) == 0xCAFEBABE);

  static u8 cart[128];
  for (u32 i = 0; i < sizeof cart; i++) cart[i] = (u8)(i + 1);
  sys.rom = cart; sys.rom_size = sizeof cart;
  sys.pi[0] = 0x2000; sys.pi[1] = 0x10000000; sys.pi[3] = 15;
  pi_dma_write(); check(!memcmp(rdram + 0x2000, alt_rdram + 0x2000, 16));
  memset(rdram + 0x2100, 0xAA, 32); memset(alt_rdram + 0x2100, 0xBB, 32);
  sys.pi[0] = 0x2102; sys.pi[1] = 0x10000000; sys.pi[3] = 15;
  pi_dma_write();
  for (u32 a = 0x2102; a < 0x2110; a++) check(rdram[a ^ 3] == alt_rdram[a ^ 3]);
  check(alt_rdram[0x2100 ^ 3] == 0xBB && alt_rdram[0x2110 ^ 3] == 0xBB);

  *(u32 *)spmem = 0x12345678; *(u32 *)(spmem + 4) = 0x90ABCDEF;
  sp_pend.mem = 0; sp_pend.dram = 0x3000; sp_dma(7, 0);
  check(!memcmp(rdram + 0x3000, alt_rdram + 0x3000, 8));

  sys.si_dram = RDRAM_MAX - 32;
  memset(sys.pif, 0xFF, sizeof sys.pif); sys.pif[63] = 0;
  bus_write32(0x04800004, 0, ~0u);
  for (u32 i = 0; i < 64; i++) { u32 a = (sys.si_dram + i) & (RDRAM_MAX - 1); check(rdram[a ^ 3] == alt_rdram[a ^ 3]); }

  // A frame boundary will resync from the master before the next command;
  // guest writes must not trigger premature drawing during this interval.
  need_resync = 1; *(u32 *)(alt_rdram + 0x4000) = 0x87654321;
  bus_write32(0x4000, 0xAABBCCDD, ~0u);
  check(*(u32 *)(alt_rdram + 0x4000) == 0x87654321);
  check(photon_reference_hidden(0x4000) == 3 && photon_reference_hidden(0x4002) == 3);
  printf("oracle coordinates/writes: %u checks, %u failed\n", checks, failed);
  return failed != 0;
}
