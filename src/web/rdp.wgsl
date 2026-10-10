// N64 RDP as a WebGPU compute shader. One invocation = one framebuffer pixel; it walks the primitives binned
// to its 8x8 tile in submission order and runs the full pixel pipeline (coverage, attribute interpolation,
// texturing, colour combiner, depth test, blender, coverage/Z write-back) with exact integer math.
// This is a line-for-line port of the C reference in rdp_pixel.h (pipeline formulation after parallel-RDP, MIT).

struct Batch {
  fb_fmt: u32, fb_width: u32, fb_height: u32, fb_addr: u32,
  depth_addr: u32, fb_size: u32, dx_shift: u32, dx_mask: u32,
  num_prims: u32, num_spans: u32, num_states: u32, num_tilesets: u32,
  num_tmem: u32, fb_alias: u32, rdram_mask: u32, z_use: u32,
  tiles_x: u32, ntiles: u32, bins_words: u32, ordered: u32,
};
@group(0) @binding(0) var<uniform> U: Batch;
@group(0) @binding(1) var<storage, read_write> fb: array<u32>;
@group(0) @binding(2) var<storage, read> prims: array<i32>;
@group(0) @binding(3) var<storage, read> spans: array<u32>;
@group(0) @binding(4) var<storage, read> tables: array<u32>;
@group(0) @binding(5) var<storage, read> tmem: array<u32>;
@group(0) @binding(6) var<storage, read> bins: array<u32>;
@group(0) @binding(7) var<storage, read_write> feedback: array<i32>;
var<private> pipeline_combined: vec4<i32>;
var<private> pipeline_memory: vec4<i32>;
var<private> pipeline_pre_memory: vec4<i32>;
var<private> key_rgb: vec3<i32>;
var<private> key_bypass: vec3<i32>;

// One source, two pipelines. The native pass is bit-exact and feeds emulated memory. With HD defined the same
// pipeline rasterizes every primitive again at 2^HL times the resolution into a separate, display-only copy of
// framebuffer memory (S*S sub-pixels per emulated halfword), deriving sub-pixel spans from the raw edge setup.
//#if HD
const HL: u32 = __HD_L__u;
//#else
const HL: u32 = 0u;
//#endif
const HS: i32 = 1i << HL;
const HM: i32 = HS - 1i;
const FBK: u32 = __FB_MASK__u;     // halfwords covered by the framebuffer copy, minus one

const PRIM_WORDS = 72u;
const P_FLAGS = 0u; const P_SPAN = 1u; const P_YLO = 2u; const P_YHI = 3u; const P_STATE = 4u; const P_TMEM = 5u;
const P_TILESET = 6u; const P_SEQ = 7u; const P_RGBA = 8u; const P_DRGBA_DX = 12u; const P_DRGBA_DE = 16u;
const P_DRGBA_DY = 20u; const P_STZW = 24u; const P_DSTZW_DX = 28u; const P_DSTZW_DE = 32u; const P_DSTZW_DY = 36u;
const P_CONST = 40u; const P_FOG = 48u; const P_BLEND = 49u; const P_FILL = 50u; const P_DZ = 51u; const P_CONV0 = 52u;
const P_CONV1 = 53u; const P_YBASE = 54u; const P_TAIL = 55u;
const P_XH = 56u; const P_XM = 57u; const P_XL = 58u; const P_DXHDY = 59u; const P_DXMDY = 60u; const P_DXLDY = 61u;
const P_YH = 62u; const P_YM = 63u; const P_YL = 64u; const P_SCX = 65u; const P_SCY = 66u; const P_STLO = 67u; const P_STHI = 69u;
const T_STATES = 0u; const T_TILES = 8192u; const T_LUT = 73728u; const T_PERSP = 81920u; const T_DITHER = 82048u;

const SETUP_FLIP = 1u; const SETUP_DO_OFFSET = 2u; const SETUP_SKIP_XFRAC = 4u; const SETUP_RECT = 32u; const SETUP_FILL_COPY = 128u;
const PF_ST_CLAMP = 0x10000u;
// Tags kept with each sub-pixel of the high-resolution copy (above the 18 bits of an emulated halfword), for the video
// output pass: TAG_FLAT - drawn by a filtered texture rectangle; TAG_HX / TAG_HY - its texture coordinate was held at the
// rectangle's last sample along x / y, so the pixel wants blending towards the neighbouring emulated pixel (see vi.wgsl).
const TAG_FLAT = 0x40000u; const TAG_HX = 0x80000u; const TAG_HY = 0x100000u; const TAG_MASK = 0x1C0000u;
const RS_INTERLACE_FIELD = 1u; const RS_AA = 4u; const RS_PERSPECTIVE = 8u; const RS_TLUT = 16u; const RS_TLUT_TYPE = 32u;
const RS_CVG_TIMES_ALPHA = 64u; const RS_ALPHA_CVG_SELECT = 128u; const RS_MULTI_CYCLE = 256u; const RS_TEX_LOD = 512u;
const RS_SHARPEN = 1024u; const RS_DETAIL = 2048u; const RS_FILL = 4096u; const RS_COPY = 8192u; const RS_SAMPLE_QUAD = 16384u;
const RS_ALPHA_TEST = 32768u; const RS_ALPHA_TEST_DITHER = 65536u; const RS_MID_TEXEL = 131072u; const RS_USES_TEXEL0 = 262144u;
const RS_USES_TEXEL1 = 524288u; const RS_USES_LOD = 1048576u; const RS_USES_PIPELINED_TEXEL1 = 2097152u;
const RS_CONVERT_ONE = 4194304u; const RS_BILERP0 = 8388608u; const RS_BILERP1 = 16777216u; const RS_NOISE_DUAL = 33554432u;
const RS_KEY = 67108864u;
const RS_NOISE = 268435456u;
const DB_DEPTH_TEST = 1u; const DB_DEPTH_UPDATE = 2u; const DB_FORCE_BLEND = 8u; const DB_IMAGE_READ = 16u;
const DB_COLOR_ON_CVG = 32u; const DB_MULTI_CYCLE = 64u; const DB_AA = 128u; const DB_DITHER = 256u;
const TILE_CLAMP_S = 1u; const TILE_MIRROR_S = 2u; const TILE_CLAMP_T = 4u; const TILE_MIRROR_T = 8u;
const FB_5551 = 2u; const FB_IA88 = 3u; const FB_8888 = 4u;

var<private> px_color: vec4<i32>;
var<private> px_color_dirty: bool;
var<private> px_depth: u32;
var<private> px_dz: u32;
var<private> px_depth_dirty: bool;
var<private> px_fb_index: u32;
var<private> px_sub: u32;      // sub-pixel slot inside the emulated pixel (always 0 in the native pass)
var<private> px_tag: u32;      // tags stored with the colour (high-resolution pass only)
var<private> px_tag_next: u32; // ... as the primitive being drawn would set them
var<private> px_noise: u32;

fn msb(v: i32) -> i32 { if (v <= 0) { return -1; } return i32(firstLeadingBit(u32(v))); }
fn sx(v: i32, bits: u32) -> i32 { return extractBits(v, 0u, bits); }
fn clamp9(c: i32) -> i32 { return clamp(sx(c - 0x80, 9u) + 0x80, 0, 0xFF); }
fn clamp_z(z0: i32) -> i32 { return clamp(sx(z0 - (1 << 17u), 19u) + (1 << 17u), 0, 0x3FFFF); }
fn b2i(b: bool) -> i32 { return select(0, 1, b); }

fn reseed_noise(x: u32, y: u32, off: u32) {
  let NP = 1103515245u;
  var s = vec3<u32>(x, y, off);
  s = ((s >> vec3<u32>(8u)) ^ s.yzx) * NP;
  s = ((s >> vec3<u32>(8u)) ^ s.yzx) * NP;
  s = ((s >> vec3<u32>(8u)) ^ s.yzx) * NP;
  px_noise = (s.x >> 16u) & 0xFFFFu;
}

// span of the primitive on the current line (set by load_span / compute_span)
var<private> sp_xl: vec4<u32>;
var<private> sp_xr: vec4<u32>;
var<private> sp_start_x: i32;
var<private> sp_end_x: i32;
var<private> sp_base_x: i32;
var<private> sp_xfrac: i32;
var<private> sp_lodlength: i32;

fn load_span(sp: u32) {
  let w0 = spans[sp]; let w1 = spans[sp + 1u]; let w2 = spans[sp + 2u]; let w3 = spans[sp + 3u];
  sp_xl = vec4<u32>(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu, w1 >> 16u);
  sp_xr = vec4<u32>(w2 & 0xFFFFu, w2 >> 16u, w3 & 0xFFFFu, w3 >> 16u);
  let w4 = spans[sp + 4u]; let w6 = spans[sp + 6u];
  sp_start_x = i32(w4 & 0xFFFFu); sp_end_x = i32(w4 >> 16u);
  sp_base_x = i32(spans[sp + 5u]);
  sp_xfrac = i32(w6 & 0xFFu);
  sp_lodlength = extractBits(i32(w6 >> 16u), 0u, 16u);
}

