# Renderer accuracy implementation

Implemented against the investigation baseline `ae97e914f9a2c6ff6a3e5c1286b5f90bd40a1083`, using the pinned Angrylion RDP Plus revision in `tools/versions.env`. This supersedes the implementation proposals in `ACCURACY_INVESTIGATION.md`; its original traces and altered-reference causal probes remain historical evidence. Acceptance uses the standard generated guest-write/noise-zero adapter, not the altered-reference probes. Hardware accuracy remains a separate claim.

## Changes

| Item | Implementation | Verification |
|---|---|---|
| Row aliases | Software traverses logical spans without clipping X to framebuffer stride. Traversal respects flip. Batches with overlapping physical rows use ordered WGSL dispatch; conservative height/watch/readback ranges include the aliased rows. Logical texture coordinates remain unchanged. | Authored narrow-stride fill and descending-triangle vectors; complete Mario Kart replay; native and full-storage HD-at-1x GPU checkpoints. |
| Raw and VI fixture stages | `cmp_raw_ref.py` compares exact decoded RGB dimensions/pixels against each fixture's exact filename. `vitest.c` compares VI RGB with the independent reference for the guest register/field sequence. Both are required by `npm run validate`. | All five raw fixtures match. Each VI fixture compares 95 rendered fields and 29,184,000 RGB pixels with zero differences. |
| Pipeline feedback | Persistent COMBINED, memory and pre-memory registers; zero-coverage/rejected samples update retained values. Two-cycle blending reads retained memory before installing the current sample. Primitive-tail cycle-zero lookahead preserves the next unwritten sample. GPU batches with dependencies execute in order; independent batches retain parallel rasterization and reduce designated primitive tails in a separate dispatch. | Independent synthetic command stream and commercial replays; GPU vectors compare framebuffer/hidden data and all twelve retained registers across batches. |
| Observer coverage | Before each SYNC_FULL resynchronization, compare all potentially touched target halfwords, including earlier/offscreen targets, row overshoots and wrapping addresses. Color byte masks distinguish small formats; depth is tracked when used; effective hidden bits are compared separately. All counted discrepancies fail the oracle. | Mutation tests retain earlier-byte, RAM-end-wrap and hidden-only failures. Authored I4, I8, IA16, RGBA16/32 and separate-depth command cases. |
| Chroma key alpha | Decode key enable; snapshot key widths and invalidate state on SetKey. Evaluate signed 17-bit combiner fractions, positive half-step tie, minimum channel alpha and bypass RGB. Coverage multiplication uses the combiner alpha; coverage selection overrides key alpha. Native and WGSL follow the same semantics. | 368,640 independent reference checks across 512 input sets, four coverage modes, nine coverage values, four alphas and five width sets; one/two-cycle GPU command cases. |

Software fallback transitions request coherent GPU readback before low-format drawing. The next GPU batch uploads software-produced feedback. A separate hidden-bit dirty tag ensures repeated data with changed RDP hidden bits is uploaded without applying the CPU-write LSB rule. Feedback snapshots use aligned offsets and obey existing reset-generation/failure ordering.

The GPU lane adds a seventh storage binding and retains 48 bytes of pipeline state plus primitive-tail records per resolution. Ordered dispatch deliberately favors correctness and can be substantially slower for dependent/two-cycle batches. `stats.orderedBatches` exposes this scheduling cost; no physical-device FPS claim is made. Display-only 2x/4x feedback is an approximation; exact claims below concern native or HD-at-1x with full backing storage.

## Validation

Final results are recorded in `VALIDATION_REPORT.md`. Commands below produce ignored local outputs; proprietary cartridges and reference source are not committed.

```sh
. out/env.sh
npm run validate
npm run test:roms
./out/oracle_nn "roms/Mario Kart 64 (USA).z64" 3300 -i "$(cat tools/inputs/mk64.txt)"
./out/oracle_nn "roms/Perfect Dark (USA) (Rev 1).z64" 6500 -i "$(cat tools/inputs/pd.txt)"
./out/oracle_nn "roms/World Driver Championship (USA).z64" 16762 -i "$(cat tools/inputs/wdc.txt)"
node tools/dawntest.mjs testroms/RSPCP2VRCP.N64 1 "0" "" --fn tools/rdp_gpu_vectors.js
node tools/dawntest.mjs testroms/RSPCP2VRCP.N64 1 "0" "" --fn tools/rdp_gpu_vectors.js --hd 0
```

On this Linux host, Dawn uses `VK_ICD_FILENAMES="$PWD/.tools/lvp_icd.json"`. The native build generates `out/rdp-vectors.json` from authored commands; the GPU runner never uploads the software expectations. Its verdict is a separate synthetic lane, not a manufactured VI or machine-state comparison.

The legacy `cmp_ref.py` retains its exact thresholds and reports the same five failures. It compares raw reference PNGs to VI scanout and therefore is retained as a visibly failing diagnostic, with its log uploaded by CI. Required fixture correctness now uses the two independently validated output stages above. No legacy threshold or reference image was changed. The workflow's diagnostic step uses `continue-on-error`; required baseline gates still fail on any actual mismatch.

## Reference policy and remaining boundaries

The pinned reference's I8 fill expression shifts right by the byte-position number and then left by three, rather than shifting right by eight times the position. The small-format synthetic case exposed this behavior. The software fallback follows that pinned comparison profile explicitly; it is not evidence that the same expression describes console hardware. I4 fill pipeline-crash behavior is outside the added valid shading cases.

The observer records intervals of potential primitive writes, not a complete machine-memory transaction trace. GPU gameplay checks still inspect the last batch target and three execution counters, rather than full CPU/FPU/RSP state. The synthetic GPU register checks broaden that lane only for the authored streams. Noise-zero retains the combiner bias `0x20` and does not validate hardware randomness.

Next work should prioritize independent two-cycle boundary vectors for next-pixel alpha comparison, cycle-one texel replacement/LOD selection and retained depth/blender shifts; broader TMEM and VI corner cases; performance profiling/optimization of ordered dispatch; and physical GPU/mobile/controller/audio coverage. CACHE, 32-bit PC/address modeling and constant-CPI timing remain unchanged. Full game compatibility or hardware certification is not inferred from bounded zero-difference replays.
