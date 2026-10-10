// Native headless harness: runs a ROM for N frames, dumps frames/audio, scripted input.
//   native rom frames [-o prefix] [-e every] [-i "frame:BTN[+BTN]:dur,frame:X=80:dur,..."] [-raw] [-wav file]
#include "../src/n64.c"
#include "png.h"
#include <time.h>

void host_log(const char *msg, u32 a, u32 b) { printf("[log] %s %x %x\n", msg, a, b); }
void host_gpu_flush(void) {}

static u8 fb_rgba[640 * 576 * 4];
static int dump_raw_fb(const char *path) {
  u32 origin = sys.vi[1] & 0xFFFFFC, width = sys.vi[2] & 0xFFF, type = sys.vi[0] & 3;
  if (!width || (type != 2 && type != 3)) return 0;
  u32 dst_width = width < 640 ? width : 640;
  u32 h = width >= 640 ? 480 : 240;
  for (u32 y = 0; y < h; y++) for (u32 x = 0; x < dst_width; x++) {
    u8 *o = fb_rgba + (y * dst_width + x) * 4;
    if (type == 3) { u32 p = RDRAM32((origin + (y * width + x) * 4) & (RDRAM_MAX - 1)); o[0] = p >> 24; o[1] = p >> 16; o[2] = p >> 8; }
    else { u32 a = (origin + (y * width + x) * 2) & (RDRAM_MAX - 1); u16 p = *(u16 *)(rdram + (a ^ 2)); o[0] = ((p >> 11) & 31) << 3; o[1] = ((p >> 6) & 31) << 3; o[2] = ((p >> 1) & 31) << 3; }
    o[3] = 255;
  }
  return png_write(path, fb_rgba, dst_width, h);
}
static void dump_vi(const char *path) {
  // present at 640x480: line-double progressive output
  u32 h = vi_out_h, oh = vi_state.serrate ? h : h * 2;
  static u8 tmp[640 * 576 * 4];
  for (u32 y = 0; y < oh; y++) memcpy(tmp + y * 640 * 4, (u8 *)vi_out + (vi_state.serrate ? y : y / 2) * 640 * 4, 640 * 4);
  png_write(path, tmp, 640, oh);
}

typedef struct { int frame, dur; u32 buttons; int has_x, has_y, x, y; } InputEv;
static InputEv evs[4096]; static int nev;
static u32 btn_mask(const char *s, int n) {
  static const struct { const char *n; u32 m; } tab[] = { {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000}, {"DU", 0x800}, {"DD", 0x400}, {"DL", 0x200}, {"DR", 0x100},
    {"L", 0x20}, {"R", 0x10}, {"CU", 8}, {"CD", 4}, {"CL", 2}, {"CR", 1} };
  for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) if ((int)strlen(tab[i].n) == n && !strncmp(tab[i].n, s, n)) return tab[i].m;
  return 0;
}
static void parse_inputs(const char *s) {
  while (*s && nev < 4096) {
    InputEv *e = &evs[nev]; memset(e, 0, sizeof *e);
    e->frame = atoi(s); while (*s && *s != ':') s++; if (*s) s++;
    const char *b = s; while (*s && *s != ':' && *s != ',') s++;
    const char *p = b;
    while (p < s) {
      const char *q = p; while (q < s && *q != '+') q++;
      if (p[0] == 'X' && p[1] == '=') { e->has_x = 1; e->x = atoi(p + 2); }
      else if (p[0] == 'Y' && p[1] == '=') { e->has_y = 1; e->y = atoi(p + 2); }
      else e->buttons |= btn_mask(p, (int)(q - p));
      p = q < s ? q + 1 : q;
    }
    e->dur = 1;
    if (*s == ':') { s++; e->dur = atoi(s); while (*s && *s != ',') s++; }
    if (*s == ',') s++;
    nev++;
  }
}
static void apply_inputs(int frame) {
  u32 b = 0; int x = 0, y = 0;
  for (int i = 0; i < nev; i++) if (frame >= evs[i].frame && frame < evs[i].frame + evs[i].dur) { b |= evs[i].buttons; if (evs[i].has_x) x = evs[i].x; if (evs[i].has_y) y = evs[i].y; }
  sys.buttons[0] = b; sys.stick_x[0] = x; sys.stick_y[0] = y;
}