//#if HD
// Span setup for one high-resolution line: the software span setup with the primitive scaled by S, the four
// sub-scanlines evaluated as one vector. Returns false if the line is empty.
fn compute_span(P: u32, y: i32) -> bool {
  let flags = u32(prims[P + P_FLAGS]) & 0xFFu;
  let flip = (flags & SETUP_FLIP) != 0u;
  let xh0 = prims[P + P_XH] << HL; let xm0 = prims[P + P_XM] << HL; let xl0 = prims[P + P_XL] << HL;
  let dxhdy = prims[P + P_DXHDY]; let dxmdy = prims[P + P_DXMDY]; let dxldy = prims[P + P_DXLDY];
  let yh_n = prims[P + P_YH];
  let yh = yh_n * HS; let ym = prims[P + P_YM] * HS; let yl = prims[P + P_YL] * HS;
  let yh_base = (yh_n & ~3) * HS;
  let scx = u32(prims[P + P_SCX]); let scy = u32(prims[P + P_SCY]);
  let ylo = max(yh, i32(scy & 0xFFFFu) * HS); let yhi = min(yl, i32(scy >> 16u) * HS);
  let lo_sc = i32(scx & 0xFFFFu) << (1u + HL); let hi_sc = i32(scx >> 16u) << (1u + HL);
  var xhb = xh0 + (4 * y - yh_base) * dxhdy;
  if ((flags & SETUP_DO_OFFSET) != 0u) { xhb += 3 * dxhdy; }
  sp_base_x = xhb >> 15u;
  sp_xfrac = select((xhb >> 7u) & 0xFF, 0, (flags & SETUP_SKIP_XFRAC) != 0u);
  sp_lodlength = 0;
  let sbits = 27u + HL;
  let ys = vec4<i32>(y * 4) + vec4<i32>(0, 1, 2, 3);
  let clip_y = (ys < vec4<i32>(ylo)) | (ys >= vec4<i32>(yhi));
  let dh = ys - vec4<i32>(yh_base);
  var xh = vec4<i32>(xh0) + dh * dxhdy;
  let xm = vec4<i32>(xm0) + dh * dxmdy;
  var xl = vec4<i32>(xl0) + (ys - vec4<i32>(ym)) * dxldy;
  xl = select(xl, xm, ys < vec4<i32>(ym));
  xl = extractBits(xl, 0u, sbits); xh = extractBits(xh, 0u, sbits);
  let one = vec4<i32>(1); let zero = vec4<i32>(0);
  let xhs = (xh >> vec4<u32>(12u)) | select(zero, one, (xh & vec4<i32>(0xFFF)) != zero);
  let xls = (xl >> vec4<u32>(12u)) | select(zero, one, (xl & vec4<i32>(0xFFF)) != zero);
  let l0 = select(xls, xhs, flip); let r0 = select(xhs, xls, flip);
  let invalid = ((l0 >> vec4<u32>(1u)) > (r0 >> vec4<u32>(1u))) | clip_y;
  let all_over = all(min(l0, r0) >= vec4<i32>(hi_sc));
  let all_under = all(max(l0, r0) < vec4<i32>(lo_sc));
  let l = select(clamp(l0, vec4<i32>(lo_sc), vec4<i32>(hi_sc)), vec4<i32>(0xFFFF), invalid);
  let r = select(clamp(r0, vec4<i32>(lo_sc), vec4<i32>(hi_sc)), zero, invalid);
  sp_xl = vec4<u32>(l) & vec4<u32>(0xFFFFu); sp_xr = vec4<u32>(r) & vec4<u32>(0xFFFFu);
  sp_start_x = i32(min(min(sp_xl.x, sp_xl.y), min(sp_xl.z, sp_xl.w)) >> 3u);
  sp_end_x = i32(max(max(sp_xr.x, sp_xr.y), max(sp_xr.z, sp_xr.w)) >> 3u);
  return !all(invalid) && !all_over && !all_under;
}
//#endif

fn compute_coverage(x: i32) -> u32 {
  let xl = sp_xl; let xr = sp_xr;
  let b = (u32(x) << 3u) & 0xFFFFu;
  let s0 = b; let s4 = (b + 4u) & 0xFFFFu; let s2 = (b + 2u) & 0xFFFFu; let s6 = (b + 6u) & 0xFFFFu;
  var c = 0u;
  if (s0 >= xl.x && s0 < xr.x) { c |= 1u; }
  if (s4 >= xl.x && s4 < xr.x) { c |= 2u; }
  if (s2 >= xl.y && s2 < xr.y) { c |= 4u; }
  if (s6 >= xl.y && s6 < xr.y) { c |= 8u; }
  if (s0 >= xl.z && s0 < xr.z) { c |= 16u; }
  if (s4 >= xl.z && s4 < xr.z) { c |= 32u; }
  if (s2 >= xl.w && s2 < xr.w) { c |= 64u; }
  if (s6 >= xl.w && s6 < xr.w) { c |= 128u; }
  return c;
}

// returns (s, t, overflow)
fn perspective_divide(s: i32, t: i32, w_in: i32) -> vec3<i32> {
  var overflow = 0;
  let w_carry = w_in <= 0;
  let w = w_in & 0x7FFF;
  let shift = min(14 - msb(w), 14);
  let normout = (w << u32(shift)) & 0x3FFF;
  let wnorm = normout & 0xFF;
  let ti = T_PERSP + u32(normout >> 8u) * 2u;
  let rcp = ((i32(tables[ti + 1u]) * wnorm) >> 10u) + i32(tables[ti]);
  var prod = vec2<i32>(s * rcp, t * rcp);
  let temp_mask = ((1 << 30u) - 1) & -((1 << 29u) >> u32(shift));
  let oob = prod & vec2<i32>(temp_mask);
  var temp: vec2<i32>;
  if (shift != 14) { prod = prod >> vec2<u32>(u32(13 - shift)); temp = prod; }
  else { temp = prod << vec2<u32>(1u); }
  if (oob.x != temp_mask && oob.x != 0) { temp.x = select(-0x8000, 0x7FFF, (prod.x & (1 << 29u)) == 0); overflow = 1; }
  if (oob.y != temp_mask && oob.y != 0) { temp.y = select(-0x8000, 0x7FFF, (prod.y & (1 << 29u)) == 0); overflow = 1; }
  if (w_carry) { temp = vec2<i32>(0x7FFF); overflow = 1; }
  temp = clamp(temp, vec2<i32>(-0x10000), vec2<i32>(0xFFFF));
  return vec3<i32>(temp.x, temp.y, overflow);
}

// ---- texture ------------------------------------------------------------------------------------------
fn tm8(tm: u32, idx: u32) -> u32 { let i = idx & 0xFFFu; return (tmem[tm + (i >> 2u)] >> ((i & 3u) * 8u)) & 0xFFu; }
fn tm16(tm: u32, idx: u32) -> u32 { let i = idx & 0x7FFu; return (tmem[tm + (i >> 1u)] >> ((i & 1u) * 16u)) & 0xFFFFu; }

fn texel_mask(c_in: i32, maskbits: i32, mirror: bool) -> i32 {
  var c = c_in;
  if (maskbits != 0) {
    let mask = 1 << u32(maskbits);
    if (mirror) { c ^= max((c & mask) - 1, 0); }
    c &= mask - 1;
  }
  return c;
}

const TX_RGBA4 = 0; const TX_IA4 = 1; const TX_CI4 = 2; const TX_CI4_TLUT = 3; const TX_CI8_TLUT = 4; const TX_CI32 = 5;
const TX_CI32_TLUT = 6; const TX_RGBA8 = 7; const TX_IA8 = 8; const TX_YUV16 = 9; const TX_RGBA16 = 10; const TX_IA16 = 11;
const TX_RGBA32 = 12;

fn conv_rgba16(w: u32) -> vec4<i32> {
  let r = (w >> 11u) & 31u; let g = (w >> 6u) & 31u; let b = (w >> 1u) & 31u;
  return vec4<i32>(i32((r << 3u) | (r >> 2u)), i32((g << 3u) | (g >> 2u)), i32((b << 3u) | (b >> 2u)), i32((w & 1u) * 0xFFu));
}
fn conv_ia16(w: u32) -> vec4<i32> { let i = i32(w >> 8u); return vec4<i32>(i, i, i, i32(w & 0xFFu)); }
fn conv_lut(w: u32, tlut_type: bool) -> vec4<i32> { if (tlut_type) { return conv_ia16(w); } return conv_rgba16(w); }

// Every texel format is at most two 16-bit TMEM reads: the texel itself, then (TLUT entry | upper RGBA32 half | chroma).
fn texel_fetch(T: u32, TM: u32, kind: i32, s: i32, t: i32, lut_offset: u32, addr_xor: u32, tlut_type: bool, chroma_x: u32) -> vec4<i32> {
  let us = u32(s); let ut = u32(t);
  let t4 = tables[T + 4u];
  let base = (t4 & 0xFFFFu) + (t4 >> 16u) * ut;
  let pal = (tables[T + 5u] >> 16u) & 0xFFu;
  let is_lut = kind == TX_CI4_TLUT || kind == TX_CI8_TLUT || kind == TX_CI32_TLUT;
  let two = is_lut || kind == TX_RGBA32 || kind == TX_YUV16;
  var bo = base; var bits = 16u;
  if (kind <= TX_CI4_TLUT) { bo += us >> 1u; bits = 4u; }
  else if (kind == TX_CI8_TLUT || kind == TX_RGBA8 || kind == TX_IA8 || kind == TX_YUV16) { bo += us; bits = 8u; }
  else { bo += us * 2u; }
  bo &= select(0xFFFu, 0x7FFu, two);
  if (kind == TX_YUV16) { bo |= 0x800u; }
  let rowx = (ut & 1u) << 1u;
  let i1 = ((bo >> 1u) ^ rowx) ^ 1u;
  let w16 = tm16(TM, i1);
  var w = w16;
  if (bits == 8u) { w = (w16 >> ((~bo & 1u) * 8u)) & 0xFFu; }
  else if (bits == 4u) { w = (w16 >> ((~bo & 1u) * 8u + (~us & 1u) * 4u)) & 0xFu; }
  var w2 = 0u;
  if (two) {
    var i2 = i1 | 0x400u;
    if (is_lut) {
      var ci = w;
      if (bits == 4u) { ci |= pal << 4u; } else if (bits == 16u) { ci = w16 >> 8u; }
      i2 = 0x400u | (((ci << 2u) + lut_offset) ^ addr_xor);
    } else if (kind == TX_YUV16) {
      let bc = (base + chroma_x * 2u) & 0x7FFu;
      i2 = ((bc >> 1u) ^ rowx) ^ 1u;
    }
    w2 = tm16(TM, i2);
  }
  var o: vec4<i32>;
  switch (kind) {
    case 0: { o = vec4<i32>(i32(w | (w << 4u))); }
    case 1: { var inn = w & 0xEu; inn = (inn << 4u) | (inn << 1u) | (inn >> 2u); o = vec4<i32>(i32(inn), i32(inn), i32(inn), i32((w & 1u) * 0xFFu)); }
    case 2: { o = vec4<i32>(i32(w | (pal << 4u))); }
    case 3, 4, 6: { o = conv_lut(w2, tlut_type); }
    case 5: { o = vec4<i32>(i32(w16 >> 8u), i32(w16 & 0xFFu), i32(w16 >> 8u), i32(w16 & 0xFFu)); }
    case 7: { o = vec4<i32>(i32(w)); }
    case 8: { var inn = w >> 4u; var al = w & 0xFu; al |= al << 4u; inn |= inn << 4u; o = vec4<i32>(i32(inn), i32(inn), i32(inn), i32(al)); }
    case 9: { o = vec4<i32>(i32((w2 >> 8u) & 0xFFu) - 0x80, i32(w2 & 0xFFu) - 0x80, i32(w), i32(w)); }
    case 10: { o = conv_rgba16(w16); }
    case 11: { o = conv_ia16(w16); }
    default: { o = vec4<i32>(i32(w16 >> 8u), i32(w16 & 0xFFu), i32(w2 >> 8u), i32(w2 & 0xFFu)); }
  }
  return o;
}

