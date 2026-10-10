// Low-level RDP.
//
// The frontend decodes RDP commands into batch buffers (primitives, spans, state, tile descriptors,
// TMEM snapshots). A batch is then rasterized either by the software pixel pipeline in rdp_pixel.h
// (reference / fallback) or by the WebGPU compute pipeline, which consumes the very same buffers.
//
// The per-pixel math follows parallel-RDP (MIT, (c) Hans-Kristian Arntzen).
// Ordered feedback follows the pinned Angrylion comparison target; bounded
// differential results do not establish hardware accuracy.
#include "core.h"

// ---- flags (shared with the shader) -----------------------------------------
enum {
  SETUP_FLIP = 1, SETUP_DO_OFFSET = 2, SETUP_SKIP_XFRAC = 4, SETUP_INTERLACE = 8, SETUP_KEEP_ODD = 16, SETUP_RECT = 32, SETUP_FILL_COPY = 128
};
enum {
  RS_INTERLACE_FIELD = 1 << 0, RS_INTERLACE_KEEP_ODD = 1 << 1, RS_AA = 1 << 2, RS_PERSPECTIVE = 1 << 3, RS_TLUT = 1 << 4,
  RS_TLUT_TYPE = 1 << 5, RS_CVG_TIMES_ALPHA = 1 << 6, RS_ALPHA_CVG_SELECT = 1 << 7, RS_MULTI_CYCLE = 1 << 8,
  RS_TEX_LOD = 1 << 9, RS_SHARPEN = 1 << 10, RS_DETAIL = 1 << 11, RS_FILL = 1 << 12, RS_COPY = 1 << 13,
  RS_SAMPLE_QUAD = 1 << 14, RS_ALPHA_TEST = 1 << 15, RS_ALPHA_TEST_DITHER = 1 << 16, RS_MID_TEXEL = 1 << 17,
  RS_USES_TEXEL0 = 1 << 18, RS_USES_TEXEL1 = 1 << 19, RS_USES_LOD = 1 << 20, RS_USES_PIPELINED_TEXEL1 = 1 << 21,
  RS_CONVERT_ONE = 1 << 22, RS_BILERP0 = 1 << 23, RS_BILERP1 = 1 << 24, RS_NOISE_DUAL = 1 << 25, RS_KEY = 1 << 26, RS_NOISE = 1 << 28
};
enum {
  DB_DEPTH_TEST = 1 << 0, DB_DEPTH_UPDATE = 1 << 1, DB_FORCE_BLEND = 1 << 3, DB_IMAGE_READ = 1 << 4,
  DB_COLOR_ON_CVG = 1 << 5, DB_MULTI_CYCLE = 1 << 6, DB_AA = 1 << 7, DB_DITHER = 1 << 8
};
enum { TILE_CLAMP_S = 1, TILE_MIRROR_S = 2, TILE_CLAMP_T = 4, TILE_MIRROR_T = 8 };
enum { FB_I4, FB_I8, FB_5551, FB_IA88, FB_8888 };

// ---- batch buffers ------------------------------------------------------------
#define PRIM_WORDS 72
#define SPAN_WORDS 8
#define STATE_WORDS 16
#define TILE_WORDS 8
#define RDP_MAX_PRIMS 4096
#define RDP_MAX_SPANS 65536
#define RDP_MAX_STATES 512
#define RDP_MAX_TILESETS 1024
#define RDP_MAX_TMEM 256

enum { P_FLAGS, P_SPAN, P_YLO, P_YHI, P_STATE, P_TMEM, P_TILESET, P_SEQ, P_RGBA = 8, P_DRGBA_DX = 12, P_DRGBA_DE = 16,
  P_DRGBA_DY = 20, P_STZW = 24, P_DSTZW_DX = 28, P_DSTZW_DE = 32, P_DSTZW_DY = 36, P_CONST = 40, P_FOG = 48, P_BLEND = 49,
  P_FILL = 50, P_DZ = 51, P_CONV0 = 52, P_CONV1 = 53, P_YBASE = 54, P_TAIL = 55,
  // raw edge setup, used by the high-resolution GPU pass to rasterize at sub-pixel positions
  P_XH = 56, P_XM, P_XL, P_DXHDY, P_DXMDY, P_DXLDY, P_YH, P_YM, P_YL, P_SCX, P_SCY, P_STLO /* s, t */, P_STHI = P_STLO + 2 };
#define PF_ST_CLAMP 0x10000u   /* P_FLAGS: P_STLO / P_STHI are valid (high-resolution pass) */
enum { S_FLAGS, S_RGB0, S_ALPHA0, S_RGB1, S_ALPHA1, S_DITHER, S_DB, S_BLEND0, S_BLEND1, S_CVGZ, S_KEY_R, S_KEY_G, S_KEY_B };

typedef struct {
  // per-batch framebuffer description (uniform block for the GPU path)
  u32 fb_fmt, fb_width, fb_height, fb_addr;     // fb_addr in pixel units
  u32 depth_addr;                               // in 16-bit units
  u32 fb_size, dx_shift, dx_mask;
  u32 num_prims, num_spans, num_states, num_tilesets, num_tmem;
  u32 alias, rdram_mask, z_use;                 // z_use: bit 0 some primitive depth-tests, bit 1 some primitive writes depth
  u32 tiles_x, ntiles, bins_words, ordered;
} BatchInfo;

s32 b_prims[RDP_MAX_PRIMS * PRIM_WORDS] __attribute__((aligned(16)));
u32 b_spans[(RDP_MAX_SPANS + 8) * SPAN_WORDS] __attribute__((aligned(16)));
u32 b_states[RDP_MAX_STATES * STATE_WORDS] __attribute__((aligned(16)));
u32 b_tiles[RDP_MAX_TILESETS * 8 * TILE_WORDS] __attribute__((aligned(16)));
u32 b_tmem[RDP_MAX_TMEM * 1024] __attribute__((aligned(16)));
BatchInfo b_info;
u8 rdp_hidden[RDRAM_MAX / 2];          // bits 0-1: hidden bits, bit 6: software fallback changed hidden bits, bit 7: written by the CPU since the last GPU sync
u16 rdp_shadow16[RDRAM_MAX / 2];
u8 blender_lut[0x8000];
u32 rdp_gpu_mode, rdp_hd_mode;
u32 rdp_stat_prims, rdp_stat_flushes, rdp_stat_spans;

// ---- RDP register state -------------------------------------------------------
typedef struct { u32 slo, shi, tlo, thi, offset, stride; u8 fmt, size, palette, mask_s, shift_s, mask_t, shift_t, flags; } Tile;

static struct {
  u32 cmd[0x10000]; u32 cmd_n;
  Tile tiles[8];
  u32 tmem[1024];                 // 4KB, host word order
  u32 sflags, dbflags, dither, cvg_mode, z_mode;
  u8 comb[4][4];                  // rgb0, alpha0, rgb1, alpha1 : muladd, mulsub, mul, add
  u8 blend[2][4];
  u32 sc_xlo, sc_ylo, sc_xhi, sc_yhi;
  u32 fog, blend_color, prim_color, env_color, fill_color;
  u32 prim_lod_frac, min_level;
  s32 convert[6];
  u32 key_center[3], key_scale[3], key_width[3];
  s32 prim_depth; u32 prim_dz, use_prim_depth;
  u32 ti_addr, ti_width, ti_size, ti_fmt;
  u32 fb_addr, fb_width, fb_fmt_field, fb_size_field, fb_fmt;
  u32 z_addr;
  u32 state_dirty, tiles_dirty, tmem_dirty;
  u32 prim_seq;
  u32 deduced_height;
} rdp;

#define TMEM8(i) (((u8 *)rdp.tmem)[(i)])
#define TMEM16(i) (((u16 *)rdp.tmem)[(i)])

void rdp_flush(void);

// ---------------------------------------------------------------------------
// pixel pipeline (software reference) -- included as a separate file so that it
// reads like the shader it mirrors
// ---------------------------------------------------------------------------
extern u32 rdp_hd_mode;
#include "rdp_pixel.h"
#include "gpu.c"

// ---------------------------------------------------------------------------
// span setup
// ---------------------------------------------------------------------------
static inline s32 sext(s32 v, int bits) { return (s32)((u32)v << (32 - bits)) >> (32 - bits); }

typedef struct { s32 xh, xm, xl, dxhdy, dxmdy, dxldy; s32 yh, ym, yl; u32 flags, tile; } TriSetup;
typedef struct { s32 rgba[4], drgba_dx[4], drgba_de[4], drgba_dy[4], stzw[4], dstzw_dx[4], dstzw_de[4], dstzw_dy[4]; } AttrSetup;

