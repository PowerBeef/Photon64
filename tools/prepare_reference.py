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
// Test-only exports of the pinned reference's divider and sampling clamp.
void photon_reference_coords(int s, int t, int w, int *os, int *ot, int *overflow) {
    static int initialized;
    if (!initialized) { tcoord_init_lut(); initialized = 1; }
    tcdiv_persp(s, t, w, os, ot);
    *overflow = ((*os | *ot) & 0x60000) != 0;
    tclod_tcclamp(os, ot);
    *os = (int16_t)*os; *ot = (int16_t)*ot;
}
unsigned photon_reference_hidden(unsigned pa) {
    uint16_t value; uint8_t hidden;
    rdram_read_pair16(&value, &hidden, pa >> 1); return hidden;
}
#endif
'''
output = pathlib.Path(sys.argv[2])
output.write_text(text)