fn shift_coord_raw(coord_in: i32, shift: i32) -> i32 {
  var coord = (coord_in << 16u) >> 16u;
  if (shift < 11) { coord = coord >> u32(shift); }
  else { coord = (coord << u32(32 - shift)) >> 16u; }
  return coord;
}
fn clamp_and_shift_coord(clamp_bit: bool, coord_in: i32, lo: i32, hi: i32, shift: i32) -> i32 {
  var coord = shift_coord_raw(coord_in, shift);
  if (clamp_bit) {
    if ((coord >> 3u) >= hi) { coord = (((hi >> 2u) - (lo >> 2u)) & 0x3FF) << 5u; }
    else { coord = max(coord - (lo << 3u), 0); }
  } else { coord -= lo << 3u; }
  return coord;
}

fn texture_convert_factors(tin: vec4<i32>, k: vec4<i32>) -> vec4<i32> {
  let r = sx(tin.x, 9u); let g = sx(tin.y, 9u); let b = sx(tin.z, 9u);
  return vec4<i32>(b + ((k.x * g + 0x80) >> 8u), b + ((k.y * r + k.z * g + 0x80) >> 8u), b + ((k.w * r + 0x80) >> 8u), b);
}

fn bilinear_3tap2(t00: vec2<i32>, t10: vec2<i32>, t01: vec2<i32>, t11: vec2<i32>, fx: i32, fy: i32) -> vec2<i32> {
  let up = (fx + fy) >= 32;
  let tb = select(t00, t11, up);
  let ffx = select(fx, 32 - fy, up); let ffy = select(fy, 32 - fx, up);
  return (((t10 - tb) * ffx + (t01 - tb) * ffy + vec2<i32>(0x10)) >> vec2<u32>(5u)) + tb;
}

fn sample_texture(T: u32, TM: u32, st_s: i32, st_t: i32, tlut: bool, tlut_type: bool, sample_quad: bool, mid_texel_state: bool,
                  convert_one: bool, bilerp: bool, factors: vec4<i32>, prev_cycle: vec4<i32>) -> vec4<i32> {
  let t5 = tables[T + 5u]; let t6 = tables[T + 6u];
  let tflags = t5 >> 24u;
  let mask_s = i32(t6 & 0xFFu); let shift_s = i32((t6 >> 8u) & 0xFFu); let mask_t = i32((t6 >> 16u) & 0xFFu); let shift_t = i32(t6 >> 24u);
  let mir_s = (tflags & TILE_MIRROR_S) != 0u; let mir_t = (tflags & TILE_MIRROR_T) != 0u;
  var cs = clamp_and_shift_coord((tflags & TILE_CLAMP_S) != 0u, st_s, i32(tables[T]), i32(tables[T + 1u]), shift_s);
  var ct = clamp_and_shift_coord((tflags & TILE_CLAMP_T) != 0u, st_t, i32(tables[T + 2u]), i32(tables[T + 3u]), shift_t);
  var fx = 0; var fy = 0;
  if (sample_quad || tlut) { fx = cs & 31; fy = ct & 31; }
  var sum_frac = fx + fy;
  cs = cs >> 5u; ct = ct >> 5u;
  let s0 = texel_mask(cs, mask_s, mir_s);
  var t0 = texel_mask(ct, mask_t, mir_t);
  var s1 = texel_mask(cs + 1, mask_s, mir_s);
  var t1 = texel_mask(ct + 1, mask_t, mir_t);
  let tdiff = max(t1 - t0, -255);
  t1 = (t0 & 0xFF) + tdiff;
  t0 &= 0xFF;
  var tb = vec4<i32>(0); var t10 = vec4<i32>(0); var t01 = vec4<i32>(0); var t11 = vec4<i32>(0);
  let mid_texel = mid_texel_state && bilerp && fx == 0x10 && fy == 0x10;
  let upper_lut = sum_frac >= 0x20;
  if (mid_texel) { sum_frac = 0; }
  let fmt = t5 & 0xFFu; let size = (t5 >> 8u) & 0xFFu;
  let yuv = fmt == 1u;
  var bs = select(s0, s1, sum_frac >= 0x20); var bt = select(t0, t1, sum_frac >= 0x20);
  let chroma_frac = ((s0 & 1) << 4u) | (fx >> 1u);
  var kind = TX_CI32;
  var ax = 0u; var en_side = sample_quad; var en_11 = mid_texel;
  if (tlut) {
    if (!sample_quad) { bs = s0; bt = t0; s1 = s0; t1 = t0; }
    ax = select(1u, 2u, upper_lut);
    kind = TX_CI32_TLUT;
    if (size == 0u) { kind = TX_CI4_TLUT; } else if (size == 1u) { kind = TX_CI8_TLUT; }
    en_side = bilerp;
  } else if (yuv) {
    kind = TX_YUV16; bs = s0; bt = t0; en_11 = sample_quad;
  } else {
    switch (fmt) {
      case 0u: { if (size == 0u) { kind = TX_RGBA4; } else if (size == 1u) { kind = TX_RGBA8; } else if (size == 2u) { kind = TX_RGBA16; } else { kind = TX_RGBA32; } }
      case 2u: { if (size == 0u) { kind = TX_CI4; } else if (size == 1u) { kind = TX_RGBA8; } else { kind = TX_CI32; } }
      case 3u: { if (size == 0u) { kind = TX_IA4; } else if (size == 1u) { kind = TX_IA8; } else if (size == 2u) { kind = TX_IA16; } else { kind = TX_CI32; } }
      default: { if (size == 0u) { kind = TX_RGBA4; } else if (size == 1u) { kind = TX_RGBA8; } else { kind = TX_CI32; } }
    }
  }
  let cx0 = u32(s0) >> 1u; let cx1 = u32(s1 + (s1 - s0)) >> 1u;
  let lut0 = select(0u, 3u, tlut && sum_frac >= 0x20);
  var tx: array<vec4<i32>, 4>;
  for (var i = 0u; i < 4u; i++) {
    tx[i] = vec4<i32>(0);
    if ((i == 3u && !en_11) || ((i == 1u || i == 2u) && !en_side)) { continue; }
    let fs = select(select(s1, s0, i == 2u), bs, i == 0u);
    let ft = select(select(t1, t0, i == 1u), bt, i == 0u);
    tx[i] = texel_fetch(T, TM, kind, fs, ft, select(i, lut0, i == 0u), ax, tlut_type, select(cx0, cx1, (i & 1u) != 0u));
  }
  tb = tx[0]; t10 = tx[1]; t01 = tx[2]; t11 = tx[3];

  var accum: vec4<i32>;
  if (convert_one) {
    let ps = vec3<i32>(sx(prev_cycle.x, 9u), sx(prev_cycle.y, 9u), sx(prev_cycle.z, 9u));
    if (sample_quad) {
      let mid_rg = select(mid_texel, mid_texel_state && chroma_frac == 0x10 && fy == 0x10, yuv);
      let mid_ba = mid_texel;
      let upper_ba = sum_frac >= 32;
      let upper_rg = select(upper_ba, (chroma_frac + fy) >= 32 && !mid_rg, yuv);
      var conv: vec4<i32>;
      {
        let f0 = select(ps.x, ps.y, upper_rg); let f1 = select(ps.y, ps.x, upper_rg);
        if (mid_rg) { conv = vec4<i32>(f0 * (t01.xy - t11.xy) + f1 * (t10.xy - t11.xy) + ((tb.xy - t11.xy) << vec2<u32>(6u)) + vec2<i32>(0x80), 0, 0); }
        else { let b = select(tb.xy, t11.xy, upper_rg && yuv); conv = vec4<i32>(f0 * (t10.xy - b) + f1 * (t01.xy - b) + vec2<i32>(0x80), 0, 0); }
      }
      {
        let f0 = select(ps.x, ps.y, upper_ba); let f1 = select(ps.y, ps.x, upper_ba);
        var c2: vec2<i32>;
        if (mid_ba) { c2 = f0 * (t01.zw - t11.zw) + f1 * (t10.zw - t11.zw) + ((tb.zw - t11.zw) << vec2<u32>(6u)) + vec2<i32>(0x80); }
        else { let b = select(tb.zw, t11.zw, upper_ba && yuv); c2 = f0 * (t10.zw - b) + f1 * (t01.zw - b) + vec2<i32>(0x80); }
        conv = vec4<i32>(conv.xy, c2);
      }
      let v = (conv >> vec4<u32>(8u)) + vec4<i32>(ps.z);
      accum = extractBits(v, 0u, 16u);
    } else { accum = vec4<i32>(ps.z); }
  } else if (yuv) {
    if (sample_quad) {
      if (bilerp) {
        let mid_chroma = mid_texel_state && chroma_frac == 0x10 && fy == 0x10;
        var c: vec2<i32>; var l: vec2<i32>;
        if (mid_chroma) { c = (tb.xy + t10.xy + t11.xy + t01.xy + vec2<i32>(2)) >> vec2<u32>(2u); }
        else { c = bilinear_3tap2(tb.xy, t10.xy, t01.xy, t11.xy, chroma_frac, fy); }
        if (mid_texel) { l = (tb.zw + t10.zw + t11.zw + t01.zw + vec2<i32>(2)) >> vec2<u32>(2u); }
        else { l = bilinear_3tap2(tb.zw, t10.zw, t01.zw, t11.zw, fx, fy); }
        accum = vec4<i32>(c, l);
      } else {
        let l = select(tb.zw, t11.zw, (fx + fy) >= 32); let c = select(tb.xy, t11.xy, (chroma_frac + fy) >= 32);
        accum = vec4<i32>(c, l);
      }
    } else { accum = tb; }
  } else if (mid_texel) {
    accum = (tb + t01 + t10 + t11 + vec4<i32>(2)) >> vec4<u32>(2u);
  } else if (bilerp && (sample_quad || tlut)) {
    let up = sum_frac >= 32;
    let ffx = select(fx, 32 - fy, up); let ffy = select(fy, 32 - fx, up);
    accum = (((t10 - tb) * ffx + (t01 - tb) * ffy + vec4<i32>(0x10)) >> vec4<u32>(5u)) + tb;
  } else { accum = tb; }

  if (!bilerp && !convert_one) { accum = texture_convert_factors(accum, factors); }
  return accum;
}

