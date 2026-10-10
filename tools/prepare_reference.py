#!/usr/bin/env python3
"""Prepare an explicit diagnostic random-bit policy without modifying the reference checkout."""
import pathlib
import sys
source = pathlib.Path(sys.argv[1]) / 'src/core/n64video.c'
text = source.read_text()
needle = 'static STRICTINLINE uint32_t irand(uint32_t* state)\n{'
if text.count(needle) != 1:
    raise SystemExit('reference random helper changed; review the adapter')
# Both renderers retain the combiner noise bias 0x20; only random bits are zero.
text = text.replace(needle, needle + '\n#ifdef NOISE_ZERO\n    (void)state; return 0;\n#endif')
text += '''
// The standalone differential harness observes guest stores. The reference
// plugin has no CPU write callback of its own; mark the affected hidden bits
// clean just as initially CPU-owned RDRAM is initialized by rdram_init().
void photon_reference_cpu_write(unsigned pa, unsigned len) {
    for (unsigned i = 0; i < len; i++)
        rdram_hidden[((pa + i) & (RDRAM_MAX_SIZE - 1)) >> 1] |= HB_CLEAN;
}
#ifdef RDP_REFERENCE_TEST
// Independent keying vectors exercise the reference combiner, bypass RGB,
// coverage multiplication/selection and alpha-dither interaction together.
void photon_reference_key(const int *a, const int *b, const int *c, const int *d,
                          const unsigned *width, int alpha, int flags, int *out, unsigned *coverage) {
    struct rdp_state w = {0};
    w.other_modes.key_en = 1;
    w.other_modes.cvg_times_alpha = flags & 1;
    w.other_modes.alpha_cvg_select = (flags >> 1) & 1;
    w.key_width.r = width[0]; w.key_width.g = width[1]; w.key_width.b = width[2];
    w.combiner_rgbsub_a_r[1] = (int*)&a[0]; w.combiner_rgbsub_a_g[1] = (int*)&a[1]; w.combiner_rgbsub_a_b[1] = (int*)&a[2];
    w.combiner_rgbsub_b_r[1] = (int*)&b[0]; w.combiner_rgbsub_b_g[1] = (int*)&b[1]; w.combiner_rgbsub_b_b[1] = (int*)&b[2];
    w.combiner_rgbmul_r[1] = (int*)&c[0]; w.combiner_rgbmul_g[1] = (int*)&c[1]; w.combiner_rgbmul_b[1] = (int*)&c[2];
    w.combiner_rgbadd_r[1] = (int*)&d[0]; w.combiner_rgbadd_g[1] = (int*)&d[1]; w.combiner_rgbadd_b[1] = (int*)&d[2];
    w.combiner_alphasub_a[1] = w.combiner_alphasub_b[1] = w.combiner_alphamul[1] = &zero_color;
    w.combiner_alphaadd[1] = &alpha;
    combiner_1cycle(&w, 5, coverage);
    out[0] = w.pixel_color.r; out[1] = w.pixel_color.g; out[2] = w.pixel_color.b; out[3] = w.pixel_color.a;
}
// Test-only exports of the pinned reference's divider and sampling clamp.
void photon_reference_coords(int s, int t, int w, int *os, int *ot, int *overflow) {
    static int initialized;
    if (!initialized) { tcoord_init_lut(); initialized = 1; }
    tcdiv_persp(s, t, w, os, ot);
    *overflow = ((*os | *ot) & 0x60000) != 0;
    tclod_tcclamp(os, ot);
    *os = (int16_t)*os; *ot = (int16_t)*ot;
}
#endif
unsigned photon_reference_hidden(unsigned pa) {
    uint16_t value; uint8_t hidden;
    rdram_read_pair16(&value, &hidden, pa >> 1); return hidden;
}
'''
output = pathlib.Path(sys.argv[2])
output.write_text(text)
