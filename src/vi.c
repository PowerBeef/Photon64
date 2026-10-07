// Software VI: fetch -> AA / dither filter -> divot -> bilinear scale -> gamma.
// Produces a 640 x (240|288|480|576) RGBA image the way the real VI DAC would (following parallel-RDP's model).
#include "core.h"

u32 vi_out[640 * 576];
u32 vi_out_w = 640, vi_out_h = 240, vi_blank = 1;
static u32 vi_frame_count;
static u8 gamma_tab[256], gamma_dither_tab[16384];
static u32 vi_raw[(640 + 16) * (576 + 16)], vi_aa[(640 + 16) * (576 + 16)];
static u32 vi_noise;

// decoded registers, also consumed by the GPU presentation path
// Display option: bits of the VI control word to force off / on for the picture only (the game still reads back
// what it wrote). Used to switch off the console's edge blur and dither noise.
u32 vi_status_clear, vi_status_set;
typedef struct {
  u32 status, origin, width;
  s32 h_start, h_res, v_start, v_res, h_start_clamp, h_end_clamp;
  s32 x_start, x_add, y_start, y_add, max_x, max_y;
  u32 serrate, field, out_h, valid, frame;
} VIState;
VIState vi_state;

static u32 isqrt32(u32 v) { u32 r = 0, b = 1u << 30; while (b > v) b >>= 2; while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return r; }

static void vi_decode(void) {
  VIState *v = &vi_state;
  memset(v, 0, sizeof *v);
  v->status = (sys.vi[0] & ~vi_status_clear) | vi_status_set; v->origin = sys.vi[1] & 0xFFFFFF; v->width = sys.vi[2] & 0xFFF;
  v->field = sys.vi_field & 1;
  v->frame = vi_frame_count;
  s32 v_start = (sys.vi[10] >> 16) & 0x3FF, v_end = sys.vi[10] & 0x3FF;
  s32 y_start = (sys.vi[13] >> 16) & 0xFFF, y_add = sys.vi[13] & 0xFFF;
  int is_pal = (sys.vi[6] & 0x3FF) > 550;
  s32 v_end_max = is_pal ? ((44 + 576) | 1) : ((34 + 480) | 1);
  if (v_end > v_end_max) v_end = v_end_max;
  if (v_start > v_end_max) v_start = v_end_max;
  s32 v_off = is_pal ? 44 : 34;
  s32 v_res = (v_end - v_start) >> 1;
  v_start = (v_start - v_off) / 2;
  if (v_start < 0) { y_start -= y_add * v_start; v_start = 0; }
  s32 max_lines = (is_pal ? 576 : 480) >> 1;
  if (v_res > max_lines - v_start) v_res = max_lines - v_start;
  s32 x_start = (sys.vi[12] >> 16) & 0xFFF, x_add = sys.vi[12] & 0xFFF;
  s32 h_start = (sys.vi[9] >> 16) & 0x3FF, h_end = sys.vi[9] & 0x3FF;
  int left_clamp = 0, right_clamp = 0;
  h_start -= is_pal ? 128 : 108; h_end -= is_pal ? 128 : 108;
  if (h_start < 0) { x_start -= x_add * h_start; h_start = 0; left_clamp = 1; }
  if (h_end > 640) { h_end = 640; right_clamp = 1; }
  v->h_start = h_start; v->h_res = h_end - h_start;
  v->h_start_clamp = h_start + (left_clamp ? 0 : 8); v->h_end_clamp = h_end - (right_clamp ? 0 : 7);
  v->v_start = v_start; v->v_res = v_res;
  v->x_start = x_start; v->x_add = x_add; v->y_start = y_start; v->y_add = y_add;
  v->max_x = (x_start + v->h_res * x_add) >> 10;
  v->max_y = (y_start + v_res * y_add) >> 10;
  v->serrate = (v->status >> 6) & 1;
  v->out_h = (is_pal ? 576 : 480) >> (v->serrate ? 0 : 1);
  v->valid = v->origin != 0 && (v->status & 2) && v->h_res > 0 && h_start < 640 && v_res > 0 && v->max_x < 640 + 8 && v->max_y < 576 + 8;
}

void vi_decode_only(void) { vi_decode(); vi_frame_count++; }

