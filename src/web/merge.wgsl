// Masked upload of CPU-side framebuffer writes into GPU RDRAM (only entries flagged valid are written).
// With HD defined the value is also replicated into every sub-pixel of the high-resolution copy.
//#if HD
const HL: u32 = __HD_L__u;
const FBK: u32 = __FB_MASK__u;
//#endif
struct M { offset: u32, count: u32, mask: u32, pad: u32 };
@group(0) @binding(0) var<uniform> m: M;
@group(0) @binding(1) var<storage, read_write> fb: array<u32>;
@group(0) @binding(2) var<storage, read> stage: array<u32>;
//#if HD
@group(0) @binding(3) var<storage, read_write> hd: array<u32>;
//#endif
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  if (g.x >= m.count) { return; }
  let v = stage[g.x];
  if ((v & 0x80000000u) != 0u) {
    let k = (m.offset + g.x) & m.mask;
    fb[k] = v & 0x3FFFFu;
//#if HD
    let n = 1u << (2u * HL);
    for (var i = 0u; i < n; i++) { hd[(k & FBK) * n + i] = v & 0x3FFFFu; }
//#endif
  }
}

//#if HD
// (re)initialise the whole high-resolution copy from the emulated-resolution GPU memory
@compute @workgroup_size(256)
fn fill(@builtin(global_invocation_id) g: vec3<u32>) {
  if (g.x >= m.count) { return; }
  let k = (m.offset + g.x) & m.mask;
  let v = fb[k];
  let n = 1u << (2u * HL);
  for (var i = 0u; i < n; i++) { hd[(k & FBK) * n + i] = v; }
}
//#endif
