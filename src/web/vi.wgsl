// N64 Video Interface on the GPU. Reads the emulated framebuffer straight from GPU "RDRAM" and reproduces what
// the VI DAC puts on the wire: coverage-driven anti-aliasing, dither filter, divot, bilinear scale-out, gamma.
//   pass 1 (fs_aa)    : fetch + AA / dither filter at framebuffer resolution  -> rgba8uint (rgb + coverage)
//   pass 2 (fs_scale) : divot + scale + gamma to the 640-wide output field    -> rgba8unorm (fields woven)
//   pass 3 (fs_blit)  : presentation to the canvas

// With HD defined the passes run on the high-resolution framebuffer copy (2^HL sub-pixels per axis).
//#if HD
const HL: u32 = __HD_L__u;
//#else
const HL: u32 = 0u;
//#endif
const HS: i32 = 1i << HL;
const HM: i32 = HS - 1i;
const FBK: u32 = __FB_MASK__u;

struct VI {
  status: u32, origin: u32, width: u32, h_start: i32,
  h_res: i32, v_start: i32, v_res: i32, h_start_clamp: i32,
  h_end_clamp: i32, x_start: i32, x_add: i32, y_start: i32,
  y_add: i32, max_x: i32, max_y: i32, serrate: u32,
  field: u32, out_h: u32, valid: u32, frame: u32,
  rdram_mask: u32, weave: u32, pad0: u32, pad1: u32,
  // presentation: destination rectangle in clip space, source size, flags
  rect: vec4<f32>,       // destination rectangle (clip space: x0, y0, x1, y1)
  src: vec4<f32>,        // u_max, v_max, sharp flag, unused
  scale: vec4<f32>,      // integer pre-scale x, y
};
@group(0) @binding(0) var<uniform> V: VI;
@group(0) @binding(1) var<storage, read> fb: array<u32>;
@group(0) @binding(2) var aa_tex: texture_2d<u32>;
@group(0) @binding(3) var out_tex: texture_2d<f32>;
@group(0) @binding(4) var out_smp: sampler;

