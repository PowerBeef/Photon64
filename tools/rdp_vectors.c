// Emit only authored synthetic batches and software results for the GPU lane.
#include "../src/n64.c"
#include <stdio.h>
#include "rdp_vectors.h"
void host_log(const char *s, u32 a, u32 b) { (void)s; (void)a; (void)b; }
static unsigned batches;
static void array(const char *name, const u32 *p, u32 n) {
  printf("\"%s\":[", name);
  for (u32 i = 0; i < n; i++) printf("%s%u", i ? "," : "", p[i]);
  printf("],");
}
void host_gpu_flush(void) {
  if (gpu_hint) return;
  printf("%s{", batches++ ? "," : "");
  array("info", (u32*)&b_info, sizeof b_info / 4);
  array("prims", (u32*)b_prims, b_info.num_prims * PRIM_WORDS);
  array("spans", b_spans, (b_info.num_spans + 1) * SPAN_WORDS);
  array("states", b_states, b_info.num_states * STATE_WORDS);
  array("tiles", b_tiles, b_info.num_tilesets * 8 * TILE_WORDS);
  array("tmem", b_tmem, b_info.num_tmem * 1024);
  array("bins", b_bins, b_info.bins_words);
  sw_render_batch();
  u32 expected[64];
  for (u32 i = 0; i < 64; i++) { u32 k = (b_info.fb_addr * (b_info.fb_fmt == FB_8888 ? 2 : 1) + i) & M16; expected[i] = VRAM16(k) | (hidden_get(k) << 16); }
  array("expected", expected, 64);
  for (u32 i = 0; i < 64; i++) { u32 k = (b_info.depth_addr + i) & M16; expected[i] = VRAM16(k) | (hidden_get(k) << 16); }
  array("depth", expected, 64);
  printf("\"feedback\":["); for (int i = 0; i < 12; i++) printf("%s%d", i ? "," : "", pipeline_feedback[i]); puts("]}");
}
int main(void) {
  sys.rdram_size = RDRAM_MAX; cpu_reset(); rdp_reset(); rdp_gpu_mode = 1;
  puts("[");
  for (u32 i = 0; i < sizeof renderer_vectors / sizeof *renderer_vectors; i++) rdp_exec(renderer_vectors[i]);
  for (u32 i = 0; i < sizeof boundary_vectors / sizeof *boundary_vectors; i++) rdp_exec(boundary_vectors[i]);
  rdp_flush(); puts("]"); return !batches;
}