static inline u32 vi_fetch(s32 x, s32 y, int fmt32, int aa) {
  u32 r, g, b, a;
  if (fmt32) {
    u32 lin = ((u32)(y * (s32)vi_state.width + x) + (vi_state.origin >> 2)) & ((RDRAM_MAX >> 2) - 1);
    u32 w = *(u32 *)(rdram + (lin << 2));
    r = w >> 24; g = (w >> 16) & 0xFF; b = (w >> 8) & 0xFF; a = (w >> 5) & 7;
  } else {
    u32 lin = ((u32)(y * (s32)vi_state.width + x) + (vi_state.origin >> 1)) & ((RDRAM_MAX >> 1) - 1);
    u32 w = *(u16 *)(rdram + ((lin ^ 1) << 1));
    r = (w >> 8) & 0xF8; g = (w >> 3) & 0xF8; b = (w << 2) & 0xF8; a = ((w & 1) << 2) | HIDDEN_AT(lin, w);
  }
  if (!aa) a = 7;
  return r | (g << 8) | (b << 16) | (a << 24);
}

static inline u32 median3(u32 l, u32 c, u32 r) {
  u32 t;
  if (l < c) { t = l; l = c; c = t; }
  if (c < r) { t = c; c = r; r = t; }
  if (l < c) { t = l; l = c; c = t; }
  return c;
}