@vertex fn vs_full(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

//#if HD
const TAG_FLAT = 0x40000u; const TAG_HX = 0x80000u; const TAG_HY = 0x100000u;     // see rdp.wgsl
fn hd_word(off: u32, sub: u32) -> u32 { return fb[(((off + (V.origin >> 1u)) & (V.rdram_mask >> 1u) & FBK) << (2u * HL)) + sub]; }
fn rgb5(w: u32) -> vec3<i32> { return vec3<i32>(i32((w >> 8u) & 0xF8u), i32((w >> 3u) & 0xF8u), i32((w << 2u) & 0xF8u)); }
//#endif
fn vi_fetch(xh: i32, yh: i32) -> vec4<u32> {
  let x = xh >> HL; let y = yh >> HL;
  let sub = u32(((yh & HM) << HL) | (xh & HM));
  let off = u32(y * i32(V.width) + x);
  var c: vec4<u32>;
  if ((V.status & 3u) == 3u) {
    let lin = ((off + (V.origin >> 2u)) & (V.rdram_mask >> 2u)) * 2u;
    let hi = fb[((lin & FBK) << (2u * HL)) + sub] & 0xFFFFu; let lo = fb[(((lin + 1u) & FBK) << (2u * HL)) + sub] & 0xFFFFu;
    c = vec4<u32>(hi >> 8u, hi & 0xFFu, lo >> 8u, (lo >> 5u) & 7u);
  } else {
    let w = fb[(((off + (V.origin >> 1u)) & (V.rdram_mask >> 1u) & FBK) << (2u * HL)) + sub];
    c = vec4<u32>((w >> 8u) & 0xF8u, (w >> 3u) & 0xF8u, (w << 2u) & 0xF8u, ((w & 1u) << 2u) | ((w >> 16u) & 3u));
//#if HD
    // Joints between texture rectangles. A rectangle cannot filter past its last sample, so the sub-pixels after it were
    // drawn with that sample held (rdp.wgsl tags them). If the emulated pixel beside or below was drawn by another
    // filtered rectangle - the next tile of the same picture, typically - the missing interpolation is done here, between
    // the two pixels, with the weights and the three-point rule the texture filter itself uses.
    let fx = xh & HM; let fy = yh & HM;
    let hx = (w & TAG_HX) != 0u && fx != 0 && x + 1 < i32(V.width); let hy = (w & TAG_HY) != 0u && fy != 0;
    if (hx || hy) {
      let wr = hd_word(off + 1u, u32(fy << HL)); let wd = hd_word(off + V.width, u32(fx));
      let okr = hx && (wr & TAG_FLAT) != 0u; let okd = hy && (wd & TAG_FLAT) != 0u;
      let c00 = vec3<i32>(c.xyz);
      var c10 = c00; var c01 = c00;
      if (okr) { c10 = rgb5(wr); }
      if (okd) { c01 = rgb5(wd); }
      var c11 = c10 + c01 - c00;
      if (okr && okd) { let wq = hd_word(off + V.width + 1u, 0u); if ((wq & TAG_FLAT) != 0u) { c11 = rgb5(wq); } }
      var ax = 0; var ay = 0;
      if (hx) { ax = fx; }
      if (hy) { ay = fy; }
      var o: vec3<i32>;
      if (ax + ay <= HS) { o = c00 * HS + (c10 - c00) * ax + (c01 - c00) * ay; }
      else { o = c11 * HS + (c01 - c11) * (HS - ax) + (c10 - c11) * (HS - ay); }
      c = vec4<u32>(vec3<u32>(clamp((o + vec3<i32>(HS >> 1u)) >> vec3<u32>(HL), vec3<i32>(0), vec3<i32>(0xFF))), c.w);
    }
//#endif
  }
  if ((V.status & 0x300u) >= 0x200u) { c.w = 7u; }
  return c;
}

@fragment fn fs_aa(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<u32> {
  let x = i32(pos.x) - HS; let y = i32(pos.y);
  let mid = vi_fetch(x, y);
  if (mid.w != 7u) {
    let m = vec3<i32>(mid.xyz);
    var lo = m; var hi = m; var slo = m; var shi = m;
    for (var k = 0; k < 6; k++) {
      var d: vec2<i32>;
      switch (k) { case 0: { d = vec2<i32>(-1, -1); } case 1: { d = vec2<i32>(1, -1); } case 2: { d = vec2<i32>(-2, 0); }
                   case 3: { d = vec2<i32>(2, 0); } case 4: { d = vec2<i32>(-1, 1); } default: { d = vec2<i32>(1, 1); } }
      let n = vi_fetch(x + d.x, y + d.y);
      if (n.w == 7u) {
        let cc = vec3<i32>(n.xyz);
        slo = min(slo, max(cc, lo));
        shi = max(shi, min(cc, hi));
        lo = min(lo, cc); hi = max(hi, cc);
      }
    }
    let offset = slo + shi - (m << vec3<u32>(1u));
    let col = (m + ((offset * (7 - i32(mid.w)) + vec3<i32>(4)) >> vec3<u32>(3u))) & vec3<i32>(0xFF);
    return vec4<u32>(vec3<u32>(col), mid.w);
  }
  if ((V.status & 0x10000u) != 0u) {
    let m = vec3<i32>(mid.xyz); let t = m >> vec3<u32>(3u);
    var acc = vec3<i32>(0);
    for (var dy = -1; dy <= 1; dy++) { for (var dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) { continue; }
      let n = vec3<i32>(vi_fetch(x + dx, y + dy).xyz >> vec3<u32>(3u)) - t;
      acc += clamp(n, vec3<i32>(-1), vec3<i32>(1));
    } }
    return vec4<u32>(vec3<u32>(((m & vec3<i32>(0xF8)) + acc) & vec3<i32>(0xFF)), mid.w);
  }
  return mid;
}

fn fin(x: i32, y: i32) -> vec3<i32> {
  let mid = textureLoad(aa_tex, vec2<i32>(x + HS, y), 0);
  if ((V.status & 0x10u) != 0u) {
    let l = textureLoad(aa_tex, vec2<i32>(x + HS - 1, y), 0); let r = textureLoad(aa_tex, vec2<i32>(x + HS + 1, y), 0);
    if ((l.w & mid.w & r.w) != 7u) {
      let a = vec3<i32>(l.xyz); let b = vec3<i32>(mid.xyz); let c = vec3<i32>(r.xyz);
      return max(min(a, b), min(max(a, b), c));
    }
  }
  return vec3<i32>(mid.xyz);
}

fn isqrt(v: u32) -> u32 {
  var r = u32(sqrt(f32(v)));
  if (r * r > v) { r -= 1u; }
  if ((r + 1u) * (r + 1u) <= v) { r += 1u; }
  return r;
}

@fragment fn fs_scale(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let ox = i32(pos.x); var fy = i32(pos.y);
  if (V.serrate != 0u && V.weave != 0u) {
    let line = fy >> HL;
    if ((line & 1) != select(0, 1, V.field == 0u)) { discard; }
    fy = ((line >> 1u) << HL) | (fy & HM);
  }
  let black = vec4<f32>(0.0, 0.0, 0.0, 1.0);
  if (V.valid == 0u) { return black; }
  let cy = fy - V.v_start * HS;
  if (cy < 0 || cy >= V.v_res * HS || ox < V.h_start_clamp * HS || ox >= V.h_end_clamp * HS) { return black; }
  let y = cy * V.y_add + V.y_start * HS;
  let by = y >> 10u; let yf = (y >> 5u) & 31;
  let cx = ox - V.h_start * HS;
  let x = cx * V.x_add + V.x_start * HS;
  let bx = x >> 10u;
  var rgb = fin(bx, by);
  if ((V.status & 0x300u) < 0x300u) {
    let xf = (x >> 5u) & 31;
    let c10 = fin(bx + 1, by); let c01 = fin(bx, by + 1); let c11 = fin(bx + 1, by + 1);
    let a = (rgb + (((c01 - rgb) * yf + vec3<i32>(16)) >> vec3<u32>(5u))) & vec3<i32>(0xFF);
    let b = (c10 + (((c11 - c10) * yf + vec3<i32>(16)) >> vec3<u32>(5u))) & vec3<i32>(0xFF);
    rgb = (a + (((b - a) * xf + vec3<i32>(16)) >> vec3<u32>(5u))) & vec3<i32>(0xFF);
  }
  let gamma = (V.status & 8u) != 0u; let gamma_dither = (V.status & 4u) != 0u;
  var noise = 0u;
  if (gamma_dither) {
    let NP = 1103515245u;
    var s = vec3<u32>(u32(cx), u32(cy), V.frame);
    s = ((s >> vec3<u32>(8u)) ^ s.yzx) * NP;
    s = ((s >> vec3<u32>(8u)) ^ s.yzx) * NP;
    s = ((s >> vec3<u32>(8u)) ^ s.yzx) * NP;
    noise = (s.x >> 16u) & 0xFFFFu;
  }
  var o = vec3<u32>(rgb);
  if (gamma) {
    if (gamma_dither) {
      let n = vec3<u32>(noise & 0x3Fu, (noise >> 6u) & 0x3Fu, ((noise >> 9u) & 0x38u) | (noise & 7u));
      let v = (o << vec3<u32>(6u)) + n;
      o = vec3<u32>(2u * isqrt(v.x), 2u * isqrt(v.y), 2u * isqrt(v.z)) & vec3<u32>(0xFFu);
    } else {
      o = vec3<u32>(2u * isqrt(o.x << 6u), 2u * isqrt(o.y << 6u), 2u * isqrt(o.z << 6u)) & vec3<u32>(0xFFu);
    }
  } else if (gamma_dither) {
    o = min(o + ((vec3<u32>(noise) >> vec3<u32>(0u, 1u, 2u)) & vec3<u32>(1u)), vec3<u32>(255u));
  }
  return vec4<f32>(vec3<f32>(o) / 255.0, 1.0);
}

// ---- presentation ---------------------------------------------------------------------------------------
struct BlitOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };
@vertex fn vs_blit(@builtin(vertex_index) i: u32) -> BlitOut {
  let p = vec2<f32>(f32(i & 1u), f32((i >> 1u) & 1u));
  var o: BlitOut;
  o.pos = vec4<f32>(mix(V.rect.xy, V.rect.zw, p), 0.0, 1.0);
  o.uv = vec2<f32>(p.x, 1.0 - p.y) * V.src.xy;
  return o;
}
@fragment fn fs_blit(i: BlitOut) -> @location(0) vec4<f32> {
  var uv = i.uv;
  if (V.src.z > 0.5) {
    // "sharp bilinear": integer pre-scale followed by linear filtering keeps pixels crisp without shimmer
    let size = vec2<f32>(textureDimensions(out_tex));
    let t = uv * size;
    let region = vec2<f32>(0.5) - vec2<f32>(0.5) / max(V.scale.xy, vec2<f32>(1.0));
    let d = fract(t) - vec2<f32>(0.5);
    let f = (d - clamp(d, -region, region)) * max(V.scale.xy, vec2<f32>(1.0)) + vec2<f32>(0.5);
    uv = (floor(t) + f) / size;
  }
  return vec4<f32>(textureSampleLevel(out_tex, out_smp, uv, 0.0).rgb, 1.0);
}