fn sample_texture_copy_word(T: u32, TM: u32, s_in: i32, t: i32, s_offset: i32, tlut: bool) -> i32 {
  let t4 = tables[T + 4u]; let t5 = tables[T + 5u]; let t6 = tables[T + 6u];
  let size = (t5 >> 8u) & 0xFFu; let tflags = t5 >> 24u;
  let mask_s = i32(t6 & 0xFFu); let mask_t = i32((t6 >> 16u) & 0xFFu);
  let mir_s = (tflags & TILE_MIRROR_S) != 0u; let mir_t = (tflags & TILE_MIRROR_T) != 0u;
  let high_word = s_offset < 2;
  let replicate_8bpp = high_word && size != 2u && !tlut;
  let s_shamt = min(size, 2u);
  let idx_mask = select(0x7FFu, 0x3FFu, size == 3u || tlut);
  var s = s_in;
  var samp: i32;
  let mt = texel_mask(t, mask_t, mir_t);
  let tbase = (t4 & 0xFFFFu) + (t4 >> 16u) * u32(mt);
  let txor = (u32(mt) & 1u) * 8u;
  if (replicate_8bpp) {
    s += 2 * s_offset;
    let ms0 = texel_mask(s, mask_s, mir_s); let ms1 = texel_mask(s + 1, mask_s, mir_s);
    let n0 = ((tbase * 2u + (u32(ms0) << s_shamt)) & 0x1FFFu) ^ txor;
    let n1 = ((tbase * 2u + (u32(ms1) << s_shamt)) & 0x1FFFu) ^ txor;
    var samp0 = i32(tm16(TM, ((n0 >> 2u) & idx_mask) ^ 1u)); var samp1 = i32(tm16(TM, ((n1 >> 2u) & idx_mask) ^ 1u));
    if (size == 1u) { samp0 = (samp0 >> (8u - 4u * (n0 & 2u))) & 0xFF; samp1 = (samp1 >> (8u - 4u * (n1 & 2u))) & 0xFF; }
    else if (size == 0u) { samp0 = ((samp0 >> (12u - 4u * (n0 & 3u))) & 0xF) * 0x11; samp1 = ((samp1 >> (12u - 4u * (n1 & 3u))) & 0xF) * 0x11; }
    else { samp0 = samp0 >> 8u; samp1 = samp1 >> 8u; }
    samp = (samp0 << 8u) | samp1;
  } else {
    s += s_offset;
    let ms = texel_mask(s, mask_s, mir_s);
    let n = ((tbase * 2u + (u32(ms) << s_shamt)) & 0x1FFFu) ^ txor;
    samp = i32(tm16(TM, ((n >> 2u) & idx_mask) ^ 1u));
    if (tlut) {
      if (size == 0u) { samp = (samp >> (12u - 4u * (n & 3u))) & 0xF; samp |= i32((t5 >> 16u) & 0xFFu) << 4u; samp = (samp << 2u) + s_offset; }
      else { samp = (samp >> (8u - 4u * (n & 2u))) & 0xFF; samp = (samp << 2u) + s_offset; }
      samp = i32(tm16(TM, u32(samp | 0x400) ^ 1u));
    }
  }
  return samp;
}

fn sample_texture_copy(T: u32, TM: u32, s_in: i32, t_in: i32, s_offset: i32, tlut: bool) -> i32 {
  let t6 = tables[T + 6u];
  let s = (shift_coord_raw(s_in, i32((t6 >> 8u) & 0xFFu)) - (i32(tables[T]) << 3u)) >> 5u;
  let t = (shift_coord_raw(t_in, i32(t6 >> 24u)) - (i32(tables[T + 2u]) << 3u)) >> 5u;
  if (U.fb_size == 0u) { return 0; }
  if (U.fb_size == 1u) {
    let samp = sample_texture_copy_word(T, TM, s, t, s_offset >> 1u, tlut);
    return (samp >> u32(8 - 8 * (s_offset & 1))) & 0xFF;
  }
  return sample_texture_copy_word(T, TM, s, t, s_offset, tlut);
}

struct Lod { tile0: u32, tile1: u32, lod_frac: i32 };
fn compute_lod_2cycle(tile0_in: u32, tile1_in: u32, max_level: u32, min_lod: i32, st: vec2<i32>, st_dx: vec2<i32>, st_dy: vec2<i32>,
                      persp_overflow: bool, tex_lod_en: bool, sharpen: bool, detail: bool) -> Lod {
  var r: Lod; r.tile0 = tile0_in; r.tile1 = tile1_in; r.lod_frac = 0;
  var magnify = false; var distant = false;
  var tile_offset = 0u;
  if (persp_overflow) { distant = true; r.lod_frac = 0xFF; }
  else {
    var dx = st_dx - st; var dy = st_dy - st;
    dx ^= dx >> vec2<u32>(31u); dy ^= dy >> vec2<u32>(31u);
    let m = max(dx, dy);
    let max_d = max(m.x, m.y);
    if (max_d >= 0x4000) { distant = true; r.lod_frac = 0xFF; tile_offset = max_level; }
    else if (max_d < 32) {
      distant = max_level == 0u; magnify = true;
      if (!sharpen && !detail) { r.lod_frac = select(0, 0xFF, distant); }
      else { r.lod_frac = extractBits((max(min_lod, max_d) << 3u) + select(0, -0x100, sharpen), 0u, 16u); }
    } else {
      let mip_base = max(msb(max_d >> 5u), 0);
      distant = u32(mip_base) >= max_level;
      if (distant && !sharpen && !detail) { r.lod_frac = 0xFF; }
      else { r.lod_frac = ((max_d << 3u) >> u32(mip_base)) & 0xFF; tile_offset = u32(mip_base); }
    }
  }
  if (tex_lod_en) {
    if (distant) { tile_offset = max_level; }
    if (!detail) {
      r.tile0 = (r.tile0 + tile_offset) & 7u;
      if (distant || (!sharpen && magnify)) { r.tile1 = r.tile0; } else { r.tile1 = (r.tile0 + 1u) & 7u; }
    } else {
      r.tile1 = (r.tile0 + tile_offset + select(2u, 1u, distant || magnify)) & 7u;
      r.tile0 = (r.tile0 + tile_offset + select(1u, 0u, magnify)) & 7u;
    }
  }
  return r;
}

// ---- dither -------------------------------------------------------------------------------------------
fn dither_matrix(m: u32, x: i32, y: i32) -> i32 { return i32(tables[T_DITHER + m * 16u + u32((y & 3) * 4 + (x & 3))]); }
fn dither_coefficients(x: i32, y: i32, mode_rgb: u32, mode_alpha: u32) -> vec2<i32> {
  var rgb_d = 0; var alpha_d = 0;
  if (mode_rgb < 2u) { rgb_d = dither_matrix(mode_rgb, x, y) * 0x49; }
  else if (mode_rgb == 2u) { rgb_d = i32(px_noise & 0x1FFu); }
  if (mode_alpha == 2u) { alpha_d = i32(px_noise & 7u); }
  else if (mode_alpha != 3u) {
    if (mode_rgb >= 2u) { alpha_d = dither_matrix(mode_rgb & 1u, x, y); } else { alpha_d = rgb_d & 7; }
    if (mode_alpha == 1u) { alpha_d = ~alpha_d & 7; }
  }
  return vec2<i32>(rgb_d, alpha_d);
}
fn rgb_dither(rgb: vec3<i32>, dith: i32) -> vec3<i32> {
  let d = (vec3<i32>(dith) >> vec3<u32>(0u, 3u, 6u)) & vec3<i32>(7);
  let r = select((rgb & vec3<i32>(0xF8)) + vec3<i32>(8), vec3<i32>(255), rgb > vec3<i32>(247));
  let replace_sign = (d - (rgb & vec3<i32>(7))) >> vec3<u32>(31u);
  return (rgb + ((r - rgb) & replace_sign)) & vec3<i32>(0xFF);
}

// ---- combiner -----------------------------------------------------------------------------------------
struct CombIn { c_muladd: u32, c_mulsub: u32, c_mul: u32, c_add: u32, shade: vec4<i32>, combined: vec4<i32>, texel0: vec4<i32>, texel1: vec4<i32>, lod_frac: i32, noise: i32 };
var<private> inp: CombIn;
fn crgb(c: u32) -> vec3<i32> { return vec3<i32>(i32(c >> 24u), i32((c >> 16u) & 0xFFu), i32((c >> 8u) & 0xFFu)); }
fn ca(c: u32) -> i32 { return i32(c & 0xFFu); }
fn k9(c: u32) -> i32 { return i32((((c >> 16u) & 0xFFu) << 8u) | ((c >> 8u) & 0xFFu)); }