static void span_setup(const TriSetup *s, int y, u32 *out) {
  int flip = s->flags & SETUP_FLIP;
  // attribute interpolation base
  s32 dy = y - (s->yh >> 2);
  s32 xhb = (s32)(s->xh + (s64)dy * (s64)(s32)((u32)s->dxhdy << 2));  // s64: the product can overflow 32 bits
  if (s->flags & SETUP_DO_OFFSET) xhb = (s32)((s64)xhb + 3 * (s64)s->dxhdy);
  s32 base_x = xhb >> 15;
  s32 xfrac = (s->flags & SETUP_SKIP_XFRAC) ? 0 : ((xhb >> 7) & 0xFF);

  s32 yh_base = s->yh & ~3, ym_base = s->ym;
  s32 ylo = s->yh > (s32)rdp.sc_ylo ? s->yh : (s32)rdp.sc_ylo;
  s32 yhi = s->yl < (s32)rdp.sc_yhi ? s->yl : (s32)rdp.sc_yhi;
  s32 lo_sc = (s32)(rdp.sc_xlo << 1), hi_sc = (s32)(rdp.sc_xhi << 1);
  s32 xleft[4], xright[4];
  int all_invalid = 1, all_over = 1, all_under = 1;
  s32 minl = 0x7FFFFFFF, maxr = -0x7FFFFFFF - 1;
  // conservative x range of the line for the high-resolution pass: its sub-pixel spans lie between the edges at
  // neighbouring sub-scanlines, including crossed ones and the first sub-scanline of the next line
  s32 cons_l = 0x7FFFFFFF, cons_r = -1;
  for (int i = 0; i < 5; i++) {
    s32 ys = y * 4 + i;
    int clip_y = ys < ylo || ys >= yhi;
    if (i == 4 && (!rdp_hd_mode || ys > yhi)) break;
    s32 xh = (s32)((s64)s->xh + (s64)(ys - yh_base) * s->dxhdy);  // truncate only after widened edge math
    s32 xm = (s32)((s64)s->xm + (s64)(ys - yh_base) * s->dxmdy);  // truncate only after widened edge math
    s32 xl = (s32)((s64)s->xl + (s64)(ys - ym_base) * s->dxldy);  // truncate only after widened edge math
    if (ys < s->ym) xl = xm;
    xl = sext(xl, 27); xh = sext(xh, 27);
    s32 xhs = (xh >> 12) | ((xh & 0xFFF) != 0);
    s32 xls = (xl >> 12) | ((xl & 0xFFF) != 0);
    s32 l = flip ? xhs : xls, r = flip ? xls : xhs;
    int invalid = (l >> 1) > (r >> 1);
    s32 mn = l < r ? l : r, mx = l > r ? l : r;
    if (i < 4 && !(mn >= hi_sc)) all_over = 0;
    if (i < 4 && !(mx < lo_sc)) all_under = 0;
    if (l < lo_sc) l = lo_sc;
    if (l > hi_sc) l = hi_sc;
    if (r < lo_sc) r = lo_sc;
    if (r > hi_sc) r = hi_sc;
    if (ys >= ylo && ys <= yhi) { if (l < cons_l) cons_l = l; if (r > cons_r) cons_r = r; }
    if (i == 4) break;
    invalid |= clip_y;
    if (invalid) { l = 0xFFFF; r = 0; } else all_invalid = 0;
    xleft[i] = l & 0xFFFF; xright[i] = r & 0xFFFF;
    if (xleft[i] < minl) minl = xleft[i];
    if (xright[i] > maxr) maxr = xright[i];
  }
  s32 start_x = minl >> 3, end_x = maxr >> 3;
  int valid = !all_invalid && !all_over && !all_under;
  int hd_valid = cons_r >= cons_l;
  if ((s->flags & SETUP_INTERLACE) && ((y & 1) != ((s->flags & SETUP_KEEP_ODD) ? 1 : 0))) valid = hd_valid = 0;
  s32 lodlength = flip ? (end_x - base_x) : (base_x - start_x);
  out[0] = (u32)xleft[0] | ((u32)xleft[1] << 16);
  out[1] = (u32)xleft[2] | ((u32)xleft[3] << 16);
  out[2] = (u32)xright[0] | ((u32)xright[1] << 16);
  out[3] = (u32)xright[2] | ((u32)xright[3] << 16);
  out[4] = (u32)start_x | ((u32)end_x << 16);
  out[5] = (u32)base_x;
  out[6] = (u32)xfrac | ((u32)(lodlength & 0xFFFF) << 16);
  // bit 0: line has pixels; bit 1: line may have pixels in the high-resolution pass, within [bits 2-14, bits 15-27]
  out[7] = (u32)valid | (hd_valid ? 2u | ((u32)((cons_l >> 3) & 0x1FFF) << 2) | ((u32)((cons_r >> 3) & 0x1FFF) << 15) : 0);
}

