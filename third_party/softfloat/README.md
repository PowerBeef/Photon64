# Photon64 SoftFloat subset

Upstream: Berkeley SoftFloat Release 3e, https://www.jhauser.us/arithmetic/SoftFloat-3e.zip.
Archive SHA256: `21130ce885d35c1fe73fc1e1bf2244178167e05c6747cad5f450cc991714c746`.

The 50 upstream C files and support headers are unmodified. `photon.c` selects f32/f64 arithmetic, square root and signed integer conversion dependencies with portable 64-bit primitives. `platform.h` is the local configuration; no host fenv or 128-bit compiler runtime is required. The core is little-endian on native and WASM hosts. `COPYING.txt` contains the BSD 3-Clause notice and is embedded in the HTML build.

`src/cpu.c` translates VR4300 rounding modes, legacy NaN polarity, E traps, FS underflow defaults and integer range restrictions. SoftFloat's IEEE policy is not used as a replacement for the processor's rules. `tools/fpu_vectors.py` computes independent exact-rational results rather than calling this library.