fn combiner_equation(sel_rgb: u32, sel_alpha: u32) -> vec4<i32> {
  var a: vec4<i32>; var b: vec4<i32>; var c: vec4<i32>; var d: vec4<i32>;
  var v: vec3<i32>; var w: i32;
  switch (sel_rgb & 0xFFu) {
    case 0u: { v = inp.combined.xyz; } case 1u: { v = inp.texel0.xyz; } case 2u: { v = inp.texel1.xyz; } case 4u: { v = inp.shade.xyz; }
    case 6u: { v = vec3<i32>(0x100); } case 7u: { v = vec3<i32>(inp.noise); } default: { v = crgb(inp.c_muladd); }
  }
  switch (sel_alpha & 0xFFu) {
    case 0u: { w = inp.combined.w; } case 1u: { w = inp.texel0.w; } case 2u: { w = inp.texel1.w; } case 4u: { w = inp.shade.w; }
    case 6u: { w = 0x100; } default: { w = ca(inp.c_muladd); }
  }
  a = vec4<i32>(v, w);
  switch ((sel_rgb >> 8u) & 0xFFu) {
    case 0u: { v = inp.combined.xyz; } case 1u: { v = inp.texel0.xyz; } case 2u: { v = inp.texel1.xyz; } case 4u: { v = inp.shade.xyz; }
    case 7u: { v = vec3<i32>(k9(inp.c_mulsub)); } default: { v = crgb(inp.c_mulsub); }
  }
  switch ((sel_alpha >> 8u) & 0xFFu) {
    case 0u: { w = inp.combined.w; } case 1u: { w = inp.texel0.w; } case 2u: { w = inp.texel1.w; } case 4u: { w = inp.shade.w; }
    case 6u: { w = 0x100; } default: { w = ca(inp.c_mulsub); }
  }
  b = vec4<i32>(v, w);
  switch ((sel_rgb >> 16u) & 0xFFu) {
    case 0u: { v = inp.combined.xyz; } case 1u: { v = inp.texel0.xyz; } case 2u: { v = inp.texel1.xyz; } case 4u: { v = inp.shade.xyz; }
    case 7u: { v = vec3<i32>(inp.combined.w); } case 8u: { v = vec3<i32>(inp.texel0.w); } case 9u: { v = vec3<i32>(inp.texel1.w); }
    case 11u: { v = vec3<i32>(inp.shade.w); } case 13u: { v = vec3<i32>(inp.lod_frac); } case 15u: { v = vec3<i32>(k9(inp.c_mul)); }
    default: { v = crgb(inp.c_mul); }
  }
  switch ((sel_alpha >> 16u) & 0xFFu) {
    case 0u: { w = inp.lod_frac; } case 1u: { w = inp.texel0.w; } case 2u: { w = inp.texel1.w; } case 4u: { w = inp.shade.w; }
    default: { w = ca(inp.c_mul); }
  }
  c = vec4<i32>(v, w);
  switch ((sel_rgb >> 24u) & 0xFFu) {
    case 0u: { v = inp.combined.xyz; } case 1u: { v = inp.texel0.xyz; } case 2u: { v = inp.texel1.xyz; } case 4u: { v = inp.shade.xyz; }
    case 6u: { v = vec3<i32>(0x100); } default: { v = crgb(inp.c_add); }
  }
  switch ((sel_alpha >> 24u) & 0xFFu) {
    case 0u: { w = inp.combined.w; } case 1u: { w = inp.texel0.w; } case 2u: { w = inp.texel1.w; } case 4u: { w = inp.shade.w; }
    case 6u: { w = 0x100; } default: { w = ca(inp.c_add); }
  }
  d = vec4<i32>(v, w);
  let cc = extractBits(c, 0u, 9u);
  let aa = extractBits(a - vec4<i32>(0x80), 0u, 9u) + vec4<i32>(0x80);
  let bb = extractBits(b - vec4<i32>(0x80), 0u, 9u) + vec4<i32>(0x80);
  let dd = extractBits(d - vec4<i32>(0x80), 0u, 9u) + vec4<i32>(0x80);
  let color = (aa - bb) * cc + vec4<i32>(0x80);
  key_rgb = (color.xyz + dd.xyz * vec3<i32>(256)) & vec3<i32>(0x1FFFF);
  key_bypass = a.xyz;
  return extractBits((color >> vec4<u32>(8u)) + dd, 0u, 16u);
}

// ---- depth --------------------------------------------------------------------------------------------
fn z_decompress(z: u32) -> i32 {
  let exponent = i32(z >> 11u); let mantissa = i32(z & 0x7FFu);
  let shift = u32(max(6 - exponent, 0));
  return (mantissa << shift) + (0x40000 - (0x40000 >> u32(exponent)));
}
fn z_compress(z: i32) -> u32 {
  let inv_z = max(0x3FFFF - z, 1);
  let exponent = clamp(17 - msb(inv_z), 0, 7);
  let shift = u32(max(6 - exponent, 0));
  return u32((exponent << 11u) + ((z >> shift) & 0x7FF));
}

struct DepthResult { ok: bool, blend_en: bool, coverage_wrap: bool, coverage_count: i32, shift_x: i32, shift_y: i32 };
fn depth_test(z: i32, dz: i32, dz_compressed: i32, cur_depth: u32, cur_dz: i32, coverage_count: i32, cur_cov: i32,
              z_compare: bool, z_mode: u32, force_blend: bool, aa_enable: bool) -> DepthResult {
  var r: DepthResult; r.coverage_count = coverage_count;
  if (z_compare) {
    let memory_z = z_decompress(cur_depth);
    var memory_dz = 1 << u32(cur_dz);
    let precision_factor = i32((cur_depth >> 11u) & 0xFu);
    var coplanar = false;
    r.shift_x = clamp(dz_compressed - cur_dz, 0, 4);
    r.shift_y = clamp(cur_dz - dz_compressed, 0, 4);
    if (precision_factor < 3) {
      if (memory_dz != 0x8000) { memory_dz = max(memory_dz << 1u, 16 >> u32(precision_factor)); }
      else { coplanar = true; memory_dz = 0xFFFF; }
    }
    var combined_dz = dz | memory_dz;
    if (combined_dz != 0) { combined_dz = 1 << u32(msb(combined_dz)); }
    let combined_dz_ip = combined_dz;
    combined_dz = combined_dz << 3u;
    let farther = coplanar || ((z + combined_dz) >= memory_z);
    let overflow = (coverage_count + cur_cov) >= 8;
    r.blend_en = force_blend || (!overflow && aa_enable && farther);
    r.coverage_wrap = overflow;
    r.ok = false;
    let max_z = memory_z == 0x3FFFF;
    let front = z < memory_z;
    let nearer = coplanar || ((z - combined_dz) <= memory_z);
    switch (z_mode) {
      case 0u: { r.ok = max_z || select(nearer, front, overflow); }
      case 1u: {
        if (!front || !farther || !overflow) { r.ok = max_z || select(nearer, front, overflow); }
        else {
          let dd = u32(max(msb(combined_dz_ip & 0xFFFF), 0));
          let cvg_coeff = ((memory_z >> dd) - (z >> dd)) & 0xF;
          r.coverage_count = min((cvg_coeff * coverage_count) >> 3u, 8);
          r.ok = true;
        }
      }
      case 2u: { r.ok = front || max_z; }
      default: { r.ok = farther && nearer && !max_z; }
    }
  } else {
    r.shift_x = 0;
    r.shift_y = min(0xF - dz_compressed, 4);
    let overflow = (coverage_count + cur_cov) >= 8;
    r.blend_en = force_blend || (!overflow && aa_enable);
    r.coverage_wrap = overflow;
    r.ok = true;
  }
  return r;
}

// ---- blender ------------------------------------------------------------------------------------------
fn blender(pixel: vec4<i32>, memory: vec4<i32>, fog: u32, blendc: u32, shade_alpha: i32, modes: u32, force_blend: bool, blend_en: bool,
           color_on_cvg: bool, coverage_wrap: bool, shift_x: i32, shift_y: i32, final_cycle: bool) -> vec3<i32> {
  let m1a = modes & 0xFFu; let m1b = (modes >> 8u) & 0xFFu; let m2a = (modes >> 16u) & 0xFFu; let m2b = modes >> 24u;
  var rgb1: vec3<i32>; var rgb0: vec3<i32>;
  switch (m2a) { case 0u: { rgb1 = pixel.xyz; } case 1u: { rgb1 = memory.xyz; } case 2u: { rgb1 = crgb(blendc); } default: { rgb1 = crgb(fog); } }
  if (final_cycle && color_on_cvg && !coverage_wrap) { return rgb1; }
  switch (m1a) { case 0u: { rgb0 = pixel.xyz; } case 1u: { rgb0 = memory.xyz; } case 2u: { rgb0 = crgb(blendc); } default: { rgb0 = crgb(fog); } }
  if (final_cycle && (!blend_en || (m1b == 0u && m2b == 0u && pixel.w == 0xFF))) { return rgb0; }
  var a0: i32; var a1: i32;
  switch (m1b) { case 0u: { a0 = pixel.w; } case 1u: { a0 = ca(fog); } case 2u: { a0 = shade_alpha; } default: { a0 = 0; } }
  switch (m2b) { case 0u: { a1 = ~a0 & 0xFF; } case 1u: { a1 = memory.w; } case 2u: { a1 = 0xFF; } default: { a1 = 0; } }
  a0 = a0 >> 3u; a1 = a1 >> 3u;
  if (m2b == 1u) { a0 = (a0 >> u32(shift_x)) & 0x3C; a1 = (a1 >> u32(shift_y)) | 3; }
  let blended = rgb0 * a0 + rgb1 * (a1 + 1);
  if (!final_cycle || force_blend) { return (blended >> vec3<u32>(5u)) & vec3<i32>(0xFF); }
  let blend_sum = u32((a0 >> 2u) + (a1 >> 2u) + 1);
  let idx = vec3<u32>((blend_sum << 11u)) | (vec3<u32>(blended >> vec3<u32>(2u)) & vec3<u32>(0x7FFu));
  return vec3<i32>(
    i32((tables[T_LUT + (idx.x >> 2u)] >> ((idx.x & 3u) * 8u)) & 0xFFu),
    i32((tables[T_LUT + (idx.y >> 2u)] >> ((idx.y & 3u) * 8u)) & 0xFFu),
    i32((tables[T_LUT + (idx.z >> 2u)] >> ((idx.z & 3u) * 8u)) & 0xFFu));
}

fn blend_coverage(coverage: i32, memory_coverage: i32, blend_en: bool, mode: u32) -> i32 {
  switch (mode) {
    case 0u: { if (blend_en) { return min(7, memory_coverage + coverage); } return (coverage - 1) & 7; }
    case 1u: { return (coverage + memory_coverage) & 7; }
    case 2u: { return 7; }
    default: { return memory_coverage; }
  }
}

// ---- framebuffer interface ----------------------------------------------------------------------------
fn alias_color_to_depth() {
  if (U.fb_fmt == FB_5551) {
    px_dz = u32((px_color.w >> 3u) | (px_color.z & 8));
    px_depth = u32(((px_color.x & 0xF8) << 6u) | ((px_color.y & 0xF8) << 1u) | ((px_color.z & 0xF8) >> 4u));
  } else if (U.fb_fmt == FB_IA88) {
    let word = u32((px_color.x << 8u) | px_color.w);
    px_depth = (word >> 2u) & 0x3FFFu;
    px_dz = ((word & 3u) << 2u) | ((word & 1u) * 3u);
  }
}
fn alias_depth_to_color() {
  let word = i32((px_depth << 4u) | px_dz);
  if (U.fb_fmt == FB_5551) { px_color = vec4<i32>((word >> 10u) & 0xF8, (word >> 5u) & 0xF8, word & 0xF8, (word & 7) << 5u); }
  else if (U.fb_fmt == FB_IA88) { px_color.x = (word >> 10u) & 0xFF; px_color.w = (word >> 2u) & 0xFF; }
  px_color_dirty = true;
}

// emulated halfword k (plus the sub-pixel slot in the high-resolution copy)
fn fbi(k: u32) -> u32 { return ((k & FBK) << (2u * HL)) + px_sub; }