// ---------------------------------------------------------------------------
// state emission
// ---------------------------------------------------------------------------
static inline u32 pack4(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static int comb_acc_t0(int c) {   // c = 0 (cycle0) or 1 (cycle1)
  const u8 *r = rdp.comb[c * 2], *a = rdp.comb[c * 2 + 1];
  return r[0] == 1 || r[1] == 1 || r[2] == 1 || r[3] == 1 || r[2] == 8 || a[0] == 1 || a[1] == 1 || a[2] == 1 || a[3] == 1;
}
static int comb_acc_t1(int c) {
  const u8 *r = rdp.comb[c * 2], *a = rdp.comb[c * 2 + 1];
  return r[0] == 2 || r[1] == 2 || r[2] == 2 || r[3] == 2 || r[2] == 9 || a[0] == 2 || a[1] == 2 || a[2] == 2 || a[3] == 2;
}
static int comb_acc_lod(int c) { return rdp.comb[c * 2][2] == 13 || rdp.comb[c * 2 + 1][2] == 0; }

static u32 deduce_flags(void) {
  u32 f = rdp.sflags & ~(RS_USES_TEXEL0 | RS_USES_TEXEL1 | RS_USES_PIPELINED_TEXEL1 | RS_USES_LOD | RS_NOISE | RS_NOISE_DUAL);
  if (!(f & (RS_FILL | RS_COPY))) {
    int multi = (f & RS_MULTI_CYCLE) != 0;
    int t0 = multi ? (comb_acc_t0(0) || comb_acc_t1(1)) : comb_acc_t0(1);
    int t1 = multi ? (comb_acc_t1(0) || comb_acc_t0(1)) : 0;
    int pt1 = multi ? 0 : comb_acc_t1(1);
    int lod = multi ? (comb_acc_lod(0) || comb_acc_lod(1)) : comb_acc_lod(1);
    if (t1 && (f & RS_CONVERT_ONE)) t0 = 1;
    if (t0) f |= RS_USES_TEXEL0;
    if (t1) f |= RS_USES_TEXEL1;
    if (pt1) f |= RS_USES_PIPELINED_TEXEL1;
    if (lod || (f & RS_TEX_LOD)) f |= RS_USES_LOD;
  }
  // noise
  if ((rdp.dither & 3) == 2 || ((rdp.dither >> 2) & 3) == 2) f |= RS_NOISE;
  else if (!(f & (RS_FILL | RS_COPY))) {
    if ((f & RS_MULTI_CYCLE) && rdp.comb[0][0] == 7) f |= RS_NOISE;
    if (rdp.comb[2][0] == 7) f |= RS_NOISE;
    if ((f & RS_MULTI_CYCLE) && rdp.comb[0][0] == 7 && rdp.comb[2][0] == 7) f |= RS_NOISE_DUAL;
    if ((f & (RS_ALPHA_TEST | RS_ALPHA_TEST_DITHER)) == (RS_ALPHA_TEST | RS_ALPHA_TEST_DITHER)) f |= RS_NOISE;
  }
  return f;
}

static u32 emit_state(void) {
  if (!rdp.state_dirty && b_info.num_states) return b_info.num_states - 1;
  u32 st[STATE_WORDS] = {0};
  st[S_FLAGS] = deduce_flags();
  st[S_RGB0] = pack4(rdp.comb[0]); st[S_ALPHA0] = pack4(rdp.comb[1]);
  st[S_RGB1] = pack4(rdp.comb[2]); st[S_ALPHA1] = pack4(rdp.comb[3]);
  st[S_DITHER] = rdp.dither;
  st[S_DB] = rdp.dbflags;
  st[S_BLEND0] = pack4(rdp.blend[0]); st[S_BLEND1] = pack4(rdp.blend[1]);
  st[S_CVGZ] = rdp.cvg_mode | (rdp.z_mode << 8);
  for (int i = 0; i < 3; i++) st[S_KEY_R + i] = rdp.key_width[i];
  rdp.state_dirty = 0;
  if (b_info.num_states && !memcmp(st, b_states + (b_info.num_states - 1) * STATE_WORDS, sizeof st)) return b_info.num_states - 1;
  memcpy(b_states + b_info.num_states * STATE_WORDS, st, sizeof st);
  return b_info.num_states++;
}

static u32 emit_tiles(void) {
  if (!rdp.tiles_dirty && b_info.num_tilesets) return b_info.num_tilesets - 1;
  u32 *o = b_tiles + b_info.num_tilesets * 8 * TILE_WORDS;
  for (int i = 0; i < 8; i++, o += TILE_WORDS) {
    Tile *t = &rdp.tiles[i];
    o[0] = t->slo; o[1] = t->shi; o[2] = t->tlo; o[3] = t->thi;
    o[4] = t->offset | (t->stride << 16);
    o[5] = t->fmt | (t->size << 8) | (t->palette << 16) | ((u32)t->flags << 24);
    o[6] = t->mask_s | (t->shift_s << 8) | (t->mask_t << 16) | ((u32)t->shift_t << 24);
    o[7] = 0;
  }
  rdp.tiles_dirty = 0;
  return b_info.num_tilesets++;
}

static u32 emit_tmem(void) {
  if (!rdp.tmem_dirty && b_info.num_tmem) return b_info.num_tmem - 1;
  memcpy(b_tmem + b_info.num_tmem * 1024, rdp.tmem, 4096);
  rdp.tmem_dirty = 0;
  return b_info.num_tmem++;
}

static u32 comb_const(int cyc, int slot) {
  // slot: 0 muladd, 1 mulsub, 2 mul, 3 add. Returns 0xRRGGBBAA of the constant input (0 if not constant).
  u8 r = rdp.comb[cyc * 2][slot], a = rdp.comb[cyc * 2 + 1][slot];
  u32 rgb = 0, alpha = 0;
  u32 prim = rdp.prim_color, env = rdp.env_color;
  switch (slot) {
    case 0: if (r == 3) rgb = prim; else if (r == 5) rgb = env; break;
    case 1:
      if (r == 3) rgb = prim; else if (r == 5) rgb = env;
      else if (r == 6) rgb = ((u32)rdp.key_center[0] << 24) | (rdp.key_center[1] << 16) | (rdp.key_center[2] << 8);
      else if (r == 7) rgb = ((u32)rdp.convert[4] & 0x1FF) << 8;
      break;
    case 2:
      if (r == 3) rgb = prim; else if (r == 5) rgb = env;
      else if (r == 6) rgb = ((u32)rdp.key_scale[0] << 24) | (rdp.key_scale[1] << 16) | (rdp.key_scale[2] << 8);
      else if (r == 10) rgb = 0x01010100u * (prim & 0xFF);
      else if (r == 12) rgb = 0x01010100u * (env & 0xFF);
      else if (r == 14) rgb = 0x01010100u * (rdp.prim_lod_frac & 0xFF);
      else if (r == 15) rgb = ((u32)rdp.convert[5] & 0x1FF) << 8;
      break;
    default: if (r == 3) rgb = prim; else if (r == 5) rgb = env; break;
  }
  if (a == 3) alpha = prim & 0xFF; else if (a == 5) alpha = env & 0xFF;
  else if (slot == 2 && a == 6) alpha = rdp.prim_lod_frac & 0xFF;
  return (rgb & 0xFFFFFF00) | alpha;
}

static int normalize_dzpix(int dz) {
  if (dz >= 0x8000) return 0x8000;
  if (dz == 0) return 1;
  if (dz == 1) return 3;
  int bit = 31 - __builtin_clz(dz);
  return 1 << (bit + 1);
}
static int dz_compress_setup(int dz) {
  int v = 0;
  if (dz & 0xFF00) v |= 8;
  if (dz & 0xF0F0) v |= 4;
  if (dz & 0xCCCC) v |= 2;
  if (dz & 0xAAAA) v |= 1;
  return v;
}

static void batch_begin(void) {
  b_info.fb_fmt = rdp.fb_fmt; b_info.fb_width = rdp.fb_width;
  switch (rdp.fb_fmt) {
    case FB_I4: b_info.fb_addr = rdp.fb_addr; b_info.fb_size = 0; b_info.dx_mask = 0; b_info.dx_shift = 0; break;
    case FB_I8: b_info.fb_addr = rdp.fb_addr; b_info.fb_size = 1; b_info.dx_mask = ~7u; b_info.dx_shift = 3; break;
    case FB_8888: b_info.fb_addr = rdp.fb_addr >> 2; b_info.fb_size = 4; b_info.dx_mask = 0; b_info.dx_shift = 1; break;
    default: b_info.fb_addr = rdp.fb_addr >> 1; b_info.fb_size = 2; b_info.dx_mask = ~3u; b_info.dx_shift = 2; break;
  }
  b_info.depth_addr = rdp.z_addr >> 1;
  b_info.alias = (rdp.fb_addr == rdp.z_addr) && (rdp.fb_fmt == FB_5551 || rdp.fb_fmt == FB_IA88);
  b_info.rdram_mask = RDRAM_MAX - 1;
  b_info.z_use = 0;
  rdp.deduced_height = 0; b_info.ordered = 0;
}

// High-resolution pass: texture coordinate bounds.
// That pass samples textures between the points the console samples. For flat (non-perspective) primitives - texture
// rectangles and 2D triangles - games only make sure that the texels under those points are right: a 32-texel tile drawn
// 32 pixels wide is sampled at texels 0..31 and never beyond. A sample a fraction of a pixel past the last point would
// filter against whatever lies outside the picture (the other edge of the tile wrapped around, or stale TMEM), which
// shows as lines wherever tiles meet. So the coordinates are held inside the range the primitive samples at native
// resolution: within the picture the magnified texture stays smooth, at its edge the last native sample is repeated.
static void st_bounds(s32 *p, const u32 *spans, s32 min_line, s32 max_line, int aa) {
  s32 lo[2] = { 0x7FFFFFFF, 0x7FFFFFFF }, hi[2] = { -0x7FFFFFFF - 1, -0x7FFFFFFF - 1 };
  int do_offset = (p[P_FLAGS] & SETUP_DO_OFFSET) != 0;
  for (s32 y = min_line; y <= max_line; y++) {
    const u32 *sp = spans + (u32)(y - min_line) * SPAN_WORDS;
    if (!(sp[7] & 1)) continue;
    s32 x0 = sp[4] & 0xFFFF, x1 = sp[4] >> 16;
    // first and last pixel of the line that are drawn (the same rule as shade_and_blend)
#define DRAWN(x) ({ u32 c_ = compute_coverage(sp, (x)); c_ && (aa || (c_ & 1)); })
    while (x0 <= x1 && !DRAWN(x0)) x0++;
    while (x1 > x0 && !DRAWN(x1)) x1--;
#undef DRAWN
    if (x0 > x1) continue;
    s32 xfrac = sp[6] & 0xFF, base_x = (s32)sp[5], dy = y - p[P_YBASE];
    for (int i = 0; i < 2; i++) {
      s32 de = p[P_DSTZW_DE + i], dyv = p[P_DSTZW_DY + i], ddx = p[P_DSTZW_DX + i], diff = 0;
      if (do_offset) { s32 deh = de & ~0x1FF, dyh = dyv & ~0x1FF; diff = deh - (deh >> 2) - dyh + (dyh >> 2); }
      u32 v = (u32)p[P_STZW + i] + (u32)de * (u32)dy;
      u32 base = ((v & ~0x1FFu) + (u32)diff - (u32)xfrac * (u32)((ddx >> 8) & ~1)) & ~0x3FFu;
      u32 sdx = (u32)(ddx & ~0x1F);
      s32 a = (s32)(base + sdx * (u32)(x0 - base_x)) >> 16, b = (s32)(base + sdx * (u32)(x1 - base_x)) >> 16;
      if (a < lo[i]) lo[i] = a;
      if (b < lo[i]) lo[i] = b;
      if (a > hi[i]) hi[i] = a;
      if (b > hi[i]) hi[i] = b;
    }
  }
  if (lo[0] > hi[0]) return;
  p[P_STLO] = lo[0]; p[P_STLO + 1] = lo[1]; p[P_STHI] = hi[0]; p[P_STHI + 1] = hi[1];
  p[P_FLAGS] |= (s32)PF_ST_CLAMP;
}

static void draw_primitive(TriSetup *s, AttrSetup *a) {
  if (b_info.num_prims == 0) batch_begin();
  // fixups
  s32 start_y = s->yh & ~3;
  if (s->ym < start_y) s->ym = 0x7FFF;
  if (rdp.sflags & RS_INTERLACE_FIELD) {
    s->flags |= SETUP_INTERLACE;
    if (rdp.sflags & RS_INTERLACE_KEEP_ODD) s->flags |= SETUP_KEEP_ODD;
  }
  if (rdp.sflags & (RS_COPY | RS_FILL)) s->flags |= SETUP_FILL_COPY;

  s32 min_sub = s->yh > (s32)rdp.sc_ylo ? s->yh : (s32)rdp.sc_ylo;
  s32 max_sub = (s->yl - 1) < ((s32)rdp.sc_yhi - 1) ? (s->yl - 1) : ((s32)rdp.sc_yhi - 1);
  s32 min_line = min_sub >> 2, max_line = max_sub >> 2;
  rdp.prim_seq++;
  if (max_line < min_line || min_line > 1023) return;
  if (max_line > min_line + 1023) max_line = min_line + 1023;
  s32 nspans = max_line - min_line + 2;
  if (b_info.num_prims >= RDP_MAX_PRIMS || b_info.num_spans + nspans > RDP_MAX_SPANS ||
      b_info.num_states + 1 >= RDP_MAX_STATES || b_info.num_tilesets + 1 >= RDP_MAX_TILESETS || b_info.num_tmem + 1 >= RDP_MAX_TMEM ||
      (rdp_gpu_mode && bin_would_overflow(nspans, rdp.fb_width))) {
    rdp_flush();
    batch_begin();
    rdp.state_dirty = rdp.tiles_dirty = rdp.tmem_dirty = 1;
  }
  if ((u32)(max_line + 1) > rdp.deduced_height) rdp.deduced_height = max_line + 1;

  s32 *p = b_prims + b_info.num_prims * PRIM_WORDS;
  p[P_FLAGS] = (s32)(s->flags | (s->tile << 8));
  p[P_SPAN] = b_info.num_spans;
  p[P_YLO] = min_line; p[P_YHI] = max_line;
  p[P_STATE] = emit_state();
  if (!(rdp.sflags & (RS_FILL | RS_COPY))) b_info.z_use |= ((rdp.dbflags & DB_DEPTH_TEST) ? 1 : 0) | ((rdp.dbflags & DB_DEPTH_UPDATE) ? 2 : 0);
  p[P_TMEM] = emit_tmem();
  p[P_TILESET] = emit_tiles();
  p[P_SEQ] = rdp.prim_seq;
  if (rdp.use_prim_depth) { a->stzw[2] = rdp.prim_depth; a->dstzw_dx[2] = a->dstzw_de[2] = a->dstzw_dy[2] = 0; }
  memcpy(p + P_RGBA, a, sizeof *a);
  for (int c = 0; c < 2; c++) for (int k = 0; k < 4; k++) p[P_CONST + c * 4 + k] = (s32)comb_const(c, k);
  p[P_FOG] = rdp.fog; p[P_BLEND] = rdp.blend_color; p[P_FILL] = rdp.fill_color;
  u32 dz, dzc;
  if (rdp.use_prim_depth) { dz = rdp.prim_dz; dzc = dz_compress_setup(dz); }
  else {
    int dzdx = a->dstzw_dx[2] >> 16, dzdy = a->dstzw_dy[2] >> 16;
    int dzpix = (dzdx < 0 ? (~dzdx & 0x7FFF) : dzdx) + (dzdy < 0 ? (~dzdy & 0x7FFF) : dzdy);
    dz = normalize_dzpix(dzpix); dzc = dz_compress_setup(dz);
  }
  p[P_DZ] = (s32)(dz | (dzc << 16) | (rdp.min_level << 24));
  p[P_CONV0] = (rdp.convert[0] & 0xFFFF) | (rdp.convert[1] << 16);
  p[P_CONV1] = (rdp.convert[2] & 0xFFFF) | (rdp.convert[3] << 16);
  p[P_YBASE] = s->yh >> 2;
  p[P_TAIL] = -1; // final shaded sample, for deterministic GPU feedback reduction
  p[P_XH] = s->xh; p[P_XM] = s->xm; p[P_XL] = s->xl; p[P_DXHDY] = s->dxhdy; p[P_DXMDY] = s->dxmdy; p[P_DXLDY] = s->dxldy;
  p[P_YH] = s->yh; p[P_YM] = s->ym; p[P_YL] = s->yl;
  p[P_SCX] = (s32)(rdp.sc_xlo | (rdp.sc_xhi << 16)); p[P_SCY] = (s32)(rdp.sc_ylo | (rdp.sc_yhi << 16));
  for (int k = 67; k < PRIM_WORDS; k++) p[k] = 0;

  u32 *sp = b_spans + b_info.num_spans * SPAN_WORDS;
  for (s32 y = min_line; y <= max_line; y++, sp += SPAN_WORDS) span_setup(s, y, sp);
  const u32 *st = b_states + (u32)p[P_STATE] * STATE_WORDS;
  if (!(st[S_FLAGS] & (RS_COPY | RS_FILL))) {
    int cycle = (st[S_FLAGS] & RS_MULTI_CYCLE) ? 0 : 1;
    u32 rgb = st[S_RGB0 + 2 * cycle], alpha = st[S_ALPHA0 + 2 * cycle];
    int feedback = 0;
    for (int slot = 0; slot < 4; slot++) {
      if (((rgb >> (slot * 8)) & 255) == 0 || (slot == 2 && ((rgb >> 16) & 255) == 7)) feedback = 1;
      if (slot != 2 && ((alpha >> (slot * 8)) & 255) == 0) feedback = 1;
    }
    if (!(st[S_DB] & DB_IMAGE_READ) && (rdp.blend[0][0] == 1 || rdp.blend[0][2] == 1 ||
        ((st[S_FLAGS] & RS_MULTI_CYCLE) && (rdp.blend[1][0] == 1 || rdp.blend[1][2] == 1)))) feedback = 1;
    // Cycle-zero blending reads the previous framebuffer sample in two-cycle mode.
    if (st[S_FLAGS] & RS_MULTI_CYCLE) feedback = 1;
    b_info.ordered |= feedback;
  }
  for (s32 y = min_line; y <= max_line; y++) {
    const u32 *span = b_spans + (b_info.num_spans + y - min_line) * SPAN_WORDS;
    if (!(span[7] & 1)) continue;
#ifdef RDP_ORACLE
    // Include every target since SYNC_FULL, including row aliases and buffers
    // replaced by a later SetColorImage. Compare before resynchronizing images.
    extern void rdp_oracle_touch(u32, u32, u32);
    u32 start = (span[4] & 65535) + y * rdp.fb_width;
    u32 count = (span[4] >> 16) - (span[4] & 65535) + 1;
    u32 size = rdp.fb_fmt == FB_8888 ? 4 : rdp.fb_fmt >= FB_5551 ? 2 : 1;
    rdp_oracle_touch(rdp.fb_addr + start * size, count * size, 0);
    if (!(st[S_FLAGS] & (RS_COPY | RS_FILL)) && (st[S_DB] & (DB_DEPTH_TEST | DB_DEPTH_UPDATE)))
      rdp_oracle_touch(rdp.z_addr + start * 2, count * 2, 1);
#endif
    u32 end = span[4] >> 16;
    if (end >= rdp.fb_width) {
      b_info.ordered = 1;
      u32 height = y + end / rdp.fb_width + 1;
      if (height > rdp.deduced_height) rdp.deduced_height = height;
    }
    if (!(st[S_FLAGS] & (RS_COPY | RS_FILL))) {
      s32 x = (p[P_FLAGS] & SETUP_FLIP) ? (s32)end : (s32)(span[4] & 65535);
      p[P_TAIL] = (y << 16) | x;
    }
  }
  memset(sp, 0, SPAN_WORDS * 4);   // sentinel line (valid = 0) for the next-line peek
  if (rdp_hd_mode && rdp_gpu_mode && !(rdp.sflags & (RS_FILL | RS_COPY | RS_PERSPECTIVE)) &&
      (b_states[(u32)p[P_STATE] * STATE_WORDS + S_FLAGS] & (RS_USES_TEXEL0 | RS_USES_TEXEL1 | RS_USES_PIPELINED_TEXEL1)))
    st_bounds(p, b_spans + b_info.num_spans * SPAN_WORDS, min_line, max_line, (rdp.sflags & RS_AA) != 0);
  if (rdp_gpu_mode) bin_primitive(b_info.num_prims, min_line, max_line, b_spans + b_info.num_spans * SPAN_WORDS, rdp.fb_width);
  b_info.num_spans += nspans;
  b_info.num_prims++;
  rdp_stat_prims++;
}

void rdp_flush(void) {
  if (!b_info.num_prims) return;
  b_info.fb_height = rdp.deduced_height;
  rdp_stat_flushes++; rdp_stat_spans += b_info.num_spans;
  // 8-bit framebuffers pack two pixels per GPU word; they are rare and are rendered by the software path
  if (rdp_gpu_mode && b_info.fb_fmt >= FB_5551) {
    b_info.bins_words = bin_build(b_info.fb_width, b_info.fb_height, &b_info.tiles_x, &b_info.ntiles);
    b_bins_words = b_info.bins_words;
    u32 pixels = b_info.fb_width * b_info.fb_height;
    if (b_info.fb_fmt == FB_8888) gpu_watch_region(b_info.fb_addr * 2, pixels * 2); else gpu_watch_region(b_info.fb_addr, pixels);
    if (!b_info.alias && (b_info.z_use & 2)) gpu_watch_region(b_info.depth_addr, pixels);
    host_gpu_flush();
    // from here on the CPU copy of what was just drawn is out of date
    if (b_info.fb_fmt == FB_8888) gpu_mark_stale(b_info.fb_addr * 2, pixels * 2); else gpu_mark_stale(b_info.fb_addr, pixels);
    if (!b_info.alias && (b_info.z_use & 2)) gpu_mark_stale(b_info.depth_addr, pixels);
  } else { sw_render_batch(); if (rdp_gpu_mode) gpu_feedback_dirty = 1; }
  bin_reset();
  b_info.num_prims = b_info.num_spans = b_info.num_states = b_info.num_tilesets = b_info.num_tmem = 0;
  rdp.state_dirty = rdp.tiles_dirty = rdp.tmem_dirty = 1;
}

// ---------------------------------------------------------------------------
// texture loading (TMEM is 4KB; RGBA32/YUV tiles split low/high halves)
// ---------------------------------------------------------------------------
static inline u32 vram_r16(u32 addr) {
  addr &= RDRAM_MAX - 1;
  if (!(addr & 1)) return *(u16 *)(rdram + (addr ^ 2));
  return (rdram[addr ^ 3] << 8) | rdram[((addr + 1) & (RDRAM_MAX - 1)) ^ 3];
}
static inline u32 vram_r32(u32 addr) { return (vram_r16(addr) << 16) | vram_r16(addr + 2); }

enum { LOAD_TILE, LOAD_TLUT, LOAD_BLOCK };

static void load_tlut(Tile *t, u32 slo, u32 shi, u32 tlo) {
  s32 width = (s32)((((shi >> 2) - (slo >> 2)) + 1) & 0xFFF);
  s32 vsize = rdp.ti_size, tsize = t->size;
  if (!width || vsize == 0 || tsize == 3) return;
  s32 eff = vsize == 1 ? ((width + 7) & ~7) : vsize == 2 ? width : ((width + 1) & ~1);
  u32 vram_addr = rdp.ti_addr + ((rdp.ti_width * (tlo >> 2) + (slo >> 2)) << (vsize - 1));
  s32 off16 = (t->offset & 0xFFF) >> 1;
  for (s32 idx = 0; idx < 0x800; idx++) {
    s32 po = (idx - off16) & 0x7FF, splat;
    s32 d = vsize - tsize;
    if (d == 2) { splat = (po >> 2) << (vsize - 2); }
    else if (d == 1) { if (po & 4) continue; splat = (po & ~7) >> (tsize + (vsize == 2 ? 2 : 0)); }
    else if (d == 0) { if (po & 0xC) continue; splat = (po & ~3) >> (tsize + (vsize == 2 ? 2 : 0)); }
    else if (d == -1) { if (po & 0x1C) continue; splat = (po >> tsize) & ~7; }
    else { s32 sp2 = (po >> 2) * 4; if (sp2 + 2 < eff) sp2 += 2; splat = sp2; }
    if (splat >= eff) continue;
    u32 addr = vram_addr + ((u32)splat << (vsize - 1));
    addr += 2 * (addr & 1) * (po & 3);
    TMEM16(idx ^ 1) = (u16)vram_r16(addr);
  }
}

// Texture fetches can wrap at RAM end; split both circular intervals before comparing.
static int ram_ranges_overlap(u32 a, u32 an, u32 b, u32 bn) {
  if (!an || !bn) return 0;
  if (an >= RDRAM_MAX || bn >= RDRAM_MAX) return 1;
  a &= RDRAM_MAX - 1; b &= RDRAM_MAX - 1;
  u32 a0 = an < RDRAM_MAX - a ? an : RDRAM_MAX - a;
  u32 b0 = bn < RDRAM_MAX - b ? bn : RDRAM_MAX - b;
  return (a < b + b0 && b < a + a0) ||
         (an > a0 && b < an - a0) || (bn > b0 && a < bn - b0) ||
         (an > a0 && bn > b0);
}
static int batch_reads_pending(u32 src, u32 bytes) {
  if (!b_info.num_prims) return 0;
  // Rare small framebuffer formats and uncertain dimensions retain the safe full flush.
  if (b_info.fb_fmt < FB_5551 || !rdp.deduced_height) return 1;
  u32 pixels = b_info.fb_width * rdp.deduced_height;
  u32 size = b_info.fb_fmt == FB_8888 ? 4 : 2;
  u32 color = b_info.fb_addr * size;
  return ram_ranges_overlap(src, bytes, color, pixels * size) ||
    (!b_info.alias && (b_info.z_use & 2) && ram_ranges_overlap(src, bytes, b_info.depth_addr * 2, pixels * 2));
}

static void rdp_load(u32 tile_i, u32 slo, u32 tlo, u32 shi, u32 thi, int mode) {
  Tile *t = &rdp.tiles[tile_i];
  if (!rdp_gpu_mode) {
    u32 vsize = rdp.ti_size;
    if (vsize) {
      u32 vbytes = 1u << (vsize - 1), src, bytes;
      if (mode == LOAD_BLOCK) {
        src = rdp.ti_addr + (rdp.ti_width * tlo + slo) * vbytes;
        bytes = ((((shi - slo + 1) & 0xFFF) * vbytes + 7) >> 3) * 8;
      } else {
        src = rdp.ti_addr + (rdp.ti_width * (tlo >> 2) + (slo >> 2)) * vbytes;
        bytes = mode == LOAD_TLUT ? 0x1000 : (thi >> 2) >= (tlo >> 2) ?
          ((thi >> 2) - (tlo >> 2)) * rdp.ti_width * vbytes + ((((((shi >> 2) - (slo >> 2) + 1) & 0xFFF) * vbytes + 7) >> 3) * 8) : 0;
      }
      if (batch_reads_pending(src, bytes)) rdp_flush();
    }
  }
  t->slo = slo; t->shi = shi; t->tlo = tlo; t->thi = thi;
  rdp.tiles_dirty = 1; rdp.tmem_dirty = 1;
  u32 vsize = rdp.ti_size, tsize = t->size;
  int yuv = t->fmt == 1;
  if (vsize == 0) return;
  if (mode == LOAD_TLUT) { load_tlut(t, slo, shi, tlo); return; }
  int split = (tsize == 3) || yuv;       // RG / BA (or UV / Y) split over lower and upper TMEM
  u32 vbytes = 1u << (vsize - 1);
  u32 texels, lines, src, dxt = 0;
  if (mode == LOAD_BLOCK) {
    texels = (shi - slo + 1) & 0xFFF;
    if (!texels || texels > 2048) return;
    lines = 1;
    src = rdp.ti_addr + (rdp.ti_width * tlo + slo) * vbytes;
    dxt = thi;
  } else {
    if ((thi >> 2) < (tlo >> 2)) return;
    texels = (((shi >> 2) - (slo >> 2)) + 1) & 0xFFF;
    if (!texels) return;
    lines = ((thi >> 2) - (tlo >> 2)) + 1;
    src = rdp.ti_addr + (rdp.ti_width * (tlo >> 2) + (slo >> 2)) * vbytes;
  }
  u32 iters = (texels * vbytes + 7) >> 3;                  // 64-bit RDRAM words per line
  u32 base16 = split ? ((t->offset & 0x7FF) >> 1) : ((t->offset & 0xFFF) >> 1);
  u32 line16 = t->stride >> 1;                             // TMEM line advance in 16-bit units
  u32 mask = split ? 0x3FF : 0x7FF;
  for (u32 l = 0; l < lines; l++) {
    u32 laddr = src + l * rdp.ti_width * vbytes;
    for (u32 i = 0; i < iters; i++) {
      u32 tline = mode == LOAD_BLOCK ? ((i * dxt) >> 11) : l;
      u32 swap = (tline & 1) << 1;
      u32 hi = vram_r32(laddr + i * 8), lo = vram_r32(laddr + i * 8 + 4);
      if (split) {
        // each iteration carries two 32-bit texels
        u32 pos = (tsize == 3 && !yuv) ? ((i << 3) >> vsize) : i;
        u32 d = base16 + tline * line16 + pos * 2;
        u32 w0[2], w1[2];
        if (yuv) {
          w0[0] = ((hi >> 16) & 0xFF00) | ((hi >> 8) & 0xFF); w1[0] = ((hi >> 8) & 0xFF00) | (hi & 0xFF);
          w0[1] = ((lo >> 16) & 0xFF00) | ((lo >> 8) & 0xFF); w1[1] = ((lo >> 8) & 0xFF00) | (lo & 0xFF);
        } else {
          w0[0] = hi >> 16; w1[0] = hi & 0xFFFF; w0[1] = lo >> 16; w1[1] = lo & 0xFFFF;
        }
        for (u32 k = 0; k < 2; k++) {
          u32 idx = ((d + k) ^ swap) & mask;
          TMEM16(idx ^ 1) = (u16)w0[k];
          TMEM16((idx | 0x400) ^ 1) = (u16)w1[k];
        }
      } else {
        u32 pos = (i << tsize) >> vsize;
        u32 d = base16 + tline * line16 + pos * 4;
        u32 w[4] = { hi >> 16, hi & 0xFFFF, lo >> 16, lo & 0xFFFF };
        for (u32 k = 0; k < 4; k++) TMEM16((((d + k) ^ swap) & mask) ^ 1) = (u16)w[k];
      }
    }
  }
}

// ---------------------------------------------------------------------------
// command decoding
// ---------------------------------------------------------------------------
u32 rdp_cmd_count[64];
static const u8 rdp_len[64] = {
  1, 1, 1, 1, 1, 1, 1, 1, 4, 6, 12, 14, 12, 14, 20, 22,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

#define SMASK(var, cond, bit) do { if (cond) (var) |= (bit); else (var) &= ~(u32)(bit); } while (0)

static void decode_tri(TriSetup *s, const u32 *w) {
  int flip = (w[0] & 0x800000u) != 0, sign_dxhdy = (w[5] & 0x80000000u) != 0;
  s->flags = (flip ? SETUP_FLIP : 0) | ((flip == sign_dxhdy) ? SETUP_DO_OFFSET : 0) | ((rdp.sflags & RS_COPY) ? SETUP_SKIP_XFRAC : 0);
  s->tile = (w[0] >> 16) & 63;
  s->yl = sext(w[0], 14); s->ym = sext(w[1] >> 16, 14); s->yh = sext(w[1], 14);
  s->xl = sext(w[2], 28) >> 1; s->xh = sext(w[4], 28) >> 1; s->xm = sext(w[6], 28) >> 1;
  s->dxldy = sext(w[3] >> 2, 28) >> 1; s->dxhdy = sext(w[5] >> 2, 28) >> 1; s->dxmdy = sext(w[7] >> 2, 28) >> 1;
}
static void decode_rgba(AttrSetup *a, const u32 *w) {
  for (int i = 0; i < 4; i++) {
    int sh = (i & 1) ? 0 : 16; int k = i >> 1;
    a->rgba[i] = (s32)((((w[0 + k] >> sh) & 0xFFFF) << 16) | ((w[4 + k] >> sh) & 0xFFFF));
    a->drgba_dx[i] = (s32)((((w[2 + k] >> sh) & 0xFFFF) << 16) | ((w[6 + k] >> sh) & 0xFFFF));
    a->drgba_de[i] = (s32)((((w[8 + k] >> sh) & 0xFFFF) << 16) | ((w[12 + k] >> sh) & 0xFFFF));
    a->drgba_dy[i] = (s32)((((w[10 + k] >> sh) & 0xFFFF) << 16) | ((w[14 + k] >> sh) & 0xFFFF));
  }
}
static void decode_tex(AttrSetup *a, const u32 *w) {
  static const u8 map[3] = { 0, 1, 3 };   // s, t, w  (index 2 is z)
  for (int i = 0; i < 3; i++) {
    int sh = (i & 1) ? 0 : 16; int k = i >> 1; int d = map[i];
    a->stzw[d] = (s32)((((w[0 + k] >> sh) & 0xFFFF) << 16) | ((w[4 + k] >> sh) & 0xFFFF));
    a->dstzw_dx[d] = (s32)((((w[2 + k] >> sh) & 0xFFFF) << 16) | ((w[6 + k] >> sh) & 0xFFFF));
    a->dstzw_de[d] = (s32)((((w[8 + k] >> sh) & 0xFFFF) << 16) | ((w[12 + k] >> sh) & 0xFFFF));
    a->dstzw_dy[d] = (s32)((((w[10 + k] >> sh) & 0xFFFF) << 16) | ((w[14 + k] >> sh) & 0xFFFF));
  }
}
static void decode_z(AttrSetup *a, const u32 *w) { a->stzw[2] = w[0]; a->dstzw_dx[2] = w[1]; a->dstzw_de[2] = w[2]; a->dstzw_dy[2] = w[3]; }

static void rdp_exec(const u32 *w) {
  u32 cmd = (w[0] >> 24) & 63;
  rdp_cmd_count[cmd]++;
  switch (cmd) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
      TriSetup s; AttrSetup a;
      memset(&a, 0, sizeof a);
      decode_tri(&s, w);
#ifdef RDP_DUMP
      { extern int rdp_dump_on; if (rdp_dump_on) printf("tri%s%s%s %s persp %d  y %.2f..%.2f\n", (cmd & 4) ? " shade" : "", (cmd & 2) ? " tex" : "", (cmd & 1) ? " z" : "", (rdp.sflags & RS_FILL) ? "FILL" : (rdp.sflags & RS_COPY) ? "COPY" : (rdp.sflags & RS_MULTI_CYCLE) ? "2CYC" : "1CYC", (rdp.sflags & RS_PERSPECTIVE) != 0, s.yh / 4.0, s.yl / 4.0); }
#endif
      const u32 *q = w + 8;
      if (cmd & 4) { decode_rgba(&a, q); q += 16; }
      if (cmd & 2) { decode_tex(&a, q); q += 16; }
      if (cmd & 1) decode_z(&a, q);
      draw_primitive(&s, &a);
      break;
    }
    case 0x24: case 0x25: case 0x36: {
      u32 xl = (w[0] >> 12) & 0xFFF, yl = w[0] & 0xFFF, xh = (w[1] >> 12) & 0xFFF, yh = w[1] & 0xFFF;
#ifdef RDP_DUMP
      { extern int rdp_dump_on; if (rdp_dump_on) { const Tile *t = &rdp.tiles[(w[1] >> 24) & 7];
        printf("%s %s x %.2f..%.2f y %.2f..%.2f", cmd == 0x36 ? "fillrect" : cmd == 0x24 ? "texrect " : "texflip ", (rdp.sflags & RS_FILL) ? "FILL" : (rdp.sflags & RS_COPY) ? "COPY" : (rdp.sflags & RS_MULTI_CYCLE) ? "2CYC" : "1CYC", xh / 4.0, xl / 4.0, yh / 4.0, yl / 4.0);
        if (cmd != 0x36) printf("  s %.3f t %.3f dsdx %.4f dtdy %.4f tile %d fmt %d/%d sl %.2f sh %.2f tl %.2f th %.2f cs %d ct %d ms %d mt %d quad %d", (s16)(w[2] >> 16) / 32.0, (s16)w[2] / 32.0, (s16)(w[3] >> 16) / 1024.0, (s16)w[3] / 1024.0,
          (w[1] >> 24) & 7, t->fmt, t->size, t->slo / 4.0, t->shi / 4.0, t->tlo / 4.0, t->thi / 4.0, (t->flags & TILE_CLAMP_S) != 0, (t->flags & TILE_CLAMP_T) != 0, t->mask_s, t->mask_t, (rdp.sflags & RS_SAMPLE_QUAD) != 0);
        printf("\n"); } }
#endif
      TriSetup s; AttrSetup a;
      memset(&a, 0, sizeof a);
      if (rdp.sflags & (RS_COPY | RS_FILL)) yl |= 3;
      s.xh = xh << 13; s.xl = xl << 13; s.xm = xl << 13; s.ym = yl; s.yl = yl; s.yh = yh;
      s.dxhdy = s.dxmdy = s.dxldy = 0;
      s.flags = SETUP_FLIP | SETUP_RECT; s.tile = 0;
      if (cmd != 0x36) {
        s32 dsdx = (s16)(w[3] >> 16), dtdy = (s16)w[3];
        s.tile = (w[1] >> 24) & 7;
        a.stzw[0] = (s32)((w[2] >> 16) << 16); a.stzw[1] = (s32)((w[2] & 0xFFFF) << 16);
        if (cmd == 0x24) { a.dstzw_dx[0] = dsdx << 11; a.dstzw_de[1] = dtdy << 11; a.dstzw_dy[1] = dtdy << 11; }
        else { a.dstzw_dx[1] = dtdy << 11; a.dstzw_de[0] = dsdx << 11; a.dstzw_dy[0] = dsdx << 11; }
        if (rdp.sflags & RS_COPY) s.flags |= SETUP_SKIP_XFRAC;
      }
      draw_primitive(&s, &a);
      break;
    }
    case 0x29:                                                         // sync full
      rdp_flush();
      // a picture is complete: a good moment for the host to start copying it back if the game tends to look at it
      if (rdp_gpu_mode && gpu_exact && gpu_stale_n) { gpu_hint = 1; host_gpu_flush(); gpu_hint = 0; }
      mi_raise(MI_DP);
      break;
    case 0x2A:
      rdp.key_width[1] = (w[0] >> 12) & 0xFFF; rdp.key_width[2] = w[0] & 0xFFF;
      rdp.key_center[1] = (w[1] >> 24) & 0xFF; rdp.key_scale[1] = (w[1] >> 16) & 0xFF;
      rdp.key_center[2] = (w[1] >> 8) & 0xFF; rdp.key_scale[2] = w[1] & 0xFF;
      rdp.state_dirty = 1;
      break;
    case 0x2B: rdp.key_width[0] = (w[1] >> 16) & 0xFFF; rdp.key_center[0] = (w[1] >> 8) & 0xFF; rdp.key_scale[0] = w[1] & 0xFF; rdp.state_dirty = 1; break;
    case 0x2C: {
      u64 m = ((u64)w[0] << 32) | w[1];
      for (int i = 0; i < 6; i++) {
        s32 k = (s32)((m >> (45 - 9 * i)) & 0x1FF);
        rdp.convert[i] = i < 4 ? 2 * sext(k, 9) + 1 : k;
      }
      break;
    }
    case 0x2D:
      rdp.sc_xlo = (w[0] >> 12) & 0xFFF; rdp.sc_ylo = w[0] & 0xFFF; rdp.sc_xhi = (w[1] >> 12) & 0xFFF; rdp.sc_yhi = w[1] & 0xFFF;
      SMASK(rdp.sflags, w[1] & (1 << 25), RS_INTERLACE_FIELD);
      SMASK(rdp.sflags, w[1] & (1 << 24), RS_INTERLACE_KEEP_ODD);
      rdp.state_dirty = 1;
      break;
    case 0x2E: rdp.prim_depth = (s32)(((w[1] >> 16) & 0x7FFF) << 16); rdp.prim_dz = w[1] & 0xFFFF; break;
    case 0x2F: {
      u32 f = rdp.sflags, d = rdp.dbflags;
      SMASK(f, w[0] & (1 << 19), RS_PERSPECTIVE); SMASK(f, w[0] & (1 << 18), RS_DETAIL); SMASK(f, w[0] & (1 << 17), RS_SHARPEN);
      SMASK(f, w[0] & (1 << 16), RS_TEX_LOD); SMASK(f, w[0] & (1 << 15), RS_TLUT); SMASK(f, w[0] & (1 << 14), RS_TLUT_TYPE);
      SMASK(f, w[0] & (1 << 13), RS_SAMPLE_QUAD); SMASK(f, w[0] & (1 << 12), RS_MID_TEXEL); SMASK(f, w[0] & (1 << 11), RS_BILERP0);
      SMASK(f, w[0] & (1 << 10), RS_BILERP1); SMASK(f, w[0] & (1 << 9), RS_CONVERT_ONE); SMASK(f, w[0] & (1 << 8), RS_KEY);
      SMASK(d, w[1] & (1 << 14), DB_FORCE_BLEND); SMASK(f, w[1] & (1 << 13), RS_ALPHA_CVG_SELECT); SMASK(f, w[1] & (1 << 12), RS_CVG_TIMES_ALPHA);
      SMASK(d, w[1] & (1 << 7), DB_COLOR_ON_CVG); SMASK(d, w[1] & (1 << 6), DB_IMAGE_READ); SMASK(d, w[1] & (1 << 5), DB_DEPTH_UPDATE);
      SMASK(d, w[1] & (1 << 4), DB_DEPTH_TEST); SMASK(f, w[1] & (1 << 3), RS_AA); SMASK(d, w[1] & (1 << 3), DB_AA);
      SMASK(f, w[1] & (1 << 1), RS_ALPHA_TEST_DITHER); SMASK(f, w[1] & (1 << 0), RS_ALPHA_TEST);
      rdp.dither = (w[0] >> 4) & 15;
      SMASK(d, (rdp.dither >> 2) != 3, DB_DITHER);
      rdp.cvg_mode = (w[1] >> 8) & 3; rdp.z_mode = (w[1] >> 10) & 3;
      f &= ~(u32)(RS_MULTI_CYCLE | RS_FILL | RS_COPY); d &= ~(u32)DB_MULTI_CYCLE;
      switch ((w[0] >> 20) & 3) { case 1: f |= RS_MULTI_CYCLE; d |= DB_MULTI_CYCLE; break; case 2: f |= RS_COPY; break; case 3: f |= RS_FILL; break; default: break; }
      for (int c = 0; c < 2; c++) {
        rdp.blend[c][0] = (w[1] >> (30 - 2 * c)) & 3; rdp.blend[c][1] = (w[1] >> (26 - 2 * c)) & 3;
        rdp.blend[c][2] = (w[1] >> (22 - 2 * c)) & 3; rdp.blend[c][3] = (w[1] >> (18 - 2 * c)) & 3;
      }
      rdp.sflags = f; rdp.dbflags = d;
      rdp.use_prim_depth = (w[1] >> 2) & 1;
      rdp.state_dirty = 1;
      break;
    }
    case 0x30: rdp_load((w[1] >> 24) & 7, (w[0] >> 12) & 0xFFF, w[0] & 0xFFF, (w[1] >> 12) & 0xFFF, w[1] & 0xFFF, LOAD_TLUT); break;
    case 0x32: {
      Tile *t = &rdp.tiles[(w[1] >> 24) & 7];
      t->slo = (w[0] >> 12) & 0xFFF; t->tlo = w[0] & 0xFFF; t->shi = (w[1] >> 12) & 0xFFF; t->thi = w[1] & 0xFFF;
      rdp.tiles_dirty = 1;
      break;
    }
    case 0x33: rdp_load((w[1] >> 24) & 7, (w[0] >> 12) & 0xFFF, w[0] & 0xFFF, (w[1] >> 12) & 0xFFF, w[1] & 0xFFF, LOAD_BLOCK); break;
    case 0x34: rdp_load((w[1] >> 24) & 7, (w[0] >> 12) & 0xFFF, w[0] & 0xFFF, (w[1] >> 12) & 0xFFF, w[1] & 0xFFF, LOAD_TILE); break;
    case 0x35: {
      Tile *t = &rdp.tiles[(w[1] >> 24) & 7];
      t->offset = (w[0] & 511) << 3; t->stride = ((w[0] >> 9) & 511) << 3;
      t->size = (w[0] >> 19) & 3; t->fmt = (w[0] >> 21) & 7;
      t->palette = (w[1] >> 20) & 15; t->shift_s = w[1] & 15; t->mask_s = (w[1] >> 4) & 15;
      t->shift_t = (w[1] >> 10) & 15; t->mask_t = (w[1] >> 14) & 15;
      t->flags = 0;
      if (w[1] & (1 << 8)) t->flags |= TILE_MIRROR_S;
      if (w[1] & (1 << 9)) t->flags |= TILE_CLAMP_S;
      if (w[1] & (1 << 18)) t->flags |= TILE_MIRROR_T;
      if (w[1] & (1 << 19)) t->flags |= TILE_CLAMP_T;
      if (t->mask_s > 10) t->mask_s = 10; else if (t->mask_s == 0) t->flags |= TILE_CLAMP_S;
      if (t->mask_t > 10) t->mask_t = 10; else if (t->mask_t == 0) t->flags |= TILE_CLAMP_T;
      rdp.tiles_dirty = 1;
      break;
    }
    case 0x37: rdp.fill_color = w[1]; break;
    case 0x38: rdp.fog = w[1]; break;
    case 0x39: rdp.blend_color = w[1]; break;
    case 0x3A: rdp.min_level = (w[0] >> 8) & 31; rdp.prim_lod_frac = w[0] & 0xFF; rdp.prim_color = w[1]; break;
    case 0x3B: rdp.env_color = w[1]; break;
    case 0x3C:
      rdp.comb[0][0] = (w[0] >> 20) & 15; rdp.comb[0][2] = (w[0] >> 15) & 31; rdp.comb[0][1] = (w[1] >> 28) & 15; rdp.comb[0][3] = (w[1] >> 15) & 7;
      rdp.comb[1][0] = (w[0] >> 12) & 7; rdp.comb[1][1] = (w[1] >> 12) & 7; rdp.comb[1][2] = (w[0] >> 9) & 7; rdp.comb[1][3] = (w[1] >> 9) & 7;
      rdp.comb[2][0] = (w[0] >> 5) & 15; rdp.comb[2][2] = w[0] & 31; rdp.comb[2][1] = (w[1] >> 24) & 15; rdp.comb[2][3] = (w[1] >> 6) & 7;
      rdp.comb[3][0] = (w[1] >> 21) & 7; rdp.comb[3][1] = (w[1] >> 3) & 7; rdp.comb[3][2] = (w[1] >> 18) & 7; rdp.comb[3][3] = w[1] & 7;
      rdp.state_dirty = 1;
      break;
    case 0x3D: rdp.ti_fmt = (w[0] >> 21) & 7; rdp.ti_size = (w[0] >> 19) & 3; rdp.ti_width = (w[0] & 0x3FF) + 1; rdp.ti_addr = w[1] & 0xFFFFFF; break;
    case 0x3E: if ((w[1] & 0xFFFFFF) != rdp.z_addr) { rdp_flush(); rdp.z_addr = w[1] & 0xFFFFFF; } break;
    case 0x3F: {
      u32 fmt = (w[0] >> 21) & 7, size = (w[0] >> 19) & 3, width = (w[0] & 1023) + 1, addr = w[1] & 0xFFFFFF;
      u32 fbfmt = size == 0 ? FB_I4 : size == 1 ? FB_I8 : size == 2 ? (fmt ? FB_IA88 : FB_5551) : FB_8888;
      if (addr != rdp.fb_addr || width != rdp.fb_width || fbfmt != rdp.fb_fmt) rdp_flush();
      rdp.fb_addr = addr; rdp.fb_width = width; rdp.fb_fmt = fbfmt;
      break;
    }
    default: break;
  }
}

