# Photon64 implementation plan

Source: `ab28c09f4b059239c6467cabbe683701cc86ce4b`, matching the attached 9 October audit. Work follows the repository's main-only policy. Priorities are grounded in executable source, not the report's severity labels alone.

| Stage | Work | Acceptance | Status |
|---|---|---|---|
| 1: Trustworthy verdicts | F01: structured PASS/FAIL/SKIP, process and parser status, complete checkpoint sets, framebuffer color/hidden/depth differences, fixed screenshot thresholds | Negative fixtures fail standalone and composed gates; clean fixtures pass | Implemented; regression tests added |
| 1: Protect progress | F02: transaction outcomes, save write generations, retryable failures, atomic states and metadata, temporary-storage labels | Abort retains battery dirty state; old commit cannot clear new writes; state abort commits neither item | Implemented; source-level failure tests |
| 1: Session ownership | F03: queue ROM/state/reset/import/home actions; capture identity; validate candidates before mutation; one reusable cartridge allocation | Real WASM A/B loads and cache records agree; invalid replacement preserves the machine; duplicate states do not stop it | Implemented; focused tests, broader browser lifecycle matrix pending |
| 1: Native safety | F04 and JoyBus follow-up: bounded output pitch, explicit format checks, aligned guest source, zero-length command rejection | ASan/UBSan width/format boundaries; no adjacent command dispatch | Implemented; native regressions |
| 2: Defined integer operations | F05: unsigned CPU wrap/shifts, branch displacement multiplication, widened RDP edges and unsigned packed spans | Same exact semantic vectors in native sanitizers and O0/O3 WASM | Implemented for reproduced cases |
| 2: Exception semantics | F07: masked bus stores with write translation and original virtual fault address | Missing/invalid/read-only mappings, all alignments, delay-slot EPC/BD | Implemented; independent byte expectations |
| 2: FPU | F06: distinguish infinite inputs from finite overflow; then bit-exact arithmetic and rounding oracle | Infinity vectors pass native/WASM; all four rounding modes, NaNs/subnormals/conversions/traps require differential corpus | Infinity classification implemented; directed rounding and full conformance remain open |
| 2: GPU coherence | F08/F09: rejected mappings/submissions cannot acknowledge; cleanup and reset generations; full chunked range scans; fail-stop device loss | Injected map/submit failures, old callbacks, capacity tails; actual backend integration | Implemented; fake-device tests and available adapter probes |
| 2: Bounded imports | F10: size checks before file reads; counted, cancelable ZIP/gzip decoding; local/central records and CRC | Oversized file never read; over-budget stream canceled; malformed/CRC inputs rejected | Implemented; focused tests; extended fuzz corpus pending |
| 3: Repeatable development | F11/F12: pinned SDK digest and oracle revision, portable native build, Node/npm/Python versions, validation script and CI | Rebuild with documented commands; required lanes fail when unavailable; record fixture/artifact hashes | Implemented; local and hosted baseline/browser evidence recorded; reference-image gate remains failing |
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
- Full FPU directed rounding is a separate implementation milestone. Host fenv alone does not solve WASM arithmetic. Choose and pin a bit-exact oracle before optimizing that path.

## Next implementation order

1. Investigate actual Angrylion and shipped-reference differences by first failing command/frame; preserve zero-tolerance verdicts and fix source semantics, not expected results.
2. Pin a trusted software FP oracle and establish the report's half-ULP vectors as expected failures in a separate conformance lane. Implement rounding/exception rules with exact bits and flags across native/WASM.
3. Extend browser tests for durable reload, write failure UI/export, load/home/state permutations, device loss, and allocation failure. Validate at least one physical adapter and mobile browser before claiming support.
4. Add recorded-input coverage for Smash and World Driver Championship, then profile frame pacing, long tasks, audio underruns, readback costs and memory by scale. Consider workers only after evidence identifies UI-thread stalls.