#ifdef RDP_DUMP
int rdp_dump_on;
#endif
#ifndef NATIVE_NO_MAIN
static u32 stall_lastp; static int stall_lastf;
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: native rom frames [-o prefix] [-e every] [-i inputs] [-raw] [-wav file] [-s start]\n"); return 1; }
  const char *prefix = 0, *wav = 0; int every = 60, raw = 0, start = 0;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "-o")) prefix = argv[++i];
    else if (!strcmp(argv[i], "-e")) every = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-i")) parse_inputs(argv[++i]);
    else if (!strcmp(argv[i], "-raw")) raw = 1;
    else if (!strcmp(argv[i], "-wav")) wav = argv[++i];
    else if (!strcmp(argv[i], "-s")) start = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-pak")) sys.pak[0] = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-xpak")) xpak_mode = atoi(argv[++i]);     // Expansion Pak: 0 auto, 1 installed, 2 removed
  }
  FILE *f = fopen(argv[1], "rb"); if (!f) { perror("rom"); return 1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  u8 *rom = malloc(sz + 16); if (fread(rom, 1, sz, f) != (size_t)sz) return 1; fclose(f);
  if (!n64_load_rom(rom, sz)) { fprintf(stderr, "bad rom\n"); return 1; }
  int frames = atoi(argv[2]);
  FILE *wf = wav ? fopen(wav, "wb") : 0; u32 wav_rd = 0, wav_n = 0;
  if (wf) fseek(wf, 44, SEEK_SET);
  clock_t t0 = clock();
  for (int i = 0; i < frames; i++) {
    apply_inputs(i);
#ifdef DP_TRACE
    { extern int dp_trace_on; dp_trace_on = getenv("DPT") && i >= atoi(getenv("DPT")) && i < atoi(getenv("DPT")) + 3; }
#endif
#ifdef RDP_DEBUG
    { extern int rdp_dbg_on, rdp_dbg_x, rdp_dbg_y; rdp_dbg_on = getenv("DBGF") && i >= atoi(getenv("DBGF")) && i < atoi(getenv("DBGF")) + 2; if (getenv("DBGX")) { rdp_dbg_x = atoi(getenv("DBGX")); rdp_dbg_y = atoi(getenv("DBGY")); } }
#endif
#ifdef RDP_DUMP
    { extern int rdp_dump_on; rdp_dump_on = getenv("DUMPF") && i >= atoi(getenv("DUMPF")) && i < atoi(getenv("DUMPF")) + (getenv("DUMPN") ? atoi(getenv("DUMPN")) : 1); if (rdp_dump_on) printf("---- frame %d\n", i); }
#endif
    n64_run_frame();
    if (rdp_stat_prims != stall_lastp) { stall_lastp = rdp_stat_prims; stall_lastf = i; }
    if (wf) { while (wav_rd != audio_wr) { fwrite(&audio_buf[(wav_rd & (AUDIO_RING - 1)) * 2], 2, 2, wf); wav_rd++; wav_n++; } }
    if (prefix && !raw && i + 4 >= start) vi_render();
    if (prefix && i >= start && (i % every) == every - 1) { char path[256]; snprintf(path, sizeof path, "%s%05d.png", prefix, i + 1); if (raw) { if (!dump_raw_fb(path)) { fprintf(stderr, "raw output failed\n"); return 1; } } else dump_vi(path); }
  }
  double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
  if (wf) {
    u32 hdr[11] = { 0x46464952, 36 + wav_n * 4, 0x45564157, 0x20746D66, 16, 0x00020001, audio_freq, audio_freq * 4, 0x00100004, 0x61746164, wav_n * 4 };
    fseek(wf, 0, SEEK_SET); fwrite(hdr, 4, 11, wf); fclose(wf);
  }
  { u32 h = 2166136261u; for (u32 k = 0; k < RDRAM_MAX; k += 4) h = (h ^ *(u32 *)(rdram + k)) * 16777619u; u32 ha = 2166136261u; extern s16 audio_buf[]; for (u32 k = 0; k < 65536; k++) ha = (ha ^ (u16)audio_buf[k]) * 16777619u; printf("hash rdram=%08x audio=%08x\n", h, ha); }
  printf("frames=%d time=%.3fs (%.1f fps) pc=%08x idle=%.1f%% rsp_instr=%llu prims=%u flushes=%u\n", frames, dt, frames / dt, cpu.pc,
         100.0 * cpu.idle_skipped / (cpu.cycles ? cpu.cycles : 1), (unsigned long long)rsp.icount, rdp_stat_prims, rdp_stat_flushes);
  printf("last primitive drawn in frame %d; dp start=%x end=%x cur=%x status=%x sp=%x mi=%x/%x\n", stall_lastf, sys.dp_start, sys.dp_end, sys.dp_current, sys.dp_status, sys.sp_status, sys.mi_intr, sys.mi_mask);
  printf("vi: ctrl=%x origin=%x width=%x h=%x v=%x xs=%x ys=%x audio=%u@%u\n", sys.vi[0], sys.vi[1], sys.vi[2], sys.vi[9], sys.vi[10], sys.vi[12], sys.vi[13], audio_wr, audio_freq);
  return 0;
}
#endif
