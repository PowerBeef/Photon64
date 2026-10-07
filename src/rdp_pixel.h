// Software pixel pipeline. Written to mirror the WGSL compute shader one-to-one:
// integer-only helpers operating on the batch buffers.

static s32 px_color[4]; static int px_color_dirty;
static u32 px_depth, px_dz; static int px_depth_dirty;
static u32 px_fb_index;
static u32 px_noise;
#ifdef RDP_DEBUG
int rdp_dbg_on, rdp_dbg_x, rdp_dbg_y;
#endif

static inline s32 clampi(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline s32 mini(s32 a, s32 b) { return a < b ? a : b; }
static inline s32 maxi(s32 a, s32 b) { return a > b ? a : b; }
static inline s32 msb(s32 v) { return v > 0 ? 31 - __builtin_clz((u32)v) : -1; }
static inline s32 lsb(u32 v) { return v ? __builtin_ctz(v) : -1; }
static inline s32 sx(s32 v, int bits) { return (s32)((u32)v << (32 - bits)) >> (32 - bits); }
static inline s32 clamp9(s32 c) { return clampi(sx(c - 0x80, 9) + 0x80, 0, 0xFF); }
static inline s32 clamp_z(s32 z) { z -= 1 << 17; z = sx(z, 19); z += 1 << 17; return clampi(z, 0, 0x3FFFF); }

// ---- noise ------------------------------------------------------------------
static inline void reseed_noise(u32 x, u32 y, u32 off) {
#ifdef NOISE_ZERO   // (differential tests against a reference renderer: both sides read zero from every noise source)
  (void)x; (void)y; (void)off; px_noise = 0; return;
#endif
  const u32 NP = 1103515245u;
  u32 s0 = x, s1 = y, s2 = off, t0, t1, t2;
  for (int i = 0; i < 3; i++) {
    t0 = ((s0 >> 8) ^ s1) * NP; t1 = ((s1 >> 8) ^ s2) * NP; t2 = ((s2 >> 8) ^ s0) * NP;
    s0 = t0; s1 = t1; s2 = t2;
  }
  px_noise = (s0 >> 16) & 0xFFFF;
}

// ---- coverage ---------------------------------------------------------------
static inline u32 compute_coverage(const u32 *sp, s32 x) {
  u32 xl0 = sp[0] & 0xFFFF, xl1 = sp[0] >> 16, xl2 = sp[1] & 0xFFFF, xl3 = sp[1] >> 16;
  u32 xr0 = sp[2] & 0xFFFF, xr1 = sp[2] >> 16, xr2 = sp[3] & 0xFFFF, xr3 = sp[3] >> 16;
  u32 b = ((u32)x << 3) & 0xFFFF;
  u32 s0 = b, s4 = (b + 4) & 0xFFFF, s2 = (b + 2) & 0xFFFF, s6 = (b + 6) & 0xFFFF;
  u32 c = 0;
  if (s0 >= xl0 && s0 < xr0) c |= 1;
  if (s4 >= xl0 && s4 < xr0) c |= 2;
  if (s2 >= xl1 && s2 < xr1) c |= 4;
  if (s6 >= xl1 && s6 < xr1) c |= 8;
  if (s0 >= xl2 && s0 < xr2) c |= 16;
  if (s4 >= xl2 && s4 < xr2) c |= 32;
  if (s2 >= xl3 && s2 < xr3) c |= 64;
  if (s6 >= xl3 && s6 < xr3) c |= 128;
  return c;
}

// ---- perspective ------------------------------------------------------------
static const s16 persp_table[64][2] = {
  {0x4000, -252 * 4}, {0x3f04, -244 * 4}, {0x3e10, -238 * 4}, {0x3d22, -230 * 4}, {0x3c3c, -223 * 4}, {0x3b5d, -218 * 4}, {0x3a83, -210 * 4}, {0x39b1, -205 * 4},
  {0x38e4, -200 * 4}, {0x381c, -194 * 4}, {0x375a, -189 * 4}, {0x369d, -184 * 4}, {0x35e5, -179 * 4}, {0x3532, -175 * 4}, {0x3483, -170 * 4}, {0x33d9, -166 * 4},
  {0x3333, -162 * 4}, {0x3291, -157 * 4}, {0x31f4, -155 * 4}, {0x3159, -150 * 4}, {0x30c3, -147 * 4}, {0x3030, -143 * 4}, {0x2fa1, -140 * 4}, {0x2f15, -137 * 4},
  {0x2e8c, -134 * 4}, {0x2e06, -131 * 4}, {0x2d83, -128 * 4}, {0x2d03, -125 * 4}, {0x2c86, -123 * 4}, {0x2c0b, -120 * 4}, {0x2b93, -117 * 4}, {0x2b1e, -115 * 4},
  {0x2aab, -113 * 4}, {0x2a3a, -110 * 4}, {0x29cc, -108 * 4}, {0x2960, -106 * 4}, {0x28f6, -104 * 4}, {0x288e, -102 * 4}, {0x2828, -100 * 4}, {0x27c4, -98 * 4},
  {0x2762, -96 * 4}, {0x2702, -94 * 4}, {0x26a4, -92 * 4}, {0x2648, -91 * 4}, {0x25ed, -89 * 4}, {0x2594, -87 * 4}, {0x253d, -86 * 4}, {0x24e7, -85 * 4},
  {0x2492, -83 * 4}, {0x243f, -81 * 4}, {0x23ee, -80 * 4}, {0x239e, -79 * 4}, {0x234f, -77 * 4}, {0x2302, -76 * 4}, {0x22b6, -74 * 4}, {0x226c, -74 * 4},
  {0x2222, -72 * 4}, {0x21da, -71 * 4}, {0x2193, -70 * 4}, {0x214d, -69 * 4}, {0x2108, -67 * 4}, {0x20c5, -67 * 4}, {0x2082, -65 * 4}, {0x2041, -65 * 4} };

// st[2] in/out; returns overflow flag
static inline int perspective_divide(s32 s, s32 t, s32 w, s32 *os, s32 *ot) {
  int overflow = 0;
  int w_carry = w <= 0;
  w &= 0x7FFF;
  s32 shift = mini(14 - msb(w), 14);
  s32 normout = (w << shift) & 0x3FFF;
  s32 wnorm = normout & 0xFF;
  s32 rcp = ((persp_table[normout >> 8][1] * wnorm) >> 10) + persp_table[normout >> 8][0];
  s32 prod[2] = { s * rcp, t * rcp };
  s32 temp_mask = ((1 << 30) - 1) & -((1 << 29) >> shift);
  s32 oob[2] = { prod[0] & temp_mask, prod[1] & temp_mask };
  s32 temp[2];
  if (shift != 14) { prod[0] >>= (13 - shift); prod[1] >>= (13 - shift); temp[0] = prod[0]; temp[1] = prod[1]; }
  else { temp[0] = (s32)((u32)prod[0] << 1); temp[1] = (s32)((u32)prod[1] << 1); }
  for (int i = 0; i < 2; i++) {
    if (oob[i] != temp_mask && oob[i] != 0) {
      temp[i] = (prod[i] & (1 << 29)) == 0 ? 0x7FFF : -0x8000;
      overflow = 1;
    }
  }
  if (w_carry) { temp[0] = temp[1] = 0x7FFF; overflow = 1; }
  *os = clampi(temp[0], -0x10000, 0xFFFF);
  *ot = clampi(temp[1], -0x10000, 0xFFFF);
  return overflow;
}

// ---- texture ----------------------------------------------------------------
#define TL_SLO(T) ((s32)(T)[0])
#define TL_SHI(T) ((s32)(T)[1])
#define TL_TLO(T) ((s32)(T)[2])
#define TL_THI(T) ((s32)(T)[3])
#define TL_OFFSET(T) ((T)[4] & 0xFFFF)
#define TL_STRIDE(T) ((T)[4] >> 16)
#define TL_FMT(T) ((T)[5] & 0xFF)
#define TL_SIZE(T) (((T)[5] >> 8) & 0xFF)
#define TL_PAL(T) (((T)[5] >> 16) & 0xFF)
#define TL_FLAGS(T) ((T)[5] >> 24)
#define TL_MASK_S(T) ((s32)((T)[6] & 0xFF))
#define TL_SHIFT_S(T) ((s32)(((T)[6] >> 8) & 0xFF))
#define TL_MASK_T(T) ((s32)(((T)[6] >> 16) & 0xFF))
#define TL_SHIFT_T(T) ((s32)((T)[6] >> 24))
#define TM8(TM, i) (((const u8 *)(TM))[i])
#define TM16(TM, i) (((const u16 *)(TM))[i])

static inline s32 texel_mask(s32 c, s32 maskbits, int mirror) {
  if (maskbits != 0) {
    s32 mask = 1 << maskbits;
    if (mirror) c ^= maxi((c & mask) - 1, 0);
    c &= mask - 1;
  }
  return c;
}

enum { TX_RGBA4, TX_IA4, TX_CI4, TX_CI4_TLUT, TX_CI8_TLUT, TX_CI32, TX_CI32_TLUT, TX_RGBA8, TX_IA8, TX_YUV16, TX_RGBA16, TX_IA16, TX_RGBA32 };

static inline void conv_rgba16(u32 w, s32 *o) {
  u32 r = (w >> 11) & 31, g = (w >> 6) & 31, b = (w >> 1) & 31;
  o[0] = (r << 3) | (r >> 2); o[1] = (g << 3) | (g >> 2); o[2] = (b << 3) | (b >> 2); o[3] = (w & 1) * 0xFF;
}
static inline void conv_ia16(u32 w, s32 *o) { o[0] = o[1] = o[2] = w >> 8; o[3] = w & 0xFF; }

static inline void texel_fetch(const u32 *T, const u32 *TM, int kind, s32 s, s32 t, u32 lut_offset, u32 addr_xor, int tlut_type, u32 chroma_x, s32 *o) {
  u32 us = (u32)s, ut = (u32)t;
  u32 base = TL_OFFSET(T) + TL_STRIDE(T) * ut;
  u32 pal = TL_PAL(T);
  switch (kind) {
    case TX_RGBA4: case TX_IA4: case TX_CI4: case TX_CI4_TLUT: {
      u32 bo = (base + (us >> 1)) & (kind == TX_CI4_TLUT ? 0x7FF : 0xFFF);
      u32 shift = (~us & 1) * 4;
      u32 idx = (bo ^ ((ut & 1) << 2)) ^ 3;
      u32 w = (TM8(TM, idx) >> shift) & 0xF;
      if (kind == TX_RGBA4) { w |= w << 4; o[0] = o[1] = o[2] = o[3] = w; }
      else if (kind == TX_IA4) { u32 in = w & 0xE; in = (in << 4) | (in << 1) | (in >> 2); o[0] = o[1] = o[2] = in; o[3] = (w & 1) * 0xFF; }
      else if (kind == TX_CI4) { w |= pal << 4; o[0] = o[1] = o[2] = o[3] = w; }
      else {
        w |= pal << 4;
        u32 le = ((w << 2) + lut_offset) ^ addr_xor;
        w = TM16(TM, 0x400 | le);
        if (tlut_type) conv_ia16(w, o); else conv_rgba16(w, o);
      }
      break;
    }
    case TX_CI8_TLUT: {
      u32 bo = (base + us) & 0x7FF;
      u32 idx = (bo ^ ((ut & 1) << 2)) ^ 3;
      u32 w = TM8(TM, idx);
      u32 le = ((w << 2) + lut_offset) ^ addr_xor;
      w = TM16(TM, 0x400 | le);
      if (tlut_type) conv_ia16(w, o); else conv_rgba16(w, o);
      break;
    }
    case TX_CI32: case TX_RGBA16: case TX_IA16: {
      u32 bo = (base + us * 2) & 0xFFF;
      u32 idx = ((bo >> 1) ^ ((ut & 1) << 1)) ^ 1;
      u32 w = TM16(TM, idx);
      if (kind == TX_CI32) { o[0] = o[2] = w >> 8; o[1] = o[3] = w & 0xFF; }
      else if (kind == TX_RGBA16) conv_rgba16(w, o);
      else conv_ia16(w, o);
      break;
    }
    case TX_CI32_TLUT: {
      u32 bo = (base + us * 2) & 0x7FF;
      u32 idx = ((bo >> 1) ^ ((ut & 1) << 1)) ^ 1;
      u32 w = TM16(TM, idx);
      u32 le = (((w >> 6) & ~3u) + lut_offset) ^ addr_xor;
      w = TM16(TM, 0x400 | le);
      if (tlut_type) conv_ia16(w, o); else conv_rgba16(w, o);
      break;
    }
    case TX_RGBA8: case TX_IA8: {
      u32 bo = (base + us) & 0xFFF;
      u32 idx = (bo ^ ((ut & 1) << 2)) ^ 3;
      u32 w = TM8(TM, idx);
      if (kind == TX_RGBA8) { o[0] = o[1] = o[2] = o[3] = w; }
      else { u32 in = w >> 4, al = w & 0xF; al |= al << 4; in |= in << 4; o[0] = o[1] = o[2] = in; o[3] = al; }
      break;
    }
    case TX_YUV16: {
      u32 bl = (base + us) & 0x7FF, bc = (base + chroma_x * 2) & 0x7FF;
      u32 il = (bl ^ ((ut & 1) << 2)) ^ 3;
      u32 ic = ((bc >> 1) ^ ((ut & 1) << 1)) ^ 1;
      u32 luma = TM8(TM, il | 0x800);
      u32 chroma = TM16(TM, ic);
      o[0] = (s32)((chroma >> 8) & 0xFF) - 0x80; o[1] = (s32)(chroma & 0xFF) - 0x80; o[2] = o[3] = luma;
      break;
    }
    default: {   // TX_RGBA32
      u32 bo = (base + us * 2) & 0x7FF;
      u32 idx = ((bo >> 1) ^ ((ut & 1) << 1)) ^ 1;
      u32 lw = TM16(TM, idx), uw = TM16(TM, idx | 0x400);
      o[0] = lw >> 8; o[1] = lw & 0xFF; o[2] = uw >> 8; o[3] = uw & 0xFF;
      break;
    }
  }
}

static inline s32 shift_coord(s32 coord, s32 lo, s32 shift) {
  coord = clampi(coord, -0x8000, 0x7FFF);
  if (shift < 11) coord >>= shift;
  else { coord = (s32)((u32)coord << (32 - shift)); coord >>= 16; }
  return coord - (lo << 3);
}
static inline s32 clamp_and_shift_coord(int clamp_bit, s32 coord, s32 lo, s32 hi, s32 shift) {
  coord = clampi(coord, -0x8000, 0x7FFF);
  if (shift < 11) coord >>= shift;
  else { coord = (s32)((u32)coord << (32 - shift)); coord >>= 16; }
  if (clamp_bit) {
    if ((coord >> 3) >= hi) coord = (((hi >> 2) - (lo >> 2)) & 0x3FF) << 5;
    else coord = maxi(coord - (lo << 3), 0);
  } else coord -= lo << 3;
  return coord;
}

static inline void texture_convert_factors(const s32 *in, const s32 *k, s32 *o) {
  s32 r = sx(in[0], 9), g = sx(in[1], 9), b = sx(in[2], 9);
  o[0] = b + ((k[0] * g + 0x80) >> 8);
  o[1] = b + ((k[1] * r + k[2] * g + 0x80) >> 8);
  o[2] = b + ((k[3] * r + 0x80) >> 8);
  o[3] = b;
}

static inline void bilinear_3tap2(const s32 *t00, const s32 *t10, const s32 *t01, const s32 *t11, s32 fx, s32 fy, s32 *o) {
  int up = (fx + fy) >= 32;
  const s32 *tb = up ? t11 : t00;
  s32 ffx = up ? 32 - fy : fx, ffy = up ? 32 - fx : fy;
  for (int i = 0; i < 2; i++) o[i] = (((t10[i] - tb[i]) * ffx + (t01[i] - tb[i]) * ffy + 0x10) >> 5) + tb[i];
}

static void sample_texture(const u32 *T, const u32 *TM, s32 st_s, s32 st_t, int tlut, int tlut_type, int sample_quad, int mid_texel_state,
                           int convert_one, int bilerp, const s32 *factors, const s32 *prev_cycle, s32 *accum) {
  u32 tflags = TL_FLAGS(T);
  s32 cs = clamp_and_shift_coord((tflags & TILE_CLAMP_S) != 0, st_s, TL_SLO(T), TL_SHI(T), TL_SHIFT_S(T));
  s32 ct = clamp_and_shift_coord((tflags & TILE_CLAMP_T) != 0, st_t, TL_TLO(T), TL_THI(T), TL_SHIFT_T(T));
  s32 fx = 0, fy = 0;
  if (sample_quad || tlut) { fx = cs & 31; fy = ct & 31; }
  s32 sum_frac = fx + fy;
  cs >>= 5; ct >>= 5;
  s32 s0 = texel_mask(cs, TL_MASK_S(T), (tflags & TILE_MIRROR_S) != 0);
  s32 t0 = texel_mask(ct, TL_MASK_T(T), (tflags & TILE_MIRROR_T) != 0);
  s32 s1 = texel_mask(cs + 1, TL_MASK_S(T), (tflags & TILE_MIRROR_S) != 0);
  s32 t1 = texel_mask(ct + 1, TL_MASK_T(T), (tflags & TILE_MIRROR_T) != 0);
  s32 tdiff = maxi(t1 - t0, -255);
  t1 = (t0 & 0xFF) + tdiff;
  t0 &= 0xFF;
  s32 tb[4] = {0, 0, 0, 0}, t10[4] = {0, 0, 0, 0}, t01[4] = {0, 0, 0, 0}, t11[4] = {0, 0, 0, 0};
  int mid_texel = mid_texel_state && bilerp && fx == 0x10 && fy == 0x10;
  int upper_lut = sum_frac >= 0x20;
  if (mid_texel) sum_frac = 0;
  u32 fmt = TL_FMT(T), size = TL_SIZE(T);
  int yuv = fmt == 1;
  s32 bs = sum_frac >= 0x20 ? s1 : s0, bt = sum_frac >= 0x20 ? t1 : t0;
  s32 chroma_frac = ((s0 & 1) << 4) | (fx >> 1);
  if (tlut) {
    if (!sample_quad) { bs = s0; bt = t0; s1 = s0; t1 = t0; }
    int upper = sum_frac >= 0x20;
    u32 ax = upper_lut ? 2 : 1;
    int kind = size == 0 ? TX_CI4_TLUT : size == 1 ? TX_CI8_TLUT : TX_CI32_TLUT;
    texel_fetch(T, TM, kind, bs, bt, upper ? 3 : 0, ax, tlut_type, 0, tb);
    if (bilerp) {
      texel_fetch(T, TM, kind, s1, t0, 1, ax, tlut_type, 0, t10);
      texel_fetch(T, TM, kind, s0, t1, 2, ax, tlut_type, 0, t01);
    }
    if (mid_texel) texel_fetch(T, TM, kind, s1, t1, 3, ax, tlut_type, 0, t11);
  } else if (yuv) {
    u32 cx0 = (u32)s0 >> 1, cx1 = (u32)(s1 + (s1 - s0)) >> 1;
    texel_fetch(T, TM, TX_YUV16, s0, t0, 0, 0, 0, cx0, tb);
    if (sample_quad) {
      texel_fetch(T, TM, TX_YUV16, s1, t0, 0, 0, 0, cx1, t10);
      texel_fetch(T, TM, TX_YUV16, s0, t1, 0, 0, 0, cx0, t01);
      texel_fetch(T, TM, TX_YUV16, s1, t1, 0, 0, 0, cx1, t11);
    }
  } else {
    int kind;
    switch (fmt) {
      case 0: kind = size == 0 ? TX_RGBA4 : size == 1 ? TX_RGBA8 : size == 2 ? TX_RGBA16 : TX_RGBA32; break;
      case 2: kind = size == 0 ? TX_CI4 : size == 1 ? TX_RGBA8 : TX_CI32; break;
      case 3: kind = size == 0 ? TX_IA4 : size == 1 ? TX_IA8 : size == 2 ? TX_IA16 : TX_CI32; break;
      default: kind = size == 0 ? TX_RGBA4 : size == 1 ? TX_RGBA8 : TX_CI32; break;
    }
    texel_fetch(T, TM, kind, bs, bt, 0, 0, 0, 0, tb);
    if (sample_quad) {
      texel_fetch(T, TM, kind, s1, t0, 0, 0, 0, 0, t10);
      texel_fetch(T, TM, kind, s0, t1, 0, 0, 0, 0, t01);
    }
    if (mid_texel) texel_fetch(T, TM, kind, s1, t1, 0, 0, 0, 0, t11);
  }

  if (convert_one) {
    s32 ps[3] = { sx(prev_cycle[0], 9), sx(prev_cycle[1], 9), sx(prev_cycle[2], 9) };
    if (sample_quad) {
      int mid_rg = yuv ? (mid_texel_state && chroma_frac == 0x10 && fy == 0x10) : mid_texel;
      int mid_ba = mid_texel;
      int upper_ba = sum_frac >= 32;
      int upper_rg = yuv ? ((chroma_frac + fy) >= 32 && !mid_rg) : upper_ba;
      s32 conv[4];
      for (int h = 0; h < 2; h++) {       // h=0: rg pair, h=1: ba pair
        int mid = h ? mid_ba : mid_rg, upper = h ? upper_ba : upper_rg;
        s32 f0 = upper ? ps[1] : ps[0], f1 = upper ? ps[0] : ps[1];
        for (int c = 0; c < 2; c++) {
          int i = h * 2 + c;
          if (mid) conv[i] = f0 * (t01[i] - t11[i]) + f1 * (t10[i] - t11[i]) + (s32)((u32)(tb[i] - t11[i]) << 6) + 0x80;
          else { s32 b = (upper && yuv) ? t11[i] : tb[i]; conv[i] = f0 * (t10[i] - b) + f1 * (t01[i] - b) + 0x80; }
        }
      }
      for (int i = 0; i < 4; i++) accum[i] = (s32)(s16)((conv[i] >> 8) + ps[2]);
    } else accum[0] = accum[1] = accum[2] = accum[3] = ps[2];
  } else if (yuv) {
    if (sample_quad) {
      if (bilerp) {
        int mid_chroma = mid_texel_state && chroma_frac == 0x10 && fy == 0x10;
        if (mid_chroma) for (int i = 0; i < 2; i++) accum[i] = (tb[i] + t10[i] + t11[i] + t01[i] + 2) >> 2;
        else bilinear_3tap2(tb, t10, t01, t11, chroma_frac, fy, accum);
        if (mid_texel) for (int i = 2; i < 4; i++) accum[i] = (tb[i] + t10[i] + t11[i] + t01[i] + 2) >> 2;
        else bilinear_3tap2(tb + 2, t10 + 2, t01 + 2, t11 + 2, fx, fy, accum + 2);
      } else {
        const s32 *l = (fx + fy >= 32) ? t11 : tb, *c = (chroma_frac + fy >= 32) ? t11 : tb;
        accum[0] = c[0]; accum[1] = c[1]; accum[2] = l[2]; accum[3] = l[3];
      }
    } else for (int i = 0; i < 4; i++) accum[i] = tb[i];
  } else if (mid_texel) {
    for (int i = 0; i < 4; i++) accum[i] = (tb[i] + t01[i] + t10[i] + t11[i] + 2) >> 2;
  } else if (bilerp && (sample_quad || tlut)) {
    s32 ffx = sum_frac >= 32 ? 32 - fy : fx, ffy = sum_frac >= 32 ? 32 - fx : fy;
    for (int i = 0; i < 4; i++) accum[i] = (((t10[i] - tb[i]) * ffx + (t01[i] - tb[i]) * ffy + 0x10) >> 5) + tb[i];
  } else for (int i = 0; i < 4; i++) accum[i] = tb[i];

  if (!bilerp && !convert_one) { s32 tmp[4] = { accum[0], accum[1], accum[2], accum[3] }; texture_convert_factors(tmp, factors, accum); }
}

static inline s32 sample_texture_copy_word(const u32 *T, const u32 *TM, s32 s, s32 t, s32 s_offset, int tlut) {
  u32 size = TL_SIZE(T), tflags = TL_FLAGS(T);
  int high_word = s_offset < 2;
  int replicate_8bpp = high_word && size != 2 && !tlut;
  s32 samp;
  s32 s_shamt = mini((s32)size, 2);
  int large_texel = size == 3;
  u32 idx_mask = (large_texel || tlut) ? 0x3FF : 0x7FF;
  if (replicate_8bpp) {
    s += 2 * s_offset;
    s32 ms0 = texel_mask(s, TL_MASK_S(T), (tflags & TILE_MIRROR_S) != 0);
    s32 ms1 = texel_mask(s + 1, TL_MASK_S(T), (tflags & TILE_MIRROR_S) != 0);
    s32 mt = texel_mask(t, TL_MASK_T(T), (tflags & TILE_MIRROR_T) != 0);
    u32 tbase = TL_OFFSET(T) + TL_STRIDE(T) * (u32)mt;
    u32 n0 = ((tbase * 2 + ((u32)ms0 << s_shamt)) & 0x1FFF) ^ (((u32)mt & 1) * 8);
    u32 n1 = ((tbase * 2 + ((u32)ms1 << s_shamt)) & 0x1FFF) ^ (((u32)mt & 1) * 8);
    s32 samp0 = TM16(TM, ((n0 >> 2) & idx_mask) ^ 1), samp1 = TM16(TM, ((n1 >> 2) & idx_mask) ^ 1);
    if (size == 1) {
      samp0 = (samp0 >> (8 - 4 * (s32)(n0 & 2))) & 0xFF; samp1 = (samp1 >> (8 - 4 * (s32)(n1 & 2))) & 0xFF;
    } else if (size == 0) {
      samp0 = ((samp0 >> (12 - 4 * (s32)(n0 & 3))) & 0xF) * 0x11; samp1 = ((samp1 >> (12 - 4 * (s32)(n1 & 3))) & 0xF) * 0x11;
    } else { samp0 >>= 8; samp1 >>= 8; }
    samp = (samp0 << 8) | samp1;
  } else {
    s += s_offset;
    s32 ms = texel_mask(s, TL_MASK_S(T), (tflags & TILE_MIRROR_S) != 0);
    s32 mt = texel_mask(t, TL_MASK_T(T), (tflags & TILE_MIRROR_T) != 0);
    u32 tbase = TL_OFFSET(T) + TL_STRIDE(T) * (u32)mt;
    u32 n = ((tbase * 2 + ((u32)ms << s_shamt)) & 0x1FFF) ^ (((u32)mt & 1) * 8);
    samp = TM16(TM, ((n >> 2) & idx_mask) ^ 1);
    if (tlut) {
      if (size == 0) { samp = (samp >> (12 - 4 * (s32)(n & 3))) & 0xF; samp |= TL_PAL(T) << 4; samp = (samp << 2) + s_offset; }
      else { samp = (samp >> (8 - 4 * (s32)(n & 2))) & 0xFF; samp = (samp << 2) + s_offset; }
      samp = TM16(TM, ((u32)(samp | 0x400) ^ 1) & 0x7FF);
    }
  }
  return samp;
}

static inline s32 sample_texture_copy(const u32 *T, const u32 *TM, s32 s, s32 t, s32 s_offset, int tlut) {
  s = shift_coord(s, TL_SLO(T), TL_SHIFT_S(T)) >> 5;
  t = shift_coord(t, TL_TLO(T), TL_SHIFT_T(T)) >> 5;
  if (b_info.fb_size == 0) return 0;
  if (b_info.fb_size == 1) {
    s32 samp = sample_texture_copy_word(T, TM, s, t, s_offset >> 1, tlut);
    return (samp >> (8 - 8 * (s_offset & 1))) & 0xFF;
  }
  return sample_texture_copy_word(T, TM, s, t, s_offset, tlut);
}

static inline void compute_lod_2cycle(u32 *tile0, u32 *tile1, s32 *lod_frac, u32 max_level, s32 min_lod, const s32 *st, const s32 *st_dx, const s32 *st_dy,
                                      int persp_overflow, int tex_lod_en, int sharpen, int detail) {
  int magnify = 0, distant = 0;
  u32 tile_offset = 0;
  if (persp_overflow) { distant = 1; *lod_frac = 0xFF; }
  else {
    s32 dx0 = st_dx[0] - st[0], dx1 = st_dx[1] - st[1], dy0 = st_dy[0] - st[0], dy1 = st_dy[1] - st[1];
    dx0 ^= dx0 >> 31; dx1 ^= dx1 >> 31; dy0 ^= dy0 >> 31; dy1 ^= dy1 >> 31;
    s32 max_d = maxi(maxi(dx0, dy0), maxi(dx1, dy1));
    if (max_d >= 0x4000) { distant = 1; *lod_frac = 0xFF; tile_offset = max_level; }
    else if (max_d < 32) {
      distant = max_level == 0; magnify = 1;
      if (!sharpen && !detail) *lod_frac = distant ? 0xFF : 0;
      else *lod_frac = (s32)(s16)((maxi(min_lod, max_d) << 3) + (sharpen ? -0x100 : 0));
    } else {
      s32 mip_base = maxi(msb(max_d >> 5), 0);
      distant = (u32)mip_base >= max_level;
      if (distant && !sharpen && !detail) *lod_frac = 0xFF;
      else { *lod_frac = ((max_d << 3) >> mip_base) & 0xFF; tile_offset = mip_base; }
    }
  }
  if (tex_lod_en) {
    if (distant) tile_offset = max_level;
    if (!detail) {
      *tile0 = (*tile0 + tile_offset) & 7;
      if (distant || (!sharpen && magnify)) *tile1 = *tile0; else *tile1 = (*tile0 + 1) & 7;
    } else {
      *tile1 = (*tile0 + tile_offset + ((distant || magnify) ? 1 : 2)) & 7;
      *tile0 = (*tile0 + tile_offset + (magnify ? 0 : 1)) & 7;
    }
  }
}

// ---- dither -----------------------------------------------------------------
static const u8 dither_matrices[2][16] = {
  { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 },
  { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 } };

static inline void dither_coefficients(s32 x, s32 y, s32 mode_rgb, s32 mode_alpha, s32 *rgb_d, s32 *alpha_d) {
  if (mode_rgb < 2) *rgb_d = dither_matrices[mode_rgb][(y & 3) * 4 + (x & 3)] * 0x49;
  else if (mode_rgb == 2) *rgb_d = px_noise & 0x1FF;
  else *rgb_d = 0;
  if (mode_alpha == 3) *alpha_d = 0;
  else if (mode_alpha == 2) *alpha_d = px_noise & 7;
  else {
    *alpha_d = mode_rgb >= 2 ? dither_matrices[mode_rgb & 1][(y & 3) * 4 + (x & 3)] : (*rgb_d & 7);
    if (mode_alpha == 1) *alpha_d = ~*alpha_d & 7;
  }
}
static inline void rgb_dither(s32 *rgb, s32 dith) {
  for (int i = 0; i < 3; i++) {
    s32 o = rgb[i], d = (dith >> (3 * i)) & 7;
    s32 r = o > 247 ? 255 : (o & 0xF8) + 8;
    s32 replace_sign = (d - (o & 7)) >> 31;
    rgb[i] = (o + ((r - o) & replace_sign)) & 0xFF;
  }
}

// ---- combiner ---------------------------------------------------------------
typedef struct { u32 c_muladd, c_mulsub, c_mul, c_add; s32 shade[4], combined[4], texel0[4], texel1[4]; s32 lod_frac, noise; } CombIn;
#define CR(c) ((s32)((c) >> 24))
#define CG(c) ((s32)(((c) >> 16) & 0xFF))
#define CB(c) ((s32)(((c) >> 8) & 0xFF))
#define CA(c) ((s32)((c) & 0xFF))

static inline void combiner_equation(const CombIn *in, u32 sel_rgb, u32 sel_alpha, s32 *out) {
  s32 a[4], b[4], c[4], d[4];
  u32 m;
  // muladd
  m = sel_rgb & 0xFF;
  switch (m) {
    case 0: a[0] = in->combined[0]; a[1] = in->combined[1]; a[2] = in->combined[2]; break;
    case 1: a[0] = in->texel0[0]; a[1] = in->texel0[1]; a[2] = in->texel0[2]; break;
    case 2: a[0] = in->texel1[0]; a[1] = in->texel1[1]; a[2] = in->texel1[2]; break;
    case 4: a[0] = in->shade[0]; a[1] = in->shade[1]; a[2] = in->shade[2]; break;
    case 6: a[0] = a[1] = a[2] = 0x100; break;
    case 7: a[0] = a[1] = a[2] = in->noise; break;
    default: a[0] = CR(in->c_muladd); a[1] = CG(in->c_muladd); a[2] = CB(in->c_muladd); break;
  }
  m = sel_alpha & 0xFF;
  switch (m) {
    case 0: a[3] = in->combined[3]; break; case 1: a[3] = in->texel0[3]; break; case 2: a[3] = in->texel1[3]; break;
    case 4: a[3] = in->shade[3]; break; case 6: a[3] = 0x100; break; default: a[3] = CA(in->c_muladd); break;
  }
  // mulsub
  m = (sel_rgb >> 8) & 0xFF;
  switch (m) {
    case 0: b[0] = in->combined[0]; b[1] = in->combined[1]; b[2] = in->combined[2]; break;
    case 1: b[0] = in->texel0[0]; b[1] = in->texel0[1]; b[2] = in->texel0[2]; break;
    case 2: b[0] = in->texel1[0]; b[1] = in->texel1[1]; b[2] = in->texel1[2]; break;
    case 4: b[0] = in->shade[0]; b[1] = in->shade[1]; b[2] = in->shade[2]; break;
    case 7: b[0] = b[1] = b[2] = (CG(in->c_mulsub) << 8) | CB(in->c_mulsub); break;
    default: b[0] = CR(in->c_mulsub); b[1] = CG(in->c_mulsub); b[2] = CB(in->c_mulsub); break;
  }
  m = (sel_alpha >> 8) & 0xFF;
  switch (m) {
    case 0: b[3] = in->combined[3]; break; case 1: b[3] = in->texel0[3]; break; case 2: b[3] = in->texel1[3]; break;
    case 4: b[3] = in->shade[3]; break; case 6: b[3] = 0x100; break; default: b[3] = CA(in->c_mulsub); break;
  }
  // mul
  m = (sel_rgb >> 16) & 0xFF;
  switch (m) {
    case 0: c[0] = in->combined[0]; c[1] = in->combined[1]; c[2] = in->combined[2]; break;
    case 1: c[0] = in->texel0[0]; c[1] = in->texel0[1]; c[2] = in->texel0[2]; break;
    case 2: c[0] = in->texel1[0]; c[1] = in->texel1[1]; c[2] = in->texel1[2]; break;
    case 4: c[0] = in->shade[0]; c[1] = in->shade[1]; c[2] = in->shade[2]; break;
    case 7: c[0] = c[1] = c[2] = in->combined[3]; break;
    case 8: c[0] = c[1] = c[2] = in->texel0[3]; break;
    case 9: c[0] = c[1] = c[2] = in->texel1[3]; break;
    case 11: c[0] = c[1] = c[2] = in->shade[3]; break;
    case 13: c[0] = c[1] = c[2] = in->lod_frac; break;
    case 15: c[0] = c[1] = c[2] = (CG(in->c_mul) << 8) | CB(in->c_mul); break;
    default: c[0] = CR(in->c_mul); c[1] = CG(in->c_mul); c[2] = CB(in->c_mul); break;
  }
  m = (sel_alpha >> 16) & 0xFF;
  switch (m) {
    case 0: c[3] = in->lod_frac; break; case 1: c[3] = in->texel0[3]; break; case 2: c[3] = in->texel1[3]; break;
    case 4: c[3] = in->shade[3]; break; default: c[3] = CA(in->c_mul); break;
  }
  // add
  m = (sel_rgb >> 24) & 0xFF;
  switch (m) {
    case 0: d[0] = in->combined[0]; d[1] = in->combined[1]; d[2] = in->combined[2]; break;
    case 1: d[0] = in->texel0[0]; d[1] = in->texel0[1]; d[2] = in->texel0[2]; break;
    case 2: d[0] = in->texel1[0]; d[1] = in->texel1[1]; d[2] = in->texel1[2]; break;
    case 4: d[0] = in->shade[0]; d[1] = in->shade[1]; d[2] = in->shade[2]; break;
    case 6: d[0] = d[1] = d[2] = 0x100; break;
    default: d[0] = CR(in->c_add); d[1] = CG(in->c_add); d[2] = CB(in->c_add); break;
  }
  m = (sel_alpha >> 24) & 0xFF;
  switch (m) {
    case 0: d[3] = in->combined[3]; break; case 1: d[3] = in->texel0[3]; break; case 2: d[3] = in->texel1[3]; break;
    case 4: d[3] = in->shade[3]; break; case 6: d[3] = 0x100; break; default: d[3] = CA(in->c_add); break;
  }
  for (int i = 0; i < 4; i++) {
    s32 cc = sx(c[i], 9);
    s32 aa = sx(a[i] - 0x80, 9) + 0x80, bb = sx(b[i] - 0x80, 9) + 0x80, dd = sx(d[i] - 0x80, 9) + 0x80;
    s32 color = (aa - bb) * cc + 0x80;
    out[i] = (s32)(s16)((color >> 8) + dd);
  }
}

// ---- z ----------------------------------------------------------------------
static inline s32 z_decompress(u32 z) {
  s32 exponent = z >> 11, mantissa = z & 0x7FF;
  s32 shift = maxi(6 - exponent, 0);
  s32 base = 0x40000 - (0x40000 >> exponent);
  return (mantissa << shift) + base;
}
static inline u32 z_compress(s32 z) {
  s32 inv_z = maxi(0x3FFFF - z, 1);
  s32 exponent = clampi(17 - msb(inv_z), 0, 7);
  s32 shift = maxi(6 - exponent, 0);
  s32 mantissa = (z >> shift) & 0x7FF;
  return (u32)((exponent << 11) + mantissa);
}

static inline int depth_test(s32 z, s32 dz, s32 dz_compressed, u32 cur_depth, s32 cur_dz, s32 *coverage_count, s32 cur_cov,
                             int z_compare, s32 z_mode, int force_blend, int aa_enable, int *blend_en, int *coverage_wrap, s32 *shift_x, s32 *shift_y) {
  int depth_pass;
  if (z_compare) {
    s32 memory_z = z_decompress(cur_depth);
    s32 memory_dz = 1 << cur_dz;
    s32 precision_factor = (cur_depth >> 11) & 0xF;
    int coplanar = 0;
    *shift_x = clampi(dz_compressed - cur_dz, 0, 4);
    *shift_y = clampi(cur_dz - dz_compressed, 0, 4);
    if (precision_factor < 3) {
      if (memory_dz != 0x8000) memory_dz = maxi(memory_dz << 1, 16 >> precision_factor);
      else { coplanar = 1; memory_dz = 0xFFFF; }
    }
    s32 combined_dz = dz | memory_dz;
    if (combined_dz != 0) combined_dz = 1 << msb(combined_dz);
    s32 combined_dz_ip = combined_dz;
    combined_dz <<= 3;
    int farther = coplanar || ((z + combined_dz) >= memory_z);
    int overflow = (*coverage_count + cur_cov) >= 8;
    *blend_en = force_blend || (!overflow && aa_enable && farther);
    *coverage_wrap = overflow;
    depth_pass = 0;
    int max_z = memory_z == 0x3FFFF;
    int front = z < memory_z;
    s32 z_closest = z - combined_dz;
    int nearer = coplanar || (z_closest <= memory_z);
    switch (z_mode) {
      case 0: depth_pass = max_z || (overflow ? front : nearer); break;
      case 1:
        if (!front || !farther || !overflow) depth_pass = max_z || (overflow ? front : nearer);
        else {
          s32 d = maxi(msb(combined_dz_ip & 0xFFFF), 0);
          s32 cvg_coeff = ((memory_z >> d) - (z >> d)) & 0xF;
          *coverage_count = mini((cvg_coeff * *coverage_count) >> 3, 8);
          depth_pass = 1;
        }
        break;
      case 2: depth_pass = front || max_z; break;
      default: depth_pass = farther && nearer && !max_z; break;
    }
  } else {
    *shift_x = 0;
    *shift_y = mini(0xF - dz_compressed, 4);
    int overflow = (*coverage_count + cur_cov) >= 8;
    *blend_en = force_blend || (!overflow && aa_enable);
    *coverage_wrap = overflow;
    depth_pass = 1;
  }
  return depth_pass;
}

// ---- blender ----------------------------------------------------------------
static inline void blender(const s32 *pixel, const s32 *memory, u32 fog, u32 blendc, s32 shade_alpha, u32 modes,
                           int force_blend, int blend_en, int color_on_cvg, int coverage_wrap, s32 shift_x, s32 shift_y, int final_cycle, s32 *out) {
  u32 m1a = modes & 0xFF, m1b = (modes >> 8) & 0xFF, m2a = (modes >> 16) & 0xFF, m2b = modes >> 24;
  s32 rgb1[3], rgb0[3];
  switch (m2a) {
    case 0: rgb1[0] = pixel[0]; rgb1[1] = pixel[1]; rgb1[2] = pixel[2]; break;
    case 1: rgb1[0] = memory[0]; rgb1[1] = memory[1]; rgb1[2] = memory[2]; break;
    case 2: rgb1[0] = CR(blendc); rgb1[1] = CG(blendc); rgb1[2] = CB(blendc); break;
    default: rgb1[0] = CR(fog); rgb1[1] = CG(fog); rgb1[2] = CB(fog); break;
  }
  if (final_cycle && color_on_cvg && !coverage_wrap) { out[0] = rgb1[0]; out[1] = rgb1[1]; out[2] = rgb1[2]; return; }
  switch (m1a) {
    case 0: rgb0[0] = pixel[0]; rgb0[1] = pixel[1]; rgb0[2] = pixel[2]; break;
    case 1: rgb0[0] = memory[0]; rgb0[1] = memory[1]; rgb0[2] = memory[2]; break;
    case 2: rgb0[0] = CR(blendc); rgb0[1] = CG(blendc); rgb0[2] = CB(blendc); break;
    default: rgb0[0] = CR(fog); rgb0[1] = CG(fog); rgb0[2] = CB(fog); break;
  }
  if (final_cycle && (!blend_en || (m1b == 0 && m2b == 0 && pixel[3] == 0xFF))) { out[0] = rgb0[0]; out[1] = rgb0[1]; out[2] = rgb0[2]; return; }
  s32 a0, a1;
  switch (m1b) { case 0: a0 = pixel[3]; break; case 1: a0 = CA(fog); break; case 2: a0 = shade_alpha; break; default: a0 = 0; break; }
  switch (m2b) { case 0: a1 = ~a0 & 0xFF; break; case 1: a1 = memory[3]; break; case 2: a1 = 0xFF; break; default: a1 = 0; break; }
  a0 >>= 3; a1 >>= 3;
  if (m2b == 1) { a0 = (a0 >> shift_x) & 0x3C; a1 = (a1 >> shift_y) | 3; }
  for (int i = 0; i < 3; i++) {
    s32 blended = rgb0[i] * a0 + rgb1[i] * (a1 + 1);
    if (!final_cycle || force_blend) out[i] = (blended >> 5) & 0xFF;
    else {
      s32 blend_sum = (a0 >> 2) + (a1 >> 2) + 1;
      out[i] = blender_lut[(blend_sum << 11) | ((blended >> 2) & 0x7FF)];
    }
  }
}

static inline s32 blend_coverage(s32 coverage, s32 memory_coverage, int blend_en, s32 mode) {
  switch (mode) {
    case 0: return blend_en ? mini(7, memory_coverage + coverage) : ((coverage - 1) & 7);
    case 1: return (coverage + memory_coverage) & 7;
    case 2: return 7;
    default: return memory_coverage;
  }
}

// ---- framebuffer interface (software: straight into RDRAM) ---------------------
#ifndef RDP_FB_BASE
#define RDP_FB_BASE rdram
#endif
#define VRAM8(i) (RDP_FB_BASE[(i) ^ 3])
#define VRAM16(i) (*(u16 *)(RDP_FB_BASE + (((i) ^ 1) << 1)))
#define VRAM32(i) (*(u32 *)(RDP_FB_BASE + ((i) << 2)))
#define M8 (RDRAM_MAX - 1)
#define M16 ((RDRAM_MAX >> 1) - 1)
#define M32 ((RDRAM_MAX >> 2) - 1)

static inline void alias_color_to_depth(void) {
  if (b_info.fb_fmt == FB_5551) {
    px_dz = (px_color[3] >> 3) | (px_color[2] & 8);
    px_depth = ((px_color[0] & 0xF8) << 6) | ((px_color[1] & 0xF8) << 1) | ((px_color[2] & 0xF8) >> 4);
  } else if (b_info.fb_fmt == FB_IA88) {
    u32 word = (px_color[0] << 8) | px_color[3];
    px_depth = (word >> 2) & 0x3FFF;
    px_dz = ((word & 3) << 2) | ((word & 1) * 3);
  }
}
static inline void alias_depth_to_color(void) {
  u32 word = (px_depth << 4) | px_dz;
  if (b_info.fb_fmt == FB_5551) {
    px_color[0] = (word >> 10) & 0xF8; px_color[1] = (word >> 5) & 0xF8; px_color[2] = word & 0xF8; px_color[3] = (word & 7) << 5;
  } else if (b_info.fb_fmt == FB_IA88) { px_color[0] = (word >> 10) & 0xFF; px_color[3] = (word >> 2) & 0xFF; }
  px_color_dirty = 1;
}

// Hidden (9th-bit) state is only meaningful while RDRAM still holds what the RDP wrote; a CPU write
// replaces it with the replicated LSB, as on hardware.
static inline u32 hidden_get(u32 idx16) { u32 v = VRAM16(idx16); return HIDDEN_AT(idx16, v); }
static inline void hidden_set(u32 idx16, u32 h) { rdp_hidden[idx16] = (u8)h; rdp_shadow16[idx16] = VRAM16(idx16); }

static inline void px_load(u32 x, u32 y) {
  u32 index = b_info.fb_addr + b_info.fb_width * y + x;
  px_color_dirty = 0; px_depth_dirty = 0;
  px_fb_index = index;
  switch (b_info.fb_fmt) {
    case FB_I4: case FB_I8: { index &= M8; u32 w = VRAM8(index); px_color[0] = px_color[1] = px_color[2] = w; px_color[3] = hidden_get(index >> 1); break; }
    case FB_5551: {
      index &= M16; u32 w = VRAM16(index);
      px_color[0] = (w >> 8) & 0xF8; px_color[1] = (w >> 3) & 0xF8; px_color[2] = (w << 2) & 0xF8;
      px_color[3] = ((hidden_get(index) << 5) | ((w & 1) << 7)) & 0xFF;
      break;
    }
    case FB_IA88: { index &= M16; u32 w = VRAM16(index); px_color[0] = px_color[1] = px_color[2] = w >> 8; px_color[3] = w & 0xFF; break; }
    default: { index &= M32; u32 w = VRAM32(index); px_color[0] = w >> 24; px_color[1] = (w >> 16) & 0xFF; px_color[2] = (w >> 8) & 0xFF; px_color[3] = w & 0xFF; break; }
  }
  u32 di = (b_info.depth_addr + b_info.fb_width * y + x) & M16;
  u32 w = VRAM16(di);
  px_depth = w >> 2;
  px_dz = hidden_get(di) | ((w & 3) << 2);
}

static inline void px_store(u32 x, u32 y) {
  u32 index = b_info.fb_addr + b_info.fb_width * y + x;
  if (px_color_dirty) {
    switch (b_info.fb_fmt) {
      case FB_I4: index &= M8; VRAM8(index) = 0; if (index & 1) hidden_set(index >> 1, px_color[3] & 3); else rdp_shadow16[index >> 1] = VRAM16(index >> 1); break;
      case FB_I8: {
        index &= M8;
        u32 col = (index & 1) ? px_color[1] : px_color[0];
        VRAM8(index) = col; if (index & 1) hidden_set(index >> 1, (col & 1) * 3); else rdp_shadow16[index >> 1] = VRAM16(index >> 1);
        break;
      }
      case FB_5551: {
        index &= M16;
        u32 cov = (u32)px_color[3] >> 5;
        VRAM16(index) = ((px_color[0] & 0xF8) << 8) | ((px_color[1] & 0xF8) << 3) | ((px_color[2] & 0xF8) >> 2) | (cov >> 2);
        hidden_set(index, cov & 3);
        break;
      }
      case FB_IA88: { index &= M16; u32 w = ((px_color[0] & 0xFF) << 8) | (px_color[3] & 0xFF); VRAM16(index) = w; hidden_set(index, (w & 1) * 3); break; }
      default:
        index &= M32;
        VRAM32(index) = ((u32)px_color[0] << 24) | (px_color[1] << 16) | (px_color[2] << 8) | px_color[3];
        hidden_set(2 * index, (px_color[1] & 1) * 3); hidden_set(2 * index + 1, (px_color[3] & 1) * 3);
        break;
    }
  }
  if (!b_info.alias && px_depth_dirty) {
    u32 di = (b_info.depth_addr + b_info.fb_width * y + x) & M16;
    VRAM16(di) = (px_depth << 2) | (px_dz >> 2);
    hidden_set(di, px_dz & 3);
  }
}

static inline void write_color(s32 r, s32 g, s32 b, s32 a) {
  px_color[0] = r; px_color[1] = g; px_color[2] = b;
  if (b_info.fb_fmt != FB_I4) px_color[3] = a;
  px_color_dirty = 1;
}

static inline void copy_pipeline(u32 word) {
  switch (b_info.fb_fmt) {
    case FB_I4: px_color[0] = px_color[1] = px_color[2] = px_color[3] = 0; px_color_dirty = 1; break;
    case FB_I8: word &= 0xFF; write_color(word, word, word, word); break;
    case FB_5551: write_color((word >> 8) & 0xF8, (word >> 3) & 0xF8, (word << 2) & 0xF8, (word & 1) * 0xE0); break;
    default: break;
  }
  if (b_info.alias) alias_color_to_depth();
}

static inline void fill_color(u32 col) {
  switch (b_info.fb_fmt) {
    case FB_8888: write_color(col >> 24, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF); break;
    case FB_5551: col >>= ((px_fb_index & 1) ^ 1) * 16; write_color((col >> 8) & 0xF8, (col >> 3) & 0xF8, (col << 2) & 0xF8, (col & 1) * 0xE0); break;
    case FB_IA88: col = (col >> (((px_fb_index & 1) ^ 1) * 16)) & 0xFFFF; write_color(col >> 8, col >> 8, col >> 8, col & 0xFF); break;
    case FB_I8: col = (col >> (((px_fb_index & 3) ^ 3) * 8)) & 0xFF; write_color(col, col, col, col); break;
    default: break;
  }
  if (b_info.alias) alias_color_to_depth();
}

// ---- per-pixel primitive evaluation ---------------------------------------------
static inline void shade_and_blend(const s32 *P, const u32 *SP, s32 x, s32 y) {
  const u32 *ST = b_states + (u32)P[P_STATE] * STATE_WORDS;
  u32 sflags = ST[S_FLAGS];
  u32 setup_flags = (u32)P[P_FLAGS] & 0xFF, setup_tile = ((u32)P[P_FLAGS] >> 8) & 0xFF;
  const u32 *TM = b_tmem + (u32)P[P_TMEM] * 1024;
  const u32 *TS = b_tiles + (u32)P[P_TILESET] * 8 * TILE_WORDS;
  s32 start_x = SP[4] & 0xFFFF, end_x = SP[4] >> 16;
  int flip = (setup_flags & SETUP_FLIP) != 0;
  int perspective = (sflags & RS_PERSPECTIVE) != 0;
  int tlut = (sflags & RS_TLUT) != 0, tlut_type = (sflags & RS_TLUT_TYPE) != 0;

  if (sflags & RS_NOISE) reseed_noise(x, y, (u32)P[P_SEQ]);

  // per-scanline attribute base
  s32 xfrac = SP[6] & 0xFF, base_x = (s32)SP[5];
  s32 dy = y - P[P_YBASE];
  int do_offset = (setup_flags & SETUP_DO_OFFSET) != 0;
  s32 stzw[4];
  for (int i = 0; i < 4; i++) {
    s32 de = P[P_DSTZW_DE + i], dyv = P[P_DSTZW_DY + i], diff = 0;
    if (do_offset) { s32 deh = de & ~0x1FF, dyh = dyv & ~0x1FF; diff = deh - (deh >> 2) - dyh + (dyh >> 2); }
    s32 v = P[P_STZW + i] + de * dy;
    stzw[i] = ((v & ~0x1FF) + diff - xfrac * ((P[P_DSTZW_DX + i] >> 8) & ~1)) & ~0x3FF;
  }

  if (sflags & RS_COPY) {
    if (!(x >= start_x && x <= end_x)) return;
    s32 dx = flip ? (x - start_x) : (end_x - x);
    s32 snapped = dx & (s32)b_info.dx_mask;
    s32 s_offset = dx - snapped;
    s32 lerp_dx = (dx >> b_info.dx_shift) * (flip ? 1 : -1);
    s32 s = (stzw[0] + (P[P_DSTZW_DX + 0] & ~0x1F) * lerp_dx) >> 16;
    s32 t = (stzw[1] + (P[P_DSTZW_DX + 1] & ~0x1F) * lerp_dx) >> 16;
    s32 w = (stzw[3] + (P[P_DSTZW_DX + 3] & ~0x1F) * lerp_dx) >> 16;
    if (perspective) perspective_divide(s, t, w, &s, &t);
    const u32 *T0 = TS + (setup_tile & 7) * TILE_WORDS;
    s32 texel0 = sample_texture_copy(T0, TM, s, t, s_offset, tlut);
    if ((sflags & RS_ALPHA_TEST) && b_info.fb_size == 2 && (texel0 & 1) == 0) return;
    copy_pipeline((u32)texel0);
    return;
  }
  if (sflags & RS_FILL) {
    if (x >= start_x && x <= end_x) fill_color((u32)P[P_FILL]);
    return;
  }

  u32 coverage = compute_coverage(SP, x);
  if (coverage == 0) return;
  s32 coverage_count = __builtin_popcount(coverage);
  int aa_enable = (sflags & RS_AA) != 0;
  if (!aa_enable && (coverage & 1) == 0) return;

  s32 dx = x - base_x;
  s32 dir = flip ? 1 : -1;
  s32 first = lsb(coverage);
  s32 yoff = first >> 1, xoff = ((first & 1) << 1) + (yoff & 1);

  // shade
  s32 shade[4];
  for (int i = 0; i < 4; i++) {
    s32 de = P[P_DRGBA_DE + i], dyv = P[P_DRGBA_DY + i], ddx = P[P_DRGBA_DX + i], diff = 0;
    if (do_offset) { s32 deh = de & ~0x1FF, dyh = dyv & ~0x1FF; diff = deh - (deh >> 2) - dyh + (dyh >> 2); }
    s32 v = P[P_RGBA + i] + de * dy;
    v = ((v & ~0x1FF) + diff - xfrac * ((ddx >> 8) & ~1)) & ~0x3FF;
    v += (ddx & ~0x1F) * dx;
    s32 sn = (s32)((u32)(v >> 14) << 2) + xoff * (ddx >> 14) + yoff * (dyv >> 14);
    sn = sx(sn, 16) >> 4;
    shade[i] = clamp9(sn);
  }

  // texture coordinates + z
  int uses_lod = (sflags & RS_USES_LOD) != 0;
  s32 st[2], st_dx[2] = {0, 0}, st_dy[2] = {0, 0};
  int persp_overflow = 0;
  s32 stw[3], sdx[3];
  static const u8 stw_i[3] = { 0, 1, 3 };
  for (int i = 0; i < 3; i++) { sdx[i] = P[P_DSTZW_DX + stw_i[i]] & ~0x1F; stw[i] = stzw[stw_i[i]] + sdx[i] * dx; }
  if (perspective) {
    persp_overflow |= perspective_divide(stw[0] >> 16, stw[1] >> 16, stw[2] >> 16, &st[0], &st[1]);
    if (uses_lod) {
      persp_overflow |= perspective_divide((stw[0] + dir * sdx[0]) >> 16, (stw[1] + dir * sdx[1]) >> 16, (stw[2] + dir * sdx[2]) >> 16, &st_dx[0], &st_dx[1]);
      persp_overflow |= perspective_divide((stw[0] + (P[P_DSTZW_DY + 0] & ~0x7FFF)) >> 16, (stw[1] + (P[P_DSTZW_DY + 1] & ~0x7FFF)) >> 16,
                                           (stw[2] + (P[P_DSTZW_DY + 3] & ~0x7FFF)) >> 16, &st_dy[0], &st_dy[1]);
    }
  } else {
    st[0] = stw[0] >> 16; st[1] = stw[1] >> 16;
    if (uses_lod) {
      st_dx[0] = (stw[0] + dir * sdx[0]) >> 16; st_dx[1] = (stw[1] + dir * sdx[1]) >> 16;
      st_dy[0] = (stw[0] + (P[P_DSTZW_DY + 0] & ~0x7FFF)) >> 16; st_dy[1] = (stw[1] + (P[P_DSTZW_DY + 1] & ~0x7FFF)) >> 16;
    }
  }
  s32 z = stzw[2] + P[P_DSTZW_DX + 2] * dx;
  s32 snz = (s32)((u32)(z >> 10) << 2) + xoff * (P[P_DSTZW_DX + 2] >> 10) + yoff * (P[P_DSTZW_DY + 2] >> 10);
  z = clamp_z(snz >> 5);

  // textures
  u32 tile0 = setup_tile & 7, tile1 = (tile0 + 1) & 7, max_level = setup_tile >> 3;
  u32 dzw = (u32)P[P_DZ];
  s32 min_lod = dzw >> 24;
  s32 lod_frac = 0;
  if (uses_lod) compute_lod_2cycle(&tile0, &tile1, &lod_frac, max_level, min_lod, st, st_dx, st_dy, persp_overflow,
                                   (sflags & RS_TEX_LOD) != 0, (sflags & RS_SHARPEN) != 0, (sflags & RS_DETAIL) != 0);
  s32 factors[4] = { (s16)(P[P_CONV0] & 0xFFFF), (s16)(P[P_CONV0] >> 16), (s16)(P[P_CONV1] & 0xFFFF), (s16)(P[P_CONV1] >> 16) };
  s32 texel0[4] = {0, 0, 0, 0}, texel1[4] = {0, 0, 0, 0}, zero4[4] = {0, 0, 0, 0};
  int sample_quad = (sflags & RS_SAMPLE_QUAD) != 0, mid_texel = (sflags & RS_MID_TEXEL) != 0;
  int convert_one = (sflags & RS_CONVERT_ONE) != 0, bilerp0 = (sflags & RS_BILERP0) != 0, bilerp1 = (sflags & RS_BILERP1) != 0;
  int uses_texel1 = (sflags & RS_USES_TEXEL1) != 0;
  if (sflags & RS_USES_TEXEL0)
    sample_texture(TS + tile0 * TILE_WORDS, TM, st[0], st[1], tlut, tlut_type, sample_quad, mid_texel, 0, bilerp0, factors, zero4, texel0);
  if (sflags & RS_USES_PIPELINED_TEXEL1) {
    int valid_next = (SP[SPAN_WORDS + 7] & 1) != 0;
    s32 lodlength = (s32)(s16)(SP[6] >> 16);
    int end_span = x == (flip ? end_x : start_x);
    if (end_span && lodlength >= 8 && valid_next) {
      // first pixel of the next scanline
      const u32 *SN = SP + SPAN_WORDS;
      s32 nxfrac = SN[6] & 0xFF, ndy = dy + 1, n[3];
      for (int i = 0; i < 3; i++) {
        int k = stw_i[i];
        s32 de = P[P_DSTZW_DE + k], dyv = P[P_DSTZW_DY + k], diff = 0;
        if (do_offset) { s32 deh = de & ~0x1FF, dyh = dyv & ~0x1FF; diff = deh - (deh >> 2) - dyh + (dyh >> 2); }
        s32 v = P[P_STZW + k] + de * ndy;
        n[i] = (((v & ~0x1FF) + diff - nxfrac * ((P[P_DSTZW_DX + k] >> 8) & ~1)) & ~0x3FF) >> 16;
      }
      if (perspective) perspective_divide(n[0], n[1], n[2], &st[0], &st[1]); else { st[0] = n[0]; st[1] = n[1]; }
    } else {
      s32 ndx = dx + dir, n[3];
      for (int i = 0; i < 3; i++) n[i] = (stzw[stw_i[i]] + sdx[i] * ndx) >> 16;
      if (perspective) perspective_divide(n[0], n[1], n[2], &st[0], &st[1]); else { st[0] = n[0]; st[1] = n[1]; }
    }
    tile1 = tile0;
    uses_texel1 = 1;
  }
  if (uses_texel1) {
    if (convert_one && !bilerp1) texture_convert_factors(texel0, factors, texel1);
    else sample_texture(TS + tile1 * TILE_WORDS, TM, st[0], st[1], tlut, tlut_type, sample_quad, mid_texel, convert_one, bilerp1, factors, texel0, texel1);
  }

  s32 rgb_dith, alpha_dith;
  u32 dither = ST[S_DITHER];
  dither_coefficients(x, y >> ((sflags & RS_INTERLACE_FIELD) ? 1 : 0), dither >> 2, dither & 3, &rgb_dith, &alpha_dith);

  // combiner
  int cvg_times_alpha = (sflags & RS_CVG_TIMES_ALPHA) != 0, alpha_cvg_select = (sflags & RS_ALPHA_CVG_SELECT) != 0;
  int alpha_test = (sflags & RS_ALPHA_TEST) != 0;
  CombIn in;
  s32 combined[4];
  s32 alpha_reference = 0;
  for (int i = 0; i < 4; i++) { in.shade[i] = shade[i]; in.combined[i] = 0; in.texel0[i] = texel0[i]; in.texel1[i] = texel1[i]; }
  in.lod_frac = lod_frac;
  in.noise = (s32)(((px_noise & 7) << 6) | 0x20);
  if (sflags & RS_MULTI_CYCLE) {
    in.c_muladd = P[P_CONST + 0]; in.c_mulsub = P[P_CONST + 1]; in.c_mul = P[P_CONST + 2]; in.c_add = P[P_CONST + 3];
    s32 c0[4];
    combiner_equation(&in, ST[S_RGB0], ST[S_ALPHA0], c0);
    if (alpha_test) {
      s32 ca = clamp9(c0[3]);
      s32 ea = ca + ((ca + 1) >> 8);
      if (alpha_cvg_select) ea = cvg_times_alpha ? ((ea * coverage_count + 4) >> 3) : (coverage_count << 5);
      else ea += alpha_dith;
      alpha_reference = clampi(ea, 0, 0xFF);
    }
    for (int i = 0; i < 4; i++) { in.combined[i] = c0[i]; s32 t = in.texel0[i]; in.texel0[i] = in.texel1[i]; in.texel1[i] = t; }
    in.c_muladd = P[P_CONST + 4]; in.c_mulsub = P[P_CONST + 5]; in.c_mul = P[P_CONST + 6]; in.c_add = P[P_CONST + 7];
    if (sflags & RS_NOISE_DUAL) { reseed_noise(x + 1023, y + 7, (u32)P[P_SEQ] + 11); in.noise = (s32)(((px_noise & 7) << 6) | 0x20); }
  } else {
    in.c_muladd = P[P_CONST + 4]; in.c_mulsub = P[P_CONST + 5]; in.c_mul = P[P_CONST + 6]; in.c_add = P[P_CONST + 7];
  }
  combiner_equation(&in, ST[S_RGB1], ST[S_ALPHA1], combined);
#ifdef RDP_DEBUG
  if (rdp_dbg_on && x == rdp_dbg_x && y == rdp_dbg_y)
    printf("   [mine] seq %d texel0=%d,%d,%d,%d texel1=%d,%d,%d,%d comb=%d,%d,%d,%d out=%d,%d,%d,%d shade=%d,%d,%d,%d cvg=%d\n", P[P_SEQ], texel0[0], texel0[1], texel0[2], texel0[3], texel1[0], texel1[1], texel1[2], texel1[3],
      in.combined[0], in.combined[1], in.combined[2], in.combined[3], combined[0], combined[1], combined[2], combined[3], shade[0], shade[1], shade[2], shade[3], coverage_count);
#endif
  for (int i = 0; i < 4; i++) combined[i] = clamp9(combined[i]);
  {
    s32 ea = combined[3] + ((combined[3] + 1) >> 8), ma;
    if (cvg_times_alpha) { ma = (ea * coverage_count + 4) >> 3; coverage_count = ma >> 5; }
    else ma = coverage_count << 5;
    if (alpha_cvg_select) ea = ma; else ea += alpha_dith;
    combined[3] = clampi(ea, 0, 0xFF);
  }
  if (!(sflags & RS_MULTI_CYCLE)) alpha_reference = combined[3];

  if (aa_enable && coverage_count == 0) return;
  if (alpha_test) {
    s32 threshold = (sflags & RS_ALPHA_TEST_DITHER) ? (s32)(px_noise & 0xFF) : CA((u32)P[P_BLEND]);
    if (alpha_reference < threshold) return;
  }
  s32 shade_alpha = mini(shade[3] + alpha_dith, 0xFF);

  // ---- depth / blend ----
  u32 db = ST[S_DB];
  int force_blend = (db & DB_FORCE_BLEND) != 0, z_compare = (db & DB_DEPTH_TEST) != 0, z_update = (db & DB_DEPTH_UPDATE) != 0;
  int image_read = (db & DB_IMAGE_READ) != 0, color_on_cvg = (db & DB_COLOR_ON_CVG) != 0;
  s32 memory[4];
  s32 memory_coverage = image_read ? (px_color[3] & 0xE0) : 0xE0;
  switch (b_info.fb_fmt) {
    case FB_I4: memory[0] = memory[1] = memory[2] = 0; memory_coverage = 0xE0; break;
    case FB_I8: memory[0] = memory[1] = memory[2] = px_color[0]; memory_coverage = 0xE0; break;
    case FB_5551: memory[0] = px_color[0] & 0xF8; memory[1] = px_color[1] & 0xF8; memory[2] = px_color[2] & 0xF8; break;
    case FB_IA88: memory[0] = memory[1] = memory[2] = px_color[0]; break;
    default: memory[0] = px_color[0]; memory[1] = px_color[1]; memory[2] = px_color[2]; break;
  }
  memory[3] = memory_coverage;
  s32 mem_cov = memory_coverage >> 5;
  int blend_en, coverage_wrap; s32 shift_x, shift_y;
  s32 dz = dzw & 0xFFFF, dzc = (dzw >> 16) & 0xFF;
  int z_pass = depth_test(z, dz, dzc, px_depth, (s32)px_dz, &coverage_count, mem_cov, z_compare, (ST[S_CVGZ] >> 8) & 3,
                          force_blend, (db & DB_AA) != 0, &blend_en, &coverage_wrap, &shift_x, &shift_y);
  if (z_pass && (!(db & DB_AA) || coverage_count != 0)) {
    s32 pixel[4] = { combined[0], combined[1], combined[2], combined[3] }, rgb[3];
    u32 modes = ST[S_BLEND0];
    if (db & DB_MULTI_CYCLE) {
      blender(pixel, memory, (u32)P[P_FOG], (u32)P[P_BLEND], shade_alpha, modes, force_blend, blend_en, color_on_cvg, coverage_wrap, shift_x, shift_y, 0, rgb);
      pixel[0] = rgb[0]; pixel[1] = rgb[1]; pixel[2] = rgb[2];
      modes = ST[S_BLEND1];
    }
    blender(pixel, memory, (u32)P[P_FOG], (u32)P[P_BLEND], shade_alpha, modes, force_blend, blend_en, color_on_cvg, coverage_wrap, shift_x, shift_y, 1, rgb);
    if (db & DB_DITHER) rgb_dither(rgb, rgb_dith);
    s32 new_cov = blend_coverage(coverage_count, mem_cov, blend_en, ST[S_CVGZ] & 3);
    write_color(rgb[0], rgb[1], rgb[2], new_cov << 5);
    if (z_update) {
      px_depth = z_compress(z); px_dz = dzc; px_depth_dirty = 1;
      if (b_info.alias) alias_depth_to_color();
    } else if (b_info.alias) alias_color_to_depth();
  }
}

static void sw_render_batch(void) {
  for (u32 i = 0; i < b_info.num_prims; i++) {
    const s32 *P = b_prims + i * PRIM_WORDS;
    s32 ylo = P[P_YLO], yhi = P[P_YHI];
    const u32 *SP = b_spans + (u32)P[P_SPAN] * SPAN_WORDS;
    for (s32 y = ylo; y <= yhi; y++, SP += SPAN_WORDS) {
      if (!(SP[7] & 1)) continue;
      s32 start_x = SP[4] & 0xFFFF, end_x = SP[4] >> 16;
      if (end_x >= (s32)b_info.fb_width) end_x = b_info.fb_width - 1;
      for (s32 x = start_x; x <= end_x; x++) {
        px_load(x, y);
#ifdef RDP_DEBUG   // (test tools: which primitives touch one pixel, with their state)
        s32 dbg_before[4] = { px_color[0], px_color[1], px_color[2], px_color[3] };
#endif
        shade_and_blend(P, SP, x, y);
#ifdef RDP_DEBUG
        if (rdp_dbg_on && x == rdp_dbg_x && y == rdp_dbg_y) {
          const u32 *ST = b_states + (u32)P[P_STATE] * STATE_WORDS;
          extern char *getenv(const char *); if (getenv("DBGP")) { printf("   [prim] seq %d:", P[P_SEQ]); for (int k = 0; k < PRIM_WORDS; k++) printf(" %d", P[k]); printf("\n   [span]"); for (int k = 0; k < 8; k++) printf(" %u", SP[k]); printf("\n   [tile]"); const u32 *T = b_tiles + (u32)P[P_TILESET] * 8 * TILE_WORDS + (((u32)P[P_FLAGS] >> 8) & 7) * TILE_WORDS; for (int k = 0; k < TILE_WORDS; k++) printf(" %u", T[k]); printf("\n"); }
          printf("   [dbg] prim %u seq %d flags %x tile %u: %02x%02x%02x/%02x -> %02x%02x%02x/%02x%s  sflags=%08x rgb0=%08x a0=%08x rgb1=%08x a1=%08x dith=%x db=%x bl0=%x bl1=%x cvgz=%x  prim=%08x env=%08x/%08x fog=%08x blend=%08x shade=%d,%d,%d,%d\n",
            i, P[P_SEQ], (u32)P[P_FLAGS] & 0xFF, ((u32)P[P_FLAGS] >> 8) & 0xFF, dbg_before[0], dbg_before[1], dbg_before[2], dbg_before[3], px_color[0], px_color[1], px_color[2], px_color[3], px_color_dirty ? "" : " (not written)",
            ST[S_FLAGS], ST[S_RGB0], ST[S_ALPHA0], ST[S_RGB1], ST[S_ALPHA1], ST[S_DITHER], ST[S_DB], ST[S_BLEND0], ST[S_BLEND1], ST[S_CVGZ],
            (u32)P[P_CONST], (u32)P[P_CONST + 1], (u32)P[P_CONST + 4], (u32)P[P_FOG], (u32)P[P_BLEND], P[P_RGBA] >> 16, P[P_RGBA + 1] >> 16, P[P_RGBA + 2] >> 16, P[P_RGBA + 3] >> 16);
        }
#endif
        px_store(x, y);
      }
    }
  }
}
