// Independently compare VI scanout of CPU-drawn fixtures, with the same guest
// registers and field sequence. The shipped PNGs are raw framebuffer fixtures.
#define main oracle_entry
#include "oracle.c"
#undef main
int main(int argc, char **argv) {
  if (argc != 3 || atoi(argv[2]) <= 0) return 1;
  FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
  fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
  u8 *rom = malloc(size + 16); if (!rom || fread(rom, 1, size, f) != (size_t)size) return 1; fclose(f);
  if (!n64_load_rom(rom, size)) return 1;
  reference_init();
  unsigned long long pixels = 0, bad = 0; unsigned fields = 0;
  for (int i = 0; i < atoi(argv[2]); i++) {
    n64_run_frame(); vi_render();
    memcpy(a_vi, sys.vi, sizeof a_vi); a_vi[VI_V_CURRENT_LINE] = sys.vi_field & 1;
    struct n64video_frame_buffer ref = {0}; n64video_update_screen(&ref);
    if (!ref.valid) {
      if (!vi_blank) { printf("FAIL VI validity field %d: native active, reference invalid\n", i); return 1; }
      continue;
    }
    if (ref.width != vi_out_w || ref.height != vi_out_h) {
      printf("FAIL VI dimensions field %d: %ux%u vs %ux%u\n", i, vi_out_w, vi_out_h, ref.width, ref.height); return 1;
    }
    fields++;
    for (u32 y = 0; y < vi_out_h; y++) for (u32 x = 0; x < vi_out_w; x++) {
      u32 p = vi_out[y * vi_out_w + x]; struct n64video_pixel r = ref.pixels[y * ref.pitch + x];
      if ((p & 255) != r.r || ((p >> 8) & 255) != r.g || ((p >> 16) & 255) != r.b) bad++;
      pixels++;
    }
  }
  printf("%s VI %s: %u fields, %llu RGB pixels, %llu differences (registers/field order exact; reference random bits zero)\n",
         fields && !bad ? "PASS" : "FAIL", argv[1], fields, pixels, bad);
  n64video_close(); free(rom); return !fields || bad;
}