void vi_render(void) {
  if (!gamma_tab[255]) {
    for (u32 i = 0; i < 256; i++) gamma_tab[i] = (u8)(2 * isqrt32(i << 6));
    for (u32 i = 0; i < 16384; i++) gamma_dither_tab[i] = (u8)(2 * isqrt32(i));
  }
  vi_decode();
  vi_frame_count++;
  VIState *v = &vi_state;
  if (vi_out_h != v->out_h) { memset(vi_out, 0, sizeof vi_out); vi_out_h = v->out_h; }
  vi_out_w = 640;
  if (!v->valid) {
    for (u32 i = 0; i < 640 * vi_out_h; i++) vi_out[i] = 0xFF000000;
    vi_blank = 1;
    return;
  }
  vi_blank = 0;
  u32 status = v->status;
  int fmt32 = (status & 3) == 3;
  int is_aa = (status & 0x300) < 0x200, is_bilinear = (status & 0x300) < 0x300;
  int divot = (status & 0x10) != 0, dither_filter = (status & 0x10000) != 0;
  int gamma = (status & 8) != 0, gamma_dither = (status & 4) != 0;

  // raw fetch with a 3 pixel horizontal / 2 line vertical apron
  s32 W = v->max_x + 2, H = v->max_y + 2;
  s32 RW = W + 6, RH = H + 2;
#define RAW(x, y) vi_raw[((y) + 1) * RW + (x) + 3]
#define AA(x, y) vi_aa[(y) * RW + (x) + 3]
  for (s32 y = -1; y < H + 1; y++) for (s32 x = -3; x < W + 3; x++) RAW(x, y) = vi_fetch(x, y, fmt32, is_aa);
  // AA / dither filter
  for (s32 y = 0; y < H; y++) for (s32 x = -1; x < W + 1; x++) {
    u32 mid = RAW(x, y), cov = mid >> 24, out = mid;
    if (cov != 7) {
      static const s8 nb[6][2] = { {-1, -1}, {1, -1}, {-2, 0}, {2, 0}, {-1, 1}, {1, 1} };
      out = mid & 0xFF000000;
      for (int c = 0; c < 3; c++) {
        u32 m = (mid >> (8 * c)) & 0xFF, lo = m, hi = m, slo = m, shi = m;
        for (int k = 0; k < 6; k++) {
          u32 n = RAW(x + nb[k][0], y + nb[k][1]);
          if ((n >> 24) == 7) {
            u32 cc = (n >> (8 * c)) & 0xFF;
            u32 t = cc > lo ? cc : lo; if (t < slo) slo = t;
            t = cc < hi ? cc : hi; if (t > shi) shi = t;
            if (cc < lo) lo = cc;
            if (cc > hi) hi = cc;
          }
        }
        u32 offset = slo + shi - (m << 1);
        u32 col = (m + (((offset * (7 - cov)) + 4) >> 3)) & 0xFF;
        out |= col << (8 * c);
      }
    } else if (dither_filter) {
      out = mid & 0xFF000000;
      for (int c = 0; c < 3; c++) {
        s32 m = (mid >> (8 * c)) & 0xFF, t = m >> 3, acc = 0;
        for (s32 dy = -1; dy <= 1; dy++) for (s32 dx = -1; dx <= 1; dx++) {
          if (!dx && !dy) continue;
          s32 d = (s32)((RAW(x + dx, y + dy) >> (8 * c + 3)) & 0x1F) - t;
          acc += d < -1 ? -1 : d > 1 ? 1 : d;
        }
        out |= (u32)(((m & 0xF8) + acc) & 0xFF) << (8 * c);
      }
    }
    AA(x, y) = out;
  }
  // divot (in place into vi_raw's interior, reusing it as the final filtered image)
#define FIN(x, y) vi_raw[((y) + 1) * RW + (x) + 3]
  for (s32 y = 0; y < H; y++) for (s32 x = 0; x < W; x++) {
    u32 mid = AA(x, y);
    if (divot) {
      u32 l = AA(x - 1, y), r = AA(x + 1, y);
      if (((l & mid & r) >> 24) != 7) {
        u32 o = mid & 0xFF000000;
        for (int c = 0; c < 3; c++) o |= median3((l >> (8 * c)) & 0xFF, (mid >> (8 * c)) & 0xFF, (r >> (8 * c)) & 0xFF) << (8 * c);
        mid = o;
      }
    }
    FIN(x, y) = mid;
  }
  // scale
  s32 lines = v->serrate ? v->out_h / 2 : v->out_h;
  for (s32 fy = 0; fy < lines; fy++) {
    u32 *out = vi_out + (v->serrate ? (fy * 2 + (v->field == 0 ? 1 : 0)) : fy) * 640;
    s32 cy = fy - v->v_start;
    if (cy < 0 || cy >= v->v_res) { for (int x = 0; x < 640; x++) out[x] = 0xFF000000; continue; }
    s32 y = cy * v->y_add + v->y_start;
    s32 by = y >> 10, yf = (y >> 5) & 31;
    for (s32 ox = 0; ox < 640; ox++) {
      if (ox < v->h_start_clamp || ox >= v->h_end_clamp) { out[ox] = 0xFF000000; continue; }
      s32 cx = ox - v->h_start;
      s32 x = cx * v->x_add + v->x_start;
      s32 bx = x >> 10;
      u32 c00 = FIN(bx, by);
      u32 rgb[3];
      if (is_bilinear) {
        s32 xf = (x >> 5) & 31;
        u32 c10 = FIN(bx + 1, by), c01 = FIN(bx, by + 1), c11 = FIN(bx + 1, by + 1);
        for (int c = 0; c < 3; c++) {
          u32 a = (c00 >> (8 * c)) & 0xFF, b = (c10 >> (8 * c)) & 0xFF, d = (c01 >> (8 * c)) & 0xFF, e = (c11 >> (8 * c)) & 0xFF;
          a = (a + (((d - a) * yf + 16) >> 5)) & 0xFF;
          b = (b + (((e - b) * yf + 16) >> 5)) & 0xFF;
          rgb[c] = (a + (((b - a) * xf + 16) >> 5)) & 0xFF;
        }
      } else { rgb[0] = c00 & 0xFF; rgb[1] = (c00 >> 8) & 0xFF; rgb[2] = (c00 >> 16) & 0xFF; }
      if (gamma_dither) {
        const u32 NP = 1103515245u;
        u32 s0 = (u32)cx, s1 = (u32)cy, s2 = v->frame, t0, t1, t2;
        for (int i = 0; i < 3; i++) { t0 = ((s0 >> 8) ^ s1) * NP; t1 = ((s1 >> 8) ^ s2) * NP; t2 = ((s2 >> 8) ^ s0) * NP; s0 = t0; s1 = t1; s2 = t2; }
        vi_noise = (s0 >> 16) & 0xFFFF;
      }
      if (gamma) {
        if (gamma_dither) {
          u32 n[3] = { vi_noise & 0x3F, (vi_noise >> 6) & 0x3F, ((vi_noise >> 9) & 0x38) | (vi_noise & 7) };
          for (int c = 0; c < 3; c++) rgb[c] = gamma_dither_tab[(rgb[c] << 6) + n[c]];
        } else for (int c = 0; c < 3; c++) rgb[c] = gamma_tab[rgb[c]];
      } else if (gamma_dither) {
        for (int c = 0; c < 3; c++) { rgb[c] += (vi_noise >> c) & 1; if (rgb[c] > 255) rgb[c] = 255; }
      }
      out[ox] = rgb[0] | (rgb[1] << 8) | (rgb[2] << 16) | 0xFF000000;
    }
  }
}