void (*rdp_hook_pre)(void), (*rdp_hook_post)(void);
#ifdef RDP_ORACLE
void (*rdp_hook_command)(const u32 *, u32);
void (*rdp_hook_command_post)(void);
#endif
// GPU mode: does this texture load read memory whose current contents exist only on the GPU? If so the pending
// primitives are sent off, the host is asked to copy the results back, and command processing stops in front of
// the load until that has happened (rdp_resume).
static u32 rdp_paused;
u32 rdp_debug_words[4];
static int load_blocked(const u32 *w, u32 cmd) {
  u32 vsize = rdp.ti_size;
  if (vsize == 0) return 0;
  u32 slo = (w[0] >> 12) & 0xFFF, tlo = w[0] & 0xFFF, shi = (w[1] >> 12) & 0xFFF, thi = w[1] & 0xFFF;
  u32 vbytes = 1u << (vsize - 1), src, len;
  if (cmd == 0x33) {
    u32 texels = (shi - slo + 1) & 0xFFF;
    src = rdp.ti_addr + (rdp.ti_width * tlo + slo) * vbytes;
    len = ((texels * vbytes + 7) >> 3) * 8;
  } else if (cmd == 0x34) {
    if ((thi >> 2) < (tlo >> 2)) return 0;
    u32 texels = (((shi >> 2) - (slo >> 2)) + 1) & 0xFFF, lines = ((thi >> 2) - (tlo >> 2)) + 1;
    src = rdp.ti_addr + (rdp.ti_width * (tlo >> 2) + (slo >> 2)) * vbytes;
    len = (lines - 1) * rdp.ti_width * vbytes + ((texels * vbytes + 7) >> 3) * 8;
  } else {
    src = rdp.ti_addr + ((rdp.ti_width * (tlo >> 2) + (slo >> 2)) << (vsize - 1));
    len = 0x1000;     // (generous: a palette load reads at most a few hundred entries)
  }
  src &= RDRAM_MAX - 1;
  if (!len) return 0;
  if (batch_reads_pending(src, len)) rdp_flush();
  if (!gpu_stale_n || !gpu_range_stale(src, len)) return 0;
  gpu_request_sync(); gpu_sync_cause[0] = 4; gpu_sync_cause[1] = src; gpu_sync_cause[2] = len; gpu_sync_cause[3] = cmd;
  return 1;
}

