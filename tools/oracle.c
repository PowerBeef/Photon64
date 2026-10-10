// Differential test: run every RDP command list through both our renderer and Angrylion (reference
// software RDP, used here purely as a black-box oracle) and compare the framebuffers at each SYNC_FULL.
#define RDP_ORACLE
#define NATIVE_NO_MAIN
#include <stdint.h>
static uint8_t alt_rdram[0x800000 + 16] __attribute__((aligned(4096)));
#define RDP_FB_BASE alt_rdram
#include "native.c"
#include "../../ref/angrylion-rdp-plus/src/core/n64video.h"
#include <stdarg.h>

void msg_error(const char *err, ...) { va_list a; va_start(a, err); vfprintf(stderr, err, a); va_end(a); fputc('\n', stderr); }
void msg_warning(const char *err, ...) { (void)err; }
void msg_debug(const char *err, ...) { (void)err; }
void parallel_init(uint32_t num, bool busy) { (void)num; (void)busy; }
void parallel_run(void task(uint32_t)) { task(0); }
uint32_t parallel_num_workers(void) { return 1; }
void parallel_close(void) {}

#define ref_rdram rdram
#define mine alt_rdram
static u32 a_dp[8], a_vi[14], a_mi;
static u32 *a_dp_p[8], *a_vi_p[14];
static void a_irq(void) {}
static int frame_no, dump_diffs, verbose, dbg_frame = -1;
static unsigned long long tot_px, tot_bad, tot_zbad, tot_big; static int frames_big;
static u32 last_sync_count;
static const char *diff_prefix;

static u32 calls, need_resync = 1;
static u32 reference_commands[1024];
// Angrylion renders into the machine's real RDRAM (it is the master); our renderer reads the same
// commands/textures but renders into alt_rdram. Resync once after each SYNC_FULL.
static void hook_pre(void) {
  calls++;

}

// Consume one complete command at a time, so future reference writes cannot
// contaminate textures read earlier by Photon64. Both read the reference-master RDRAM.
static void hook_command(const u32 *words, u32 count) {
  if (need_resync) { memcpy(alt_rdram, rdram, RDRAM_MAX); need_resync = 0; }
  memcpy(reference_commands, words, count * sizeof *words);
  a_dp[DP_CURRENT] = a_dp[DP_START] = 0; a_dp[DP_END] = count * 4; a_dp[DP_STATUS] = 1;
  n64video_process_list();
}

static void compare(void) {
  u32 w = rdp.fb_width, h = rdp.sc_yhi >> 2, addr = rdp.fb_addr;
  if (h > 480) h = 480;
  u32 bad = 0, zbad = 0, n = w * h, nbig = 0;
  static u8 img[960 * 480 * 4];
  for (u32 i = 0; i < n; i++) {
    u32 x, y;
    if (rdp.fb_fmt == FB_8888) { x = *(u32 *)(mine + ((addr + i * 4) & (RDRAM_MAX - 4))); y = *(u32 *)(ref_rdram + ((addr + i * 4) & (RDRAM_MAX - 4))); }
    else { u32 o = ((addr + i * 2) & (RDRAM_MAX - 2)) ^ 2; x = *(u16 *)(mine + o); y = *(u16 *)(ref_rdram + o); }
        if (dump_diffs && w <= 320 && i < 320 * 480) {   // side by side: mine | ref | diff
      u32 px = i % w, py = i / w; u8 *p = img + (py * 960 + px) * 4, *q = p + 320 * 4, *d = q + 320 * 4;
      if (rdp.fb_fmt == FB_8888) { p[0] = x >> 24; p[1] = x >> 16; p[2] = x >> 8; q[0] = y >> 24; q[1] = y >> 16; q[2] = y >> 8; }
      else { p[0] = ((x >> 11) & 31) << 3; p[1] = ((x >> 6) & 31) << 3; p[2] = ((x >> 1) & 31) << 3; q[0] = ((y >> 11) & 31) << 3; q[1] = ((y >> 6) & 31) << 3; q[2] = ((y >> 1) & 31) << 3; }
      d[0] = x != y ? 255 : 0; d[1] = (x ^ y) & 1 ? 255 : 0; d[2] = 0; p[3] = q[3] = d[3] = 255;
      if (x != y && bad < 5) printf("   diff at (%u,%u): mine=%04x ref=%04x\n", px, py, x, y);
    }
    if (x != y) {
      bad++;
      // differences of one step per 5-bit channel are what two different random number sequences give with noise dithering
      int big = 0;
      if (rdp.fb_fmt == FB_8888) { for (int c = 0; c < 4; c++) { int d = (int)((x >> (c * 8)) & 0xFF) - (int)((y >> (c * 8)) & 0xFF); if (d > 8 || d < -8) big = 1; } }
      else { for (int c = 0; c < 3; c++) { int d = (int)((x >> (1 + c * 5)) & 31) - (int)((y >> (1 + c * 5)) & 31); if (d > 1 || d < -1) big = 1; } }
      if (big) { nbig++; if (verbose > 1 && nbig < 6) printf("   BIG diff at (%u,%u): mine=%04x ref=%04x\n", i % w, i / w, x, y); }
    }
    u32 zo = ((rdp.z_addr + i * 2) & (RDRAM_MAX - 2)) ^ 2;
    if (*(u16 *)(mine + zo) != *(u16 *)(ref_rdram + zo)) { if (zbad < 6 && verbose > 1) printf("   z diff at (%u,%u): mine=%04x ref=%04x\n", i % w, i / w, *(u16 *)(mine + zo), *(u16 *)(ref_rdram + zo)); zbad++; }
  }
  tot_px += n; tot_bad += bad; tot_zbad += zbad; tot_big += nbig; if (nbig) frames_big++;
  if (verbose == 1 || bad || zbad) printf("frame %d (vi %u): fb=%06x %ux%u fmt=%u  color mismatches=%u (%.3f%%, %u big)  z mismatches=%u\n", frame_no, sys.frames, addr, w, h, rdp.fb_fmt, bad, 100.0 * bad / (n ? n : 1), nbig, zbad);
  if (dump_diffs && bad && diff_prefix) { char path[256]; snprintf(path, sizeof path, "%s%05d.png", diff_prefix, frame_no); png_write(path, img, 960, h); }
  frame_no++;
#ifdef RDP_DEBUG
  rdp_dbg_on = frame_no == dbg_frame;
#ifdef REF_RDP_DEBUG
  { extern int ang_dbg_on, ang_dbg_x, ang_dbg_y, ang_dbg_pix; ang_dbg_on = rdp_dbg_on; ang_dbg_x = rdp_dbg_x; ang_dbg_y = rdp_dbg_y; ang_dbg_pix = rdp_dbg_y * (int)rdp.fb_width + rdp_dbg_x; }
#endif
#endif
  need_resync = 1;
}

