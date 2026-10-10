# Photon64 implementation plan

Implementation of the new audit is tracked in [the 10 October implementation report](reports/implementation-2026-10-10/REPORT.md), with per-finding status, evidence and device-dependent exit gates.
The [10 October remaining-defect audit](reports/audit-2026-10-10/REPORT.md) contains the newer, code-grounded implementation sequence for source `96a7f07`, including save integrity, GPU lifecycle, CPU faults, and current CI gaps. The milestone history below is retained with its original scope.

Initial source: `ab28c09f4b059239c6467cabbe683701cc86ce4b`, matching the attached 9 October audit. Status incorporates the five-area implementation published at `8146e70`, its hosted browser follow-up at `b2d0455`, and the renderer implementation at `d741f41` with passing hosted baseline/browser validation. Work follows the repository's main-only policy. Priorities are grounded in executable source and measured results.

| Stage | Work | Acceptance | Status |
|---|---|---|---|
| 1: Trustworthy verdicts | F01: structured PASS/FAIL/SKIP, process and parser status, complete checkpoint sets, framebuffer color/hidden/depth differences, fixed screenshot thresholds | Negative fixtures fail standalone and composed gates; clean fixtures pass | Implemented; regression tests added |
| 1: Protect progress | F02: transaction outcomes, save write generations, retryable failures, atomic states and metadata, temporary-storage labels | Abort retains battery dirty state; old commit cannot clear new writes; state abort commits neither item | Implemented; source-level failure tests |
| 1: Session ownership | F03: queue ROM/state/reset/import/home actions; capture identity; validate candidates before mutation; one reusable cartridge allocation | Real WASM A/B loads and cache records agree; invalid replacement preserves the machine; duplicate states do not stop it | Implemented; focused tests, broader browser lifecycle matrix pending |
| 1: Native safety | F04 and JoyBus follow-up: bounded output pitch, explicit format checks, aligned guest source, zero-length command rejection | ASan/UBSan width/format boundaries; no adjacent command dispatch | Implemented; native regressions |
| 2: Defined integer operations | F05: unsigned CPU wrap/shifts, branch displacement multiplication, widened RDP edges and unsigned packed spans | Same exact semantic vectors in native sanitizers and O0/O3 WASM | Implemented for reproduced cases |
| 2: Exception semantics | F07: masked bus stores with write translation and original virtual fault address | Missing/invalid/read-only mappings, all alignments, delay-slot EPC/BD | Implemented; independent byte expectations |
| 2: FPU | F06: portable bit-exact arithmetic, directed rounding and exception wrapper | Exact rational/isqrt vectors for four rounding modes, NaNs/subnormals/conversions/traps in native/WASM | Implemented with SoftFloat 3e and 3628 independent vectors; broader hardware conformance remains open |
| 2: GPU coherence | F08/F09: rejected mappings/submissions cannot acknowledge; cleanup and reset generations; full chunked range scans; fail-stop device loss | Injected map/submit failures, old callbacks, capacity tails; actual backend integration | Implemented; fake-device tests and available adapter probes |
| 2: Bounded imports | F10: size checks before file reads; counted, cancelable ZIP/gzip decoding; local/central records and CRC | Oversized file never read; over-budget stream canceled; malformed/CRC inputs rejected | Implemented; focused tests; extended fuzz corpus pending |
| 3: Repeatable development | F11/F12: pinned SDK digest and oracle revision, portable native build, Node/npm/Python versions, validation script and CI | Rebuild with documented commands; required lanes fail when unavailable; record fixture/artifact hashes | Implemented; local and hosted baseline/browser checks pass; exact raw and independent VI gates required, legacy mixed-stage failures retained as a diagnostic |
| 3: Provenance | F13: align MIT metadata, document external formulation and homebrew fixtures; bundle/source hashes | License manifest agrees; commercial ROMs remain local | Implemented documentation; upstream fixture license chain still requires verification |
| 4: Compatibility and performance | Device/browser matrix, save reload/device loss, recorded commercial inputs, accuracy comparisons, audio/controller/mobile tests, memory/frame pacing | Named workload and device evidence for each support claim | Open; headless smoke evidence alone is insufficient |

## Decisions and limits