static void rdp_exec_buffered(void) {
  u32 pos = 0;
  while (pos < rdp.cmd_n) {
    u32 cmd = (rdp.cmd[pos] >> 24) & 63;
    u32 len = rdp_len[cmd] * 2;
    if (pos + len > rdp.cmd_n) break;
    int draw = (cmd >= 8 && cmd <= 15) || cmd == 0x24 || cmd == 0x25 || cmd == 0x36;
    if (rdp_gpu_mode && draw && rdp.fb_fmt < FB_5551 && gpu_stale_n) {
      rdp_flush(); gpu_request_sync(); rdp_paused = 1; break;
    }
    if (rdp_gpu_mode && (cmd == 0x30 || cmd == 0x33 || cmd == 0x34) && load_blocked(rdp.cmd + pos, cmd)) { rdp_paused = 1; break; }
#ifdef RDP_ORACLE
    if (rdp_hook_command) rdp_hook_command(rdp.cmd + pos, len);
#endif
    rdp_exec(rdp.cmd + pos);
#ifdef RDP_ORACLE
    if (rdp_hook_command_post) rdp_hook_command_post();
#endif
    pos += len;
  }
  if (pos) { memmove(rdp.cmd, rdp.cmd + pos, (rdp.cmd_n - pos) * 4); rdp.cmd_n -= pos; }
}