fn px_load(xh: u32, yh: u32) {
  let x = xh >> HL; let y = yh >> HL;
  px_sub = ((yh & u32(HM)) << HL) | (xh & u32(HM));
  let index = U.fb_addr + U.fb_width * y + x;
  let m16 = U.rdram_mask >> 1u;
  px_color_dirty = false; px_depth_dirty = false;
  px_fb_index = index;
  px_tag = 0u; px_tag_next = 0u;
  if (U.fb_fmt == FB_5551) {
    let w = fb[fbi(index & m16)];
    px_tag = w & TAG_MASK;
    px_color = vec4<i32>(i32((w >> 8u) & 0xF8u), i32((w >> 3u) & 0xF8u), i32((w << 2u) & 0xF8u), i32((((w >> 16u) & 3u) << 5u) | ((w & 1u) << 7u)));
  } else if (U.fb_fmt == FB_IA88) {
    let w = fb[fbi(index & m16)];
    let i = i32((w >> 8u) & 0xFFu);
    px_color = vec4<i32>(i, i, i, i32(w & 0xFFu));
  } else {
    let i = (index & (m16 >> 1u)) * 2u;
    let hi = fb[fbi(i)] & 0xFFFFu; let lo = fb[fbi(i + 1u)] & 0xFFFFu;
    px_color = vec4<i32>(i32(hi >> 8u), i32(hi & 0xFFu), i32(lo >> 8u), i32(lo & 0xFFu));
  }
  let w = fb[fbi((U.depth_addr + U.fb_width * y + x) & m16)];
  px_depth = (w & 0xFFFFu) >> 2u;
  px_dz = ((w >> 16u) & 3u) | ((w & 3u) << 2u);
}

fn px_store(xh: u32, yh: u32) {
  let x = xh >> HL; let y = yh >> HL;
  let index = U.fb_addr + U.fb_width * y + x;
  let m16 = U.rdram_mask >> 1u;
  if (px_color_dirty) {
    let c = vec4<u32>(px_color);
    if (U.fb_fmt == FB_5551) {
      let cov = c.w >> 5u;
      fb[fbi(index & m16)] = ((c.x & 0xF8u) << 8u) | ((c.y & 0xF8u) << 3u) | ((c.z & 0xF8u) >> 2u) | (cov >> 2u) | ((cov & 3u) << 16u) | px_tag;
    } else if (U.fb_fmt == FB_IA88) {
      let w = ((c.x & 0xFFu) << 8u) | (c.w & 0xFFu);
      fb[fbi(index & m16)] = w | (((w & 1u) * 3u) << 16u);
    } else {
      let i = (index & (m16 >> 1u)) * 2u;
      fb[fbi(i)] = ((c.x & 0xFFu) << 8u) | (c.y & 0xFFu) | (((c.y & 1u) * 3u) << 16u);
      fb[fbi(i + 1u)] = ((c.z & 0xFFu) << 8u) | (c.w & 0xFFu) | (((c.w & 1u) * 3u) << 16u);
    }
  }
  if (U.fb_alias == 0u && px_depth_dirty) {
    fb[fbi((U.depth_addr + U.fb_width * y + x) & m16)] = ((px_depth << 2u) & 0xFFFFu) | (px_dz >> 2u) | ((px_dz & 3u) << 16u);
  }
}

fn write_color(c: vec4<i32>) { px_color = c; px_color_dirty = true; px_tag = px_tag_next; }

fn copy_pipeline(word: u32) {
  if (U.fb_fmt == FB_5551) { write_color(vec4<i32>(i32((word >> 8u) & 0xF8u), i32((word >> 3u) & 0xF8u), i32((word << 2u) & 0xF8u), i32((word & 1u) * 0xE0u))); }
  if (U.fb_alias != 0u) { alias_color_to_depth(); }
}

fn fill_color(col_in: u32) {
  var col = col_in;
  if (U.fb_fmt == FB_8888) { write_color(vec4<i32>(i32(col >> 24u), i32((col >> 16u) & 0xFFu), i32((col >> 8u) & 0xFFu), i32(col & 0xFFu))); }
  else if (U.fb_fmt == FB_5551) {
    col = col >> (((px_fb_index & 1u) ^ 1u) * 16u);
    write_color(vec4<i32>(i32((col >> 8u) & 0xF8u), i32((col >> 3u) & 0xF8u), i32((col << 2u) & 0xF8u), i32((col & 1u) * 0xE0u)));
  } else if (U.fb_fmt == FB_IA88) {
    col = (col >> (((px_fb_index & 1u) ^ 1u) * 16u)) & 0xFFFFu;
    let i = i32(col >> 8u);
    write_color(vec4<i32>(i, i, i, i32(col & 0xFFu)));
  }
  if (U.fb_alias != 0u) { alias_color_to_depth(); }
}

// attribute base for the scanline (span setup that depends on per-primitive gradients); dy counts lines at the
// rendering resolution, so in the high-resolution pass it is split into whole emulated lines plus a fraction
fn attr_base(P: u32, base: u32, de_off: u32, dy_off: u32, dx_off: u32, dy: i32, xfrac: i32, do_offset: bool) -> vec4<i32> {
  let a = vec4<i32>(prims[P + base], prims[P + base + 1u], prims[P + base + 2u], prims[P + base + 3u]);
  let de = vec4<i32>(prims[P + de_off], prims[P + de_off + 1u], prims[P + de_off + 2u], prims[P + de_off + 3u]);
  let dyv = vec4<i32>(prims[P + dy_off], prims[P + dy_off + 1u], prims[P + dy_off + 2u], prims[P + dy_off + 3u]);
  let dxv = vec4<i32>(prims[P + dx_off], prims[P + dx_off + 1u], prims[P + dx_off + 2u], prims[P + dx_off + 3u]);
  var diff = vec4<i32>(0);
  if (do_offset) {
    let deh = de & vec4<i32>(~0x1FF); let dyh = dyv & vec4<i32>(~0x1FF);
    diff = (deh - (deh >> vec4<u32>(2u)) - dyh + (dyh >> vec4<u32>(2u))) >> vec4<u32>(HL);
  }
  let v = a + de * (dy >> HL) + (de >> vec4<u32>(HL)) * (dy & HM);
  return ((v & vec4<i32>(~0x1FF)) + diff - ((xfrac * ((dxv >> vec4<u32>(8u)) & vec4<i32>(~1))) >> vec4<u32>(HL))) & vec4<i32>(~0x3FF);
}