- Queue policy is invocation order. A queued state action for a game that has already changed is discarded. UI pause intent is independent of temporary operation suspension.
- Durable storage is `true`, temporary storage is `'temporary'`, transaction failure is `false`. Failed autosaves retain bytes and retry after a five-second delay. Explicit transitions try again and refuse to leave on a failed battery write. Temporary sessions allow play and advertise export.
- A dirty word is now a nonzero write generation. Every bus write increments it; only the successful matching snapshot clears it. The counter is unchanged in size, but save states are guarded by the WASM hash and older builds' states will be rejected.
- ROM magic and size are checked in JavaScript before mutation, storage reads precede commit, and a single 64 MiB cartridge allocation is reused. This trades a bounded 64 MiB reserve for predictable replacement safety and no per-load heap accumulation. The existing static core and GPU scratch also consume memory.
- GPU loss pauses the session and explains reset/reopen; switching to software cannot recover GPU-only bytes. Battery export remains available. No transparent checkpoint recovery is claimed.
- Reference screenshot checks are coarse content checks with fixed 4% green, 6% red/text tolerances and a 20-pixel floor. They are not a pixel-accuracy oracle. Existing mismatches must remain visible.
- CPU memory accesses and PCs remain 32-bit, CACHE remains a no-op, and timing remains configurable constant CPI. No broad accuracy claim is introduced.
- Guest FPU rounding now uses pinned SoftFloat 3e, with independent exact-rational/isqrt expected results. Finite vector coverage does not certify every VR4300 hardware behavior.

## Next implementation order

Items 1–5 from the accuracy investigation are implemented; see `RENDERER_ACCURACY_IMPLEMENTATION.md` for the actual code paths, reference policy and acceptance evidence. The remaining priorities are:

1. Broaden independent two-cycle conformance: next-pixel alpha comparison, cycle-one texel replacement/LOD, span-boundary feedback and retained depth/blender shifts. Keep hardware-derived expectations distinct from the pinned reference profile.
2. Profile ordered GPU dispatch and optimize dependent batches while retaining exact native/HD-at-1x parity. Validate native, software fallback, HD scale changes and state restore on a physical adapter.
3. Expand TMEM and VI edge-case vectors beyond the passing five CPU-drawn fixture sequences and authored key/format streams. Extend touched-range/memory ownership coverage and GPU gameplay observation beyond the last target.
4. Extend the FPU corpus with hardware-derived VR4300 trap/NaN/flush vectors; investigate CACHE, address-width and timing accuracy separately.
5. Extend browser allocation/load/home/state failure permutations and physical mobile/controller/audio tests.
6. Extend the six recorded gameplay scenarios and profile frame pacing, long tasks, audio underruns, readback costs and memory by scale. Consider workers after measurements identify UI-thread stalls.

## Single-pass follow-up

1. Renderer: sign-extend texture coordinates at the shift stage in C/WGSL; use a named, reproducible random-bit adapter and command-granular reference execution. Keep all counted residual color/depth mismatches failing.
2. FPU: portable SoftFloat f32/f64 arithmetic and conversions, all four guest rounding modes, VR4300 legacy NaN/flush/trap policy. Validate against independently generated Fraction/isqrt vectors in native sanitizer and WASM O0/O3 builds. Hardware certification remains separate.
3. Browser: stop on synchronous presentation errors, release held keyboard/touch input on focus/page loss, suppress hidden-page fields, reject synchronization/reset on lost devices. Hosted desktop/mobile-emulated fault injection covers quota/unavailable storage and presentation recovery; real Dawn destruction covers the device callback.
4. Games: add Smash match and World Driver Quick Race scripts; extend every local lane to recorded checkpoints and preserve failures. ROM payloads stay local. A scripted segment is not whole-game compatibility.
5. Performance: conservative wrapped source/target overlap permits larger software batches. VI SIMD candidates matched images but were rejected for inconsistent timing benefits. Compare fixed artifacts, final machine/image/audio hashes and named profiles; shared-host wall time requires qualification.

## Renderer accuracy follow-up

Implemented signed 16-bit saturation between the 17-bit perspective divider and tile coordinate shifts in C/WGSL. LOD retains the wider coordinates. Independent coordinate vectors and guest-store/DMA synchronization tests run in the required native build. The reference adapter observes CPU-owned hidden bits as well as command order; old stale-memory depth counts are superseded. Exact 1x HD comparisons now reject reduced diagnostic storage, which aliases addresses. The later ordered-feedback/keying implementation closes the counted differences in all five native recorded replays; `VALIDATION_REPORT.md` preserves before/after evidence and the remaining conformance limits.