void rdp_process(void) {
  u32 cur = sys.dp_current, end = sys.dp_end;
  if (end <= cur) return;
  if (rdp_hook_pre) rdp_hook_pre();
  u32 n = (end - cur) >> 2;
  if (rdp.cmd_n + n > 0x10000) n = 0x10000 - rdp.cmd_n;
  for (u32 i = 0; i < n; i++) {
    u32 a = cur + i * 4;
    rdp.cmd[rdp.cmd_n++] = (sys.dp_status & 1) ? *(u32 *)(spmem + (a & 0x1FFC)) : (a < sys.rdram_size ? RDRAM32(a) : 0);
  }
  sys.dp_current = cur + n * 4;  // (== end unless the FIFO clamp dropped the tail, which stays queued for next time)
  if (!rdp_paused) rdp_exec_buffered();      // (while stopped, new commands just queue up behind the load)
  if (rdp_hook_post) rdp_hook_post();
}

// The host has copied the GPU's results back: carry on with the commands that were held up.
void rdp_resume(void) {
  rdp_debug_words[0] = rdp_paused; rdp_debug_words[1] = rdp.cmd_n; rdp_debug_words[2] = rdp.cmd_n ? rdp.cmd[0] : 0;
  if (!rdp_paused) return;
  rdp_paused = 0;
  rdp_exec_buffered();
}