// x, y: pixel at the rendering resolution; SP: this line's emulated-resolution span record (the span actually used
// is in the sp_* globals)
fn shade_and_blend(P: u32, SP: u32, x: i32, y: i32, preview_cycle0: bool) {
  let ST = T_STATES + u32(prims[P + P_STATE]) * 16u;
  let sflags = tables[ST];
  let pflags = u32(prims[P + P_FLAGS]);
  let setup_flags = pflags & 0xFFu; let setup_tile = (pflags >> 8u) & 0xFFu;
  let TM = u32(prims[P + P_TMEM]) * 1024u;
  let TS = T_TILES + u32(prims[P + P_TILESET]) * 64u;
  let start_x = sp_start_x; let end_x = sp_end_x;
  px_tag_next = 0u;
  // Fill and copy mode work in whole pixels (the right and bottom edges are inclusive, copy mode steps four pixels at a
  // time), so the high-resolution pass gives them the span and the coordinates of the emulated pixel they fall in.
  let whole = (setup_flags & SETUP_FILL_COPY) != 0u;
  let xw = select(x, x >> HL, whole);
  let in_span = xw >= start_x && xw <= end_x;
  if ((sflags & RS_FILL) != 0u) {
    if (in_span) { fill_color(u32(prims[P + P_FILL])); }
    return;
  }
  let is_copy = (sflags & RS_COPY) != 0u;
  if (is_copy && !in_span) { return; }

  let flip = (setup_flags & SETUP_FLIP) != 0u;
  let dir = select(-1, 1, flip);
  let aa_enable = (sflags & RS_AA) != 0u;
  var coverage_pass = true;
  var coverage_count = 0; var xoff = 0; var yoff = 0;
  if (!is_copy) {
    let coverage = select(compute_coverage(x), 0u, preview_cycle0);
    coverage_pass = select((coverage & 1u) != 0u, coverage != 0u, aa_enable);
    coverage_count = i32(countOneBits(coverage));
    let first = select(0, i32(firstTrailingBit(coverage)), coverage != 0u);
    yoff = first >> 1u; xoff = ((first & 1) << 1u) + (yoff & 1);
  }

  let perspective = (sflags & RS_PERSPECTIVE) != 0u;
  let tlut = (sflags & RS_TLUT) != 0u; let tlut_type = (sflags & RS_TLUT_TYPE) != 0u;
  let seq = u32(prims[P + P_SEQ]);
  if ((sflags & RS_NOISE) != 0u) { reseed_noise(u32(x), u32(y), seq); }

  let xfrac = sp_xfrac; let base_x = sp_base_x;
  let dy = select(y, (y >> HL) << HL, whole) - prims[P + P_YBASE] * HS;
  let dx = x - base_x;
  let dxq = dx >> HL; let dxr = dx & HM;          // whole emulated pixels + sub-pixel remainder
  let do_offset = (setup_flags & SETUP_DO_OFFSET) != 0u;
  let stzw = attr_base(P, P_STZW, P_DSTZW_DE, P_DSTZW_DY, P_DSTZW_DX, dy, xfrac, do_offset);
  let dstzw_dx = vec4<i32>(prims[P + P_DSTZW_DX], prims[P + P_DSTZW_DX + 1u], prims[P + P_DSTZW_DX + 2u], prims[P + P_DSTZW_DX + 3u]);
  let dstzw_dy = vec4<i32>(prims[P + P_DSTZW_DY], prims[P + P_DSTZW_DY + 1u], prims[P + P_DSTZW_DY + 2u], prims[P + P_DSTZW_DY + 3u]);
  let sdx = dstzw_dx.xyw & vec3<i32>(~0x1F);

  // texture coordinate candidates: [0] this pixel, [1] one step along x, [2] one step along y (LOD), [3] next pixel
  // (the "pipelined" texel1 of 1-cycle mode). All go through one perspective divider.
  let uses_lod = (sflags & RS_USES_LOD) != 0u && !is_copy;
  let pipelined = (sflags & RS_USES_PIPELINED_TEXEL1) != 0u && !is_copy;
  var tc: array<vec3<i32>, 4>;
  var s_offset = 0;
  if (is_copy) {
    let dxc = select(end_x - xw, xw - start_x, flip);
    s_offset = dxc - (dxc & i32(U.dx_mask));
    tc[0] = (stzw.xyw + sdx * ((dxc >> U.dx_shift) * dir)) >> vec3<u32>(16u);
  } else {
    let stw = stzw.xyw + sdx * dxq + (sdx >> vec3<u32>(HL)) * dxr;
    tc[0] = stw >> vec3<u32>(16u);
    tc[1] = (stw + dir * sdx) >> vec3<u32>(16u);
    tc[2] = (stw + (dstzw_dy.xyw & vec3<i32>(~0x7FFF))) >> vec3<u32>(16u);
    tc[3] = (stw + dir * sdx) >> vec3<u32>(16u);
//#if HD
//#else
    if (pipelined) {
      if (x == select(start_x, end_x, flip) && sp_lodlength >= 8 && (spans[SP + 15u] & 1u) != 0u) {
        tc[3] = attr_base(P, P_STZW, P_DSTZW_DE, P_DSTZW_DY, P_DSTZW_DX, dy + 1, i32(spans[SP + 14u] & 0xFFu), do_offset).xyw >> vec3<u32>(16u);
      }
    }
//#endif
  }
  var stv: array<vec2<i32>, 4>;
  var persp_overflow = false;
  for (var i = 0u; i < 4u; i++) {
    stv[i] = tc[i].xy;
    let need = i == 0u || (i == 3u && pipelined) || (i != 3u && uses_lod);
    if (perspective && need) {
      let r = perspective_divide(tc[i].x, tc[i].y, tc[i].z);
      stv[i] = r.xy;
      if (i < 3u && r.z != 0) { persp_overflow = true; }
    }
  }

  if (is_copy) {
    let st = clamp(stv[0], vec2<i32>(-0x8000), vec2<i32>(0x7FFF));
    let texel = sample_texture_copy(TS + (setup_tile & 7u) * 8u, TM, st.x, st.y, s_offset, tlut);
    if ((sflags & RS_ALPHA_TEST) != 0u && U.fb_size == 2u && (texel & 1) == 0) { return; }
    copy_pipeline(u32(texel));
    return;
  }

  // shade
  let drgba_dx = vec4<i32>(prims[P + P_DRGBA_DX], prims[P + P_DRGBA_DX + 1u], prims[P + P_DRGBA_DX + 2u], prims[P + P_DRGBA_DX + 3u]);
  let drgba_dy = vec4<i32>(prims[P + P_DRGBA_DY], prims[P + P_DRGBA_DY + 1u], prims[P + P_DRGBA_DY + 2u], prims[P + P_DRGBA_DY + 3u]);
  var rgba = attr_base(P, P_RGBA, P_DRGBA_DE, P_DRGBA_DY, P_DRGBA_DX, dy, xfrac, do_offset);
  let cdx = drgba_dx & vec4<i32>(~0x1F);
  rgba += cdx * dxq + (cdx >> vec4<u32>(HL)) * dxr;
  var sn = ((rgba >> vec4<u32>(14u)) << vec4<u32>(2u)) + xoff * (drgba_dx >> vec4<u32>(14u + HL)) + yoff * (drgba_dy >> vec4<u32>(14u + HL));
  sn = extractBits(sn, 0u, 16u) >> vec4<u32>(4u);
  let shade = clamp(extractBits(sn - vec4<i32>(0x80), 0u, 9u) + vec4<i32>(0x80), vec4<i32>(0), vec4<i32>(0xFF));

  var z = stzw.z + dstzw_dx.z * dxq + (dstzw_dx.z >> HL) * dxr;
  let snz = ((z >> 10u) << 2u) + xoff * (dstzw_dx.z >> (10u + HL)) + yoff * (dstzw_dy.z >> (10u + HL));
  z = clamp_z(snz >> 5u);

  // textures
  var tile0 = setup_tile & 7u; var tile1 = (tile0 + 1u) & 7u; let max_level = setup_tile >> 3u;
  let dzw = u32(prims[P + P_DZ]);
  var lod_frac = 0;
  if (uses_lod) {
    let l = compute_lod_2cycle(tile0, tile1, max_level, i32(dzw >> 24u), stv[0], stv[1], stv[2], persp_overflow,
                               (sflags & RS_TEX_LOD) != 0u, (sflags & RS_SHARPEN) != 0u, (sflags & RS_DETAIL) != 0u);
    tile0 = l.tile0; tile1 = l.tile1; lod_frac = l.lod_frac;
  }
  let c0w = prims[P + P_CONV0]; let c1w = prims[P + P_CONV1];
  let factors = vec4<i32>(extractBits(c0w, 0u, 16u), c0w >> 16u, extractBits(c1w, 0u, 16u), c1w >> 16u);
  let sample_quad = (sflags & RS_SAMPLE_QUAD) != 0u; let mid_texel = (sflags & RS_MID_TEXEL) != 0u;
  let convert_one = (sflags & RS_CONVERT_ONE) != 0u; let bilerp0 = (sflags & RS_BILERP0) != 0u; let bilerp1 = (sflags & RS_BILERP1) != 0u;
  let uses_t0 = (sflags & RS_USES_TEXEL0) != 0u; let uses_t1 = (sflags & RS_USES_TEXEL1) != 0u || pipelined;
  // Keep the divider's 17-bit coordinates for LOD; saturate to signed 16-bit
  // only for texture sampling, before tile shifts can wrap the result.
  var st_a = clamp(stv[0], vec2<i32>(-0x8000), vec2<i32>(0x7FFF));
  var st_b = clamp(stv[3], vec2<i32>(-0x8000), vec2<i32>(0x7FFF));
//#if HD
  // Flat primitives: stay inside the coordinates the primitive samples at native resolution (see st_bounds in rdp.c).
  if ((pflags & PF_ST_CLAMP) != 0u) {
    st_a = clamp(st_a, vec2<i32>(prims[P + P_STLO], prims[P + P_STLO + 1u]), vec2<i32>(prims[P + P_STHI], prims[P + P_STHI + 1u]));
    st_b += st_a - stv[0];
    // A filtered rectangle remembers where it had to do that. Often the next sample exists after all - in the tile
    // drawn beside it - and the video output pass can then finish the interpolation across the joint.
    if (HL != 0u && (setup_flags & SETUP_RECT) != 0u && (sflags & RS_SAMPLE_QUAD) != 0u && U.fb_fmt == FB_5551) {
      let held_s = st_a.x != stv[0].x; let held_t = st_a.y != stv[0].y;
      var held_x = held_s; var held_y = held_t;
      if (dstzw_dx.x == 0) { held_x = held_t; held_y = held_s; }       // (flipped rectangle: s runs down the screen)
      px_tag_next = TAG_FLAT;
      if (held_x) { px_tag_next |= TAG_HX; }
      if (held_y) { px_tag_next |= TAG_HY; }
    }
  }
//#endif
  var tex: array<vec4<i32>, 2>;
  tex[0] = vec4<i32>(0); tex[1] = vec4<i32>(0);
  for (var c = 0u; c < 2u; c++) {
    if (c == 0u) { if (!uses_t0) { continue; } }
    else {
      if (!uses_t1) { continue; }
      if (convert_one && !bilerp1) { tex[1] = texture_convert_factors(tex[0], factors); continue; }
    }
    let tile = select(tile0, select(tile1, tile0, pipelined), c == 1u);
    let stc = select(st_a, st_b, c == 1u && pipelined);
    tex[c] = sample_texture(TS + tile * 8u, TM, stc.x, stc.y, tlut, tlut_type, sample_quad, mid_texel, convert_one && c == 1u,
                            select(bilerp0, bilerp1, c == 1u), factors, tex[0]);
  }

  let dither = tables[ST + 5u];
  let dith = dither_coefficients(x, y >> select(0u, 1u, (sflags & RS_INTERLACE_FIELD) != 0u), dither >> 2u, dither & 3u);
  let rgb_dith = dith.x; let alpha_dith = dith.y;

  // colour combiner (cycle 0 only runs in 2-cycle mode; cycle 1 is the final one)
  let cvg_times_alpha = (sflags & RS_CVG_TIMES_ALPHA) != 0u; let alpha_cvg_select = (sflags & RS_ALPHA_CVG_SELECT) != 0u;
  let alpha_test = (sflags & RS_ALPHA_TEST) != 0u;
  let multi = (sflags & RS_MULTI_CYCLE) != 0u;
  inp.shade = shade; inp.combined = pipeline_combined; inp.texel0 = tex[0]; inp.texel1 = tex[1]; inp.lod_frac = lod_frac;
  inp.noise = i32(((px_noise & 7u) << 6u) | 0x20u);
  var alpha_reference = 0;
  var combined = vec4<i32>(0);
  for (var c = select(1u, 0u, multi); c < 2u; c++) {
    let cb = P + P_CONST + c * 4u;
    inp.c_muladd = u32(prims[cb]); inp.c_mulsub = u32(prims[cb + 1u]); inp.c_mul = u32(prims[cb + 2u]); inp.c_add = u32(prims[cb + 3u]);
    let o = combiner_equation(tables[ST + 1u + c * 2u], tables[ST + 2u + c * 2u]);
    if (c == 0u) {
      if (preview_cycle0) { pipeline_combined = o & vec4<i32>(511); return; }
      if (alpha_test) {
        let cal = clamp9(o.w);
        var ea = cal + ((cal + 1) >> 8u);
        if (alpha_cvg_select) { ea = select(coverage_count << 5u, (ea * coverage_count + 4) >> 3u, cvg_times_alpha); }
        else { ea += alpha_dith; }
        alpha_reference = clamp(ea, 0, 0xFF);
      }
      inp.combined = o; inp.texel0 = tex[1]; inp.texel1 = tex[0];
      if ((sflags & RS_NOISE_DUAL) != 0u) { reseed_noise(u32(x + 1023), u32(y + 7), seq + 11u); inp.noise = i32(((px_noise & 7u) << 6u) | 0x20u); }
    } else { combined = o; }
  }
  pipeline_combined = combined & vec4<i32>(511);
  combined = clamp(extractBits(combined - vec4<i32>(0x80), 0u, 9u) + vec4<i32>(0x80), vec4<i32>(0), vec4<i32>(0xFF));
  var key_alpha = 255;
  if ((sflags & RS_KEY) != 0u) {
    for (var i = 0u; i < 3u; i++) {
      var k = sx(key_rgb[i], 17u);
      if (k > 0) { k = -k + select(0, 16, (k & 15) == 8); }
      key_alpha = min(key_alpha, i32(tables[ST + 10u + i] << 4u) + k);
      combined[i] = clamp9(key_bypass[i]);
    }
    key_alpha = max(key_alpha, 0);
  }
  {
    var ea = combined.w + ((combined.w + 1) >> 8u);
    var ma: i32;
    if (cvg_times_alpha) { ma = (ea * coverage_count + 4) >> 3u; coverage_count = ma >> 5u; }
    else { ma = coverage_count << 5u; }
    if (alpha_cvg_select) { ea = ma; } else if ((sflags & RS_KEY) != 0u) { ea = key_alpha; } else { ea += alpha_dith; }
    combined.w = clamp(ea, 0, 0xFF);
  }
  if (!multi) { alpha_reference = combined.w; }

  var alpha_pass = true;
  let blendc = u32(prims[P + P_BLEND]);
  if (alpha_test) {
    let threshold = select(ca(blendc), i32(px_noise & 0xFFu), (sflags & RS_ALPHA_TEST_DITHER) != 0u);
    if (alpha_reference < threshold) { alpha_pass = false; }
  }
  let shade_alpha = min(shade.w + alpha_dith, 0xFF);

  // depth / blend
  let db = tables[ST + 6u];
  let force_blend = (db & DB_FORCE_BLEND) != 0u; let z_compare = (db & DB_DEPTH_TEST) != 0u; let z_update = (db & DB_DEPTH_UPDATE) != 0u;
  let image_read = (db & DB_IMAGE_READ) != 0u; let color_on_cvg = (db & DB_COLOR_ON_CVG) != 0u; let db_aa = (db & DB_AA) != 0u;
  let memory_coverage = select(0xE0, px_color.w & 0xE0, image_read);
  var memory: vec4<i32>;
  if (U.fb_fmt == FB_5551) { memory = vec4<i32>(px_color.xyz & vec3<i32>(0xF8), memory_coverage); }
  else if (U.fb_fmt == FB_IA88) { memory = vec4<i32>(px_color.xxx, memory_coverage); }
  else { memory = vec4<i32>(px_color.xyz, memory_coverage); }
  if (!image_read) { memory = vec4<i32>(select(pipeline_memory.xyz, pipeline_pre_memory.xyz, multi), memory_coverage); }
  if (multi) { pipeline_pre_memory = memory; }
  let previous_memory = pipeline_memory;
  pipeline_memory = memory;
  if (!coverage_pass || !alpha_pass || (aa_enable && coverage_count == 0)) { return; }
  let mem_cov = memory_coverage >> 5u;
  let dz = i32(dzw & 0xFFFFu); let dzc = i32((dzw >> 16u) & 0xFFu);
  let cvgz = tables[ST + 9u];
  let dr = depth_test(z, dz, dzc, px_depth, i32(px_dz), coverage_count, mem_cov, z_compare, (cvgz >> 8u) & 3u, force_blend, db_aa);
  coverage_count = dr.coverage_count;
  if (dr.ok && (!db_aa || coverage_count != 0)) {
    var pixel = combined;
    let fog = u32(prims[P + P_FOG]);
    var rgb = vec3<i32>(0);
    let first_cycle = select(1u, 0u, (db & DB_MULTI_CYCLE) != 0u);
    for (var c = first_cycle; c < 2u; c++) {
      let r = blender(pixel, select(previous_memory, memory, c == 1u), fog, blendc, shade_alpha, tables[ST + 7u + c - first_cycle], force_blend, dr.blend_en, color_on_cvg,
                      dr.coverage_wrap, dr.shift_x, dr.shift_y, c == 1u);
      if (c == 0u) { pixel = vec4<i32>(r, pixel.w); } else { rgb = r; }
    }
    if ((db & DB_DITHER) != 0u) { rgb = rgb_dither(rgb, rgb_dith); }
    let new_cov = blend_coverage(coverage_count, mem_cov, dr.blend_en, cvgz & 3u);
//#if HD
    // Blending keeps the tags of the picture underneath where that picture is what remains visible: under the transparent
    // part of a sprite (nothing changed), and under a mostly transparent rectangle such as a fade or a dimmed panel.
    let same = ((rgb.x ^ px_color.x) | (rgb.y ^ px_color.y) | (rgb.z ^ px_color.z)) & 0xF8;
    if ((force_blend || dr.blend_en) && (same == 0 || ((setup_flags & SETUP_RECT) != 0u && combined.w < 0x80))) { px_tag_next = px_tag; }
//#endif
    write_color(vec4<i32>(rgb, new_cov << 5u));
    if (z_update) {
      px_depth = z_compress(z); px_dz = u32(dzc); px_depth_dirty = true;
      if (U.fb_alias != 0u) { alias_depth_to_color(); }
    } else if (U.fb_alias != 0u) { alias_color_to_depth(); }
  }
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {
  let tx = wid.x >> HL; let ty = wid.y >> HL;
  if (tx >= U.tiles_x) { return; }
  let tile = tx + ty * U.tiles_x;
  if (tile >= U.ntiles) { return; }
  let count = bins[U.ntiles + tile];
  if (count == 0u || gid.x >= (U.fb_width << HL) || gid.y >= (U.fb_height << HL)) { return; }
  let x = i32(gid.x); let y = i32(gid.y);
  let xn = x >> HL; let yn = y >> HL;
  load_feedback();
  px_noise = 0u;
  px_load(gid.x, gid.y);
  let list = 2u * U.ntiles + bins[tile];
  for (var i = 0u; i < count; i++) {
    let P = bins[list + i] * PRIM_WORDS;
    let ylo = prims[P + P_YLO];
    if (yn < ylo || yn > prims[P + P_YHI]) { continue; }
    let SP = (u32(prims[P + P_SPAN]) + u32(yn - ylo)) * 8u;
    let sf = spans[SP + 7u];
//#if HD
    // cheap reject against the line's conservative range, then the exact sub-pixel span
    if ((sf & 2u) == 0u || xn < i32((sf >> 2u) & 0x1FFFu) || xn > i32((sf >> 15u) & 0x1FFFu)) { continue; }
    if ((u32(prims[P + P_FLAGS]) & SETUP_FILL_COPY) != 0u) {
      let sx = spans[SP + 4u];                       // whole pixels: the emulated line's span
      if ((sf & 1u) == 0u || xn < i32(sx & 0xFFFFu) || xn > i32(sx >> 16u)) { continue; }
      load_span(SP);
    } else if (!compute_span(P, y) || x < sp_start_x || x > sp_end_x) { continue; }
//#else
    // cheap reject: nothing (coverage, fill or copy) can touch a pixel outside the line's [start_x, end_x]
    let sx = spans[SP + 4u];
    if ((sf & 1u) == 0u || x < i32(sx & 0xFFFFu) || x > i32(sx >> 16u)) { continue; }
    load_span(SP);
//#endif
    shade_and_blend(P, SP, x, y, false);
    if (prims[P + P_TAIL] == ((yn << 16u) | xn) && (x & HM) == 0 && (y & HM) == 0) { store_feedback(12u + (P / PRIM_WORDS) * 12u); }
  }
  px_store(gid.x, gid.y);
}

// A separate dispatch reduces one designated tail per primitive. No unordered
// invocation writes the persistent registers consumed by the next batch.
fn load_feedback() {
  pipeline_combined = vec4<i32>(feedback[0], feedback[1], feedback[2], feedback[3]);
  pipeline_memory = vec4<i32>(feedback[4], feedback[5], feedback[6], feedback[7]);
  pipeline_pre_memory = vec4<i32>(feedback[8], feedback[9], feedback[10], feedback[11]);
}
fn store_feedback(offset: u32) {
  for (var k = 0u; k < 4u; k++) { feedback[offset + k] = pipeline_combined[k]; feedback[offset + 4u + k] = pipeline_memory[k]; feedback[offset + 8u + k] = pipeline_pre_memory[k]; }
}
@compute @workgroup_size(1)
fn resolve_feedback() {
  load_feedback();
  for (var i = 0u; i < U.num_prims; i++) {
    if (prims[i * PRIM_WORDS + P_TAIL] < 0) { continue; }
    let ST = u32(prims[i * PRIM_WORDS + P_STATE]) * 16u;
    let image_read = (tables[ST + 6u] & DB_IMAGE_READ) != 0u;
    for (var k = 0u; k < 4u; k++) {
      pipeline_combined[k] = feedback[12u + i * 12u + k];
      if (image_read || k == 3u) { pipeline_memory[k] = feedback[16u + i * 12u + k]; }
    }
  }
  store_feedback(0u);
}
// Draw dependent batches in primitive/span/pixel order, keeping the logical X
// for texturing while px_load/store wrap the linear physical RDRAM address.
@compute @workgroup_size(1)
fn ordered_main() {
  load_feedback(); px_noise = 0u;
  for (var i = 0u; i < U.num_prims; i++) {
    let P = i * PRIM_WORDS;
    let ylo = prims[P + P_YLO]; let yhi = prims[P + P_YHI];
    let flip = (u32(prims[P + P_FLAGS]) & SETUP_FLIP) != 0u;
    for (var y = ylo * HS; y < (yhi + 1) * HS; y++) {
      let SP = (u32(prims[P + P_SPAN]) + u32((y >> HL) - ylo)) * 8u;
      if ((spans[SP + 7u] & 1u) == 0u) { continue; }
      load_span(SP);
      var x0 = sp_start_x * HS; var x1 = (sp_end_x + 1) * HS - 1;
//#if HD
      if ((u32(prims[P + P_FLAGS]) & SETUP_FILL_COPY) == 0u) {
        if (!compute_span(P, y)) { continue; }
        x0 = sp_start_x; x1 = sp_end_x;
      }
//#endif
      let step = select(-1, 1, flip);
      for (var x = select(x1, x0, flip); x >= x0 && x <= x1; x += step) {
        px_load(u32(x), u32(y)); shade_and_blend(P, SP, x, y, false); px_store(u32(x), u32(y));
      }
    }
    let ST = u32(prims[P + P_STATE]) * 16u;
    let tail = prims[P + P_TAIL];
    if ((tables[ST] & RS_MULTI_CYCLE) != 0u && tail >= 0) {
      let y = tail >> 16u; let x = (tail & 65535) + select(-1, 1, flip);
      let SP = (u32(prims[P + P_SPAN]) + u32(y - ylo)) * 8u;
      load_span(SP);
      shade_and_blend(P, SP, x * HS, y * HS, true);
    }
  }
  store_feedback(0u);
}