static void hook_post(void) {
  if (rdp_cmd_count[0x29] != last_sync_count) { last_sync_count = rdp_cmd_count[0x29]; compare(); }
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: oracle rom frames [-i inputs] [-d diffprefix] [-v]\n"); return 1; }
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "-i")) parse_inputs(argv[++i]);
    else if (!strcmp(argv[i], "-d")) { dump_diffs = 1; diff_prefix = argv[++i]; }
    else if (!strcmp(argv[i], "-v")) verbose = 1;
    else if (!strcmp(argv[i], "-vv")) verbose = 2;
#ifdef RDP_DEBUG
    else if (!strcmp(argv[i], "-t")) { dbg_frame = atoi(argv[i + 1]); rdp_dbg_x = atoi(argv[i + 2]); rdp_dbg_y = atoi(argv[i + 3]); i += 3; }
#endif
  }
  FILE *f = fopen(argv[1], "rb"); if (!f) { perror("rom"); return 1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  u8 *rom = malloc(sz + 16); if (fread(rom, 1, sz, f) != (size_t)sz) return 1; fclose(f);
  if (!n64_load_rom(rom, sz)) { fprintf(stderr, "bad rom\n"); return 1; }

  struct n64video_config cfg;
  n64video_config_init(&cfg);
  for (int i = 0; i < 8; i++) a_dp_p[i] = &a_dp[i];
  for (int i = 0; i < 14; i++) a_vi_p[i] = &a_vi[i];
  cfg.gfx.rdram = rdram; cfg.gfx.rdram_size = RDRAM_MAX; cfg.gfx.dmem = (u8 *)reference_commands;
  cfg.gfx.vi_reg = a_vi_p; cfg.gfx.dp_reg = a_dp_p; cfg.gfx.mi_intr_reg = &a_mi; cfg.gfx.mi_intr_cb = a_irq;
  cfg.parallel = false; cfg.num_workers = 1;
  n64video_init(&cfg);
  rdp_hook_pre = hook_pre; rdp_hook_post = hook_post; rdp_hook_command = hook_command; rdp_hook_command_post = hook_post;
  int frames = atoi(argv[2]);
  for (int i = 0; i < frames; i++) { apply_inputs(i); n64_run_frame(); }
  printf("rdp_process calls: %u\n", calls);
  printf("colour differences larger than one dither step: %llu pixels in %d frames\n", tot_big, frames_big);
  printf("TOTAL: %d rdp frames, %llu pixels, %llu color mismatches (%.5f%%), %llu z mismatches (%.5f%%)\n", frame_no, tot_px, tot_bad,
         100.0 * tot_bad / (tot_px ? tot_px : 1), tot_zbad, 100.0 * tot_zbad / (tot_px ? tot_px : 1));
  return !frame_no || !tot_px || tot_bad || tot_zbad;
}
