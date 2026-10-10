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
output = pathlib.Path(sys.argv[2])
output.write_text(text)