// hardware-accurate blender divider table (also uploaded to the GPU, so it must exist before any reset)
void rdp_init_tables(void) {
  static int done;
  if (done) return;
  done = 1;
  for (int i = 0; i < 0x8000; i++) {
    int res = 0, d = (i >> 11) & 0xF, n = i & 0x7FF, invd = (~d) & 0xF;
    int temp = invd + (n >> 8) + 1;
    int ps[9];
    ps[0] = temp & 7;
    for (int k = 0; k < 8; k++) {
      int nbit = (n >> (7 - k)) & 1;
      if (res & (0x100 >> k)) temp = invd + (ps[k] << 1) + nbit + 1;
      else temp = d + (ps[k] << 1) + nbit;
      ps[k + 1] = temp & 7;
      if (temp & 0x10) res |= 1 << (7 - k);
    }
    blender_lut[i] = (u8)res;
  }
}

void rdp_reset(void) {
  rdp_paused = 0;
  memset(pipeline_feedback, 0, sizeof pipeline_feedback); gpu_feedback_dirty = 0;
  memset(&rdp, 0, sizeof rdp);
  memset(&b_info, 0, sizeof b_info);
  memset(rdp_hidden, 0, sizeof rdp_hidden);
  memset(rdp_shadow16, 0, sizeof rdp_shadow16);
  rdp.state_dirty = rdp.tiles_dirty = rdp.tmem_dirty = 1;
  rdp.fb_fmt = FB_5551; rdp.fb_width = 320;
  rdp_init_tables();
}
