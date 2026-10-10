# Photon64 audit implementation: changes, acceptance and remaining gates

**Date:** 10 October 2026. **Frozen audit:** [`96a7f07`](../audit-2026-10-10/REPORT.md). **Implementation commits:** `6d8a9e1` (correctness/product/CI), `228a4db` (Safari journal regression/five-sequence gate), `6cb5ecf` (GPU validation ownership/recovery/docs), `1e8c2ea` (browser save-generation regression). Subsequent source/evidence changes are recorded in git history and individual CI records. This report tracks implementation; it does not overwrite the original audit or reinterpret its failures as passes.

## Delivery status

The reproduced save/reset/state, cartridge identity/selection, GPU range/lifecycle, CPU exception/privilege and signed-packing defects have source fixes and regression coverage. Product work adds audio recovery, four controllers, cross-tab ownership, save-hardware override with backup recovery, portable states/battery metadata, storage usage and diagnostic export. CI requires an actual software-adapter WebGPU lane, direct-file testing and a named nine-job release matrix. Native Safari now has bounded phase journaling and a five-clean-sequence gate.

**The entire multi-week hardware research program is not complete.** Physical iOS/Android/GPU/audio/assistive-device checks, independent RDP boundary and FPU/cache/timing/64-bit conformance, dependency-safe renderer optimization and unresolved upstream provenance need evidence beyond this environment. They remain open explicitly. No result below certifies full-game compatibility, full machine-state conformance or hardware accuracy.

## Finding-by-finding implementation

| Finding | Change | Acceptance / status |
|---|---|---|
| F01 reset dirty progress | `sys_reset` preserves dirty state; frontend reset drains durable writes and aborts on failed storage | Actual-WASM dirty/reset/reopen, held/aborted transactions and native reset assertions pass. Fixed within tested scope. |
| F02 state battery persistence | Host battery revision survives guest restoration; restore drains older writes and creates a fresh dirty obligation | Actual-WASM `0x11 → 0x22 → restore → reopen` and delayed-write regressions pass. Fixed. |
| F03 circular GPU ownership | C watch/stale/dirty and JS upload/touched/readback ranges split at the boundary, normalize offsets and handle full spans | Native tracking, mocked negative-apron/readback, real native/HD-at-1x boundary drawing and CPU partial-store lifecycle pass. Independent hardware boundary behavior is unresolved; see new boundary finding below. |
| F04 obsolete scale failure | Frontend request/session/renderer guards; serialized lower-layer activation and cancellation | Controlled completion/failure order regression passes; stale callbacks cannot reset a newer selection. Physical 2x/4x pressure testing remains open. |
| F05 GPU allocation cleanup | Explicit partial-build ownership, disposable renderer and `finally` cleanup for temporary mapping | Injected pipeline/map failures destroy resources; old-generation readbacks cannot apply. Real device loss passes. |
| F06 cartridge collision | Streaming canonical SHA-256 identities, separate metadata, verified transactional legacy copies | Independent Node digest across byte orders, same-header mutations, separate cache/state keys and abort/retry migration pass. Ambiguous migration requires explicit battery import. |
| F07 read-order inversion | Selection token precedes read/decode; file reads serialize; obsolete selections cannot mutate sessions | Delayed A/B file regression passes; public callers are snapshotted, internal owned readers avoid another full ROM copy. |
| F08 merge exception address | Translate original virtual address while reading the aligned word | All merge alignments, invalid/missing TLB and delay-slot EPC/BD tests pass native and WASM O0/O3. |
| F09 privileged segments | User/supervisor segment permissions enforced in fast and slow data/fetch paths | Forbidden loads/stores/fetch and allowed supervisor segment tests pass. Full 64-bit addressing/cache/timing remains a separate program. |
| F10 packing UB | Cast bytes/fields to unsigned before high shifts; zero-length boot copy guarded | Actual ROM/peripheral high-bit inputs and ASan/UBSan baseline pass. |
| F11 asynchronous GPU errors | Early loss/uncaptured handlers, immediate submit-scope pop, first-cause latch, bounded event ring, validation-tail barrier | Rejected mapping/submission/validation never acknowledges; device loss and diagnostic export pass. The user’s physical-device incident remains unassigned. |
| F12 GPU CI absent | Mandatory `gpu` job: actual WGSL native/HD-at-1x vectors, deliberate loss and continuous authored core/GPU transitions | Hosted GPU job passed on implementation revisions. Zero drawing now fails lifecycle coverage. Required exact job set includes this lane. |
| F13 Safari/direct-file | Incremental command journal, global and per-request budgets, short failure cleanup, desktop-alone + five full sequences; browser file-origin smoke | Safari 26.6.1 on macOS 15.7.9 passed desktop-alone and five sequences on both `228a4db` and final `1e8c2ea`. Native OS-backed read remains SKIP/NotReadableError. Original historical stall cause is not retrospectively assigned. |
| F14 ordered rendering cost | Counts, span-derived sample totals/maxima, queue completion, readback and frame percentiles; input event-to-submit timing | Instrumented. Dependency partitioning/chunking and physical performance improvement are not implemented without independent semantics and device measurements. |
| F15 runtime memory | Actual-size reusable ROM capacity, count-only slot existence, owned file candidates, serialized imports, bounded readback concurrency/pool | 8/32/64 MiB repeated reserve plateaus; fresh 8 MiB cartridge uses 72.25 MiB WASM versus old 128.25 MiB reserve. Process/GPU peak and physical memory-pressure measurements remain open. |
| F16 compatibility | ISC ares-based 6105 response with reproducible model vectors; region/revision/digest metadata; safe media override; four gamepads | Native 304 additional CIC assertions; CIC protocol expectations remain implementation-derived. Model agreement is not physical protocol or protected-checkpoint certification. Full hardware program remains open. |
| F17 product reliability | Audio retry/cleanup and counters; sibling library controls; Web Lock single writer; atomic library transactions; storage usage; portable files and backup swap | Focused 44.1/48 kHz initialization/queue, four ports/reconnect, lock conflict, portable identity/core rejection and aborted backup recovery tests pass. Physical audio, VoiceOver and eviction/device recovery sign-off remain open. |
| F18 release/provenance | Immutable action pins; exact nine-job predicate; tested artifact SHA-256/build metadata; notices copied from source SHA; indexed audit/status/capabilities/migration | Negative missing/duplicate/skipped gate tests pass. Stale old release manifest is rejected. Fixture permissions/capture settings and RDP derivation chain remain explicitly unverified in `THIRD_PARTY.md`. |

## Audit-phase acceptance map

| Phase | Delivered in this pass | Exit still requiring evidence |
|---|---|---|
| 0: evidence | Frozen audit preserved; source/core identities; first failures retained; native Safari journals and repeated sequences | Affected physical-device renderer incident and historical Safari stall are not retrospectively diagnosed. |
| 1: save integrity | Reset/state barriers, host revision, held/aborted transaction regressions, recovery/export behavior | Physical storage eviction/power-loss testing. |
| 2: identity and selection | Canonical digest, atomic verified migration, latest selection, bounded owned imports | Larger real-user legacy migrations and physical memory pressure. |
| 3: GPU ownership | Circular tracking, scale generations, partial cleanup, upload/submission validation ownership and fail-stop barriers | Independent installed-RAM boundary expectations; real 2x/4x driver pressure. |
| 4: mandatory evidence | Named GPU job, native/HD-at-1x drawing and loss, full RAM/hidden/register continuous comparisons, direct-file and browser lanes | Physical iOS/Android and native chooser coverage. |
| 5: bounded CPU defects | Unsigned assembly, original merge fault addresses and consistent segment protection, sanitizer/O0/O3 validation | Broader cache/timing/64-bit architecture remains Phase 6. |
| 6: compatibility | CIC response/model vectors, cartridge metadata and save overrides/backups, explicit conformance boundaries | Hardware CIC/FPU/renderer corpus, protected checkpoint and cache/timing/64-bit implementation. |
| 7: memory/pacing | Actual-size reusable capacity, metadata-only reads, bounded readbacks, ordered/GPU/frame/input/audio counters | Physical integrated/discrete/mobile baselines and proven safe scheduling optimization. |
| 8: product/release | Audio recovery, four ports, library semantics, single writer, backup/portable files, migration docs and exact artifact gates | Physical audio/controllers/VoiceOver; unresolved fixture/source provenance. Preview 2 selects the corrected revision with all nine jobs passed. |

## Source behavior and design choices

### Persistent progress and cartridge ownership

Reset is a save barrier. A failed durable transaction leaves the live battery bytes and dirty obligation intact and prevents replacing/resetting that progress. Imported durable battery data is adopted only after pending writes finish. Loading a machine state restores its battery and marks it dirty through a host revision that is never rolled back by the snapshot. A stale transaction cannot clear newer content.

A cartridge key is `sha256:` plus the digest of canonical big-endian bytes. Hashing yields between chunks, supports cancellation and works without WebCrypto on a file origin. The displayed title is not an identity. Legacy migration requires hashing cached old cartridge bytes; copying save/state/metadata/thumb records and the identity mapping uses one transaction, preserves originals, and aborts visibly. No guessed merge between patches/revisions occurs.

An active cartridge holds one exclusive Web Lock. A second tab is rejected before altering its current cartridge. Browser profiles/origins remain independent. Unsupported locking fails closed for durable progress writes: temporary data can still be exported. Library updates occur in a single read/write transaction. [Migration and recovery instructions](../../MIGRATION.md) explain file-origin policy, raw legacy saves, new envelopes and build-bound states.

### Renderer lifecycle and diagnostics

Circular tracking is a contract between the current core and shader. Jobs retain their exclusion/ownership ranges across both pieces of wrapped memory. Failed mapping/submission/validation cannot advance the completion acknowledgement. Stale completions are generation checked. Upload writes have their own immediately popped validation scope, so concurrent scale builds cannot absorb a runtime upload failure. Validation tails resolve to void rather than retaining nested submission histories. Allocation errors destroy partial sets; scale activation is serialized and stale frontend failures cannot alter newer settings.

Diagnostics export source SHA/dirty state, full WASM digest, browser/origin, cartridge digest/region/revision/media, adapter/scale, first cause plus bounded events, GPU counts/bytes/latencies, audio queue/counters and frame percentiles. No ROM, state or battery bytes are included. Queue completion is a host-observed measurement; field processing time is not GPU execution time. Keyboard/touch latency is event-delivery-to-next-presentation-submission, excluding pre-delivery scheduling and physical scanout. GPU timestamps, physical latency and thermal/long-session performance remain device work.

### CIC and product features

`src/cic.h` adapts the pinned ares ISC transition. `tools/cic_vectors.cpp` compiles its masked nibble model to regenerate sixteen authored expectations; the license notice is included in source and standalone output. The guest PIF challenge response is packed, with protocol reserved bytes cleared and Joybus processing suppressed for the response phase. This is independent-implementation agreement, not a new hardware-derived corpus.

Save-hardware override validates the type and creates a durable backup before changing/resetting. One backup per cartridge bounds retained storage. Restore swaps old/current payload and medium atomically; failures cannot change the active machine. Portable battery metadata checks cartridge identity while remaining usable across builds. Portable states additionally require the full core hash and exact internal layout/ROM size, bounded decompression and the same restore-save policy. They contain no cartridge bytes.

Four standard gamepads use stable index/ID slots, per-port presence/pak/rumble and disconnect clearing. Keyboard/touch remain on port 1. Real identical-controller reassignment, wireless reconnect, browser permission and haptic behavior still need the physical matrix. Audio has a serialized initialization promise, closes partial graphs on failure, permits retries and surfaces underrun/overrun/output-rate/queue diagnostics. These checks do not replace listening or headset interruption tests.

## Executed acceptance evidence

| Lane | Observation | Scope |
|---|---|---|
| Local JavaScript | 15 legacy validation checks + 68 Node tests pass | Actual WASM persistence/memory plus mocked browser/GPU failure ordering. Final CI can include later added regressions. |
| Native / WASM baseline | 11,912 core assertions; ASan/UBSan; identical O0/O3; 958,565 oracle coordinate/write assertions pass | Five independent raw fixtures have zero differences; five VI fixtures each compare 95 fields/29,184,000 RGB pixels with zero differences under declared reference policy. |
| Real GPU vectors | Fourteen batches native and full-storage HD-at-1x pass; twelve retained registers per batch | Twelve independently compared stream batches plus two C/WGSL-only circular boundary batches. Not a full machine-state comparison. |
| Real device loss | Loss barrier/reset reject as required | Dawn + Mesa llvmpipe software Vulkan, not physical driver coverage. |
| Authored continuous lifecycle | 180 fields, 3,752 GPU batches, 2,264,924,160 compared RAM/hidden bytes, state restore and reset pass | Native and full-storage HD-at-1x. Real drawing, CPU reads and partial writes. Hidden comparison uses effective ownership/shadow semantics, not stale backing bytes. |
| Hosted final `1e8c2ea` | All nine required jobs passed, including six desktop/mobile browser variants and native Safari | Exact source `1e8c2ea`, run `38087805779`. Final status/build hashes retained in `evidence/ci-final-status.json` and `evidence/tested-build-info.json`; earlier `228a4db` status remains. |

The [evidence directory](evidence/) retains synthetic/homebrew logs and CI status. Commercial-derived results remain private after automatic publication review rejected public disclosure; source changes and noncommercial evidence are unaffected. Commercial cartridge inventory, hashes and execution evidence are retained privately and excluded from public repository artifacts. Baseline and initial GPU checks were taken during source development with dirty working state; individual logs record source file/core hashes. Hosted validation is the exact-commit authority. A later frontend-only change does not reclassify an earlier C baseline as a fresh full run.

## New boundary finding retained during implementation

Adding framebuffer/depth targets at `0x7ffffc`/`0x7ffff8` exposed a difference between Photon64's existing 8 MiB circular addressing and the pinned reference's 24-bit address mask plus installed-RAM rejection. The explicit diagnostic reports **29 touched color, 13 depth and 17 hidden differences** and exits nonzero. The historical independent corpus still requires zero differences; its thresholds/expectations were not weakened. The new boundary streams are separate and labelled C/WGSL coherence, not independent accuracy.

Reproduce with `RDP_BOUNDARY_DIAGNOSTIC=1 ./out/oracle_test`. The first new Safari journal attempt also failed because a numeric WebDriver script timeout was treated as script text; `safari.test.mjs` reproduces the journal request and the corrected revision passed five native sequences. This known harness error is retained separately and does not diagnose the historical Safari stall.

The retained [failure log](evidence/boundary-reference.log) is an open hardware-conformance finding. Resolve it from permitted hardware/address-window evidence before changing the physical model. A coherent pair of implementations can agree on an inaccurate contract.

## Remaining complete-plan gates

These are implementation dependencies, not authorization requests. [CAPABILITIES.md](../../CAPABILITIES.md) provides the scoped conformance sequence.

1. **Physical incident and browser matrix.** Reproduce the same affected-device checkpoint/inputs at native/2x/4x on the affected device, export the first cause and adapter, and compare desktop integrated/discrete plus physical iOS/iPadOS/Android. Exercise actual mobile/native filesystem choosers, file and localhost origins, 200% text, VoiceOver and safe areas. Record hardware/OS/browser/source and first attempts, without uploading ROM bytes.
2. **Independent boundary/renderer corpus.** Resolve installed-RAM wrapping; obtain licensed expectations for next-pixel alpha/texel replacement/LOD, retained depth/blender shifts, TMEM and VI edge transitions. Feed authored commands to both native and full-storage HD-at-1x. Any zero-render, absent comparison or mismatch fails acceptance.
3. **CIC/peripheral protected scenarios.** Record protocol vectors separately from the ares model, then demonstrate a permitted protected cartridge checkpoint with controlled initial saves and input script. Test EEPROM/SRAM/Flash/all Paks over real browser power cycles and overrides; retain aborted-write cases.
4. **CPU hardware program.** Hardware-derived FPU trap/NaN/flush expectations precede broader claims. Design/test cache aliasing, LL/SC/CP0, RSP/DMA scheduling and 64-bit translation in separate milestones with native/O0/O3 equivalence. The functional constant-CPI 32-bit core remains the declared implementation.
5. **Measured performance architecture.** Capture physical median/p95/p99/max field, queue/input/audio latency and process/GPU memory with 8/32/64 MiB cartridges, ZIP, four states, scale transitions and long sessions. Partition only provably independent work; chunk ordered execution while retaining feedback, then rerun all exact lanes. Consider workers/OffscreenCanvas only if these baselines justify it and standalone/Safari behavior survives.
6. **Provenance and publication.** Obtain fixture permissions/capture policy and finish source-derivation review; no author correspondence or new license grant has been invented. Keep existing notices and exclude external oracle/commercial data. Publish a new immutable preview only from an explicitly selected nine-job-green revision with exact artifact hashes and accurate platform/migration/known-issue notes. Failed or incomplete candidates remain unpublished.

## Reproduction commands

```sh
. out/env.sh
npm run validate
npm test
npm run test:roms
# Select an installed adapter; this generated path is workspace-specific.
VK_ICD_FILENAMES="$PWD/.tools/lvp_icd.json" node tools/gpu_lifecycle.mjs
VK_ICD_FILENAMES="$PWD/.tools/lvp_icd.json" GPU_LIFECYCLE_HD=1 node tools/gpu_lifecycle.mjs
RDP_BOUNDARY_DIAGNOSTIC=1 ./out/oracle_test # deliberately nonzero while disagreement is open
```

`tools/filecheck.mjs` runs only on an authorized browser runner; the local Chromium restriction in `AGENTS.md` is unchanged. Native Safari uses Apple's driver on the disposable macOS CI runner. Device permissions and physical evidence are never substituted with viewport emulation or lavapipe timing.

## Final-candidate Safari save-generation regression

Candidate `6cb5ecf` passed eight of nine required CI jobs in run `38087127723`; native Safari failed its third sequence with battery byte `0` instead of `90`. The test directly changed EEPROM but assigned dirty generation `1`, reusing the restored state's generation while an older autosave could still be pending. That violates the guest's nonzero monotonic write-generation contract. A held-transaction actual-WASM regression reproduces the same assertion; its first failure is retained in `evidence/safari-generation-reproduction.log`. The harnesses now increment the generation exactly as guest EEPROM writes do. The corrected regression retains the byte-for-byte durable reload assertion; all 68 Node tests pass. The failed CI attempt is retained; the corrected source `1e8c2ea` passed every required job in run `38087805779`, including desktop-alone and five complete native Safari sequences.

## Selected release artifact

Preview `v1.0.0-preview.2` selects validated source `1e8c2ea4019a152db0c17dc77ae77ef7a1bf4e38`, run `38087805779`. The release manifest/documentation commit is separate from the tested production source. It does not rebuild or substitute the app.

- Tested HTML: 462,470 bytes; SHA-256 `6f1f23cc08b5cf675eee2756adbaa7c57d923be12c716ff7d2024ca6e96699ab`.
- Tested WASM: SHA-256 `cb6420e43762fb00c7c2d1d2bc385de02b017e43e14c34510944ad8e8bb92092`, identical to the local private replays.
- The publication workflow rejects missing/skipped/duplicate jobs, a dirty tested build, incorrect source, changed HTML or mismatched embedded/tested WASM. Notices come from the selected source SHA.

The original Safari journal failure, save-generation aliasing failure, intentionally failing boundary diagnostic and optional legacy mixed-stage screenshot failures remain visible. They are not erased by the final green required matrix. Physical/hardware/provenance work listed above remains open.

## Replay-tool warm-up follow-up

The optional `GPU_WARMUP_SNAPSHOT=1` strategy removes duplicate software warm-up work by seeding the second instance from the same checkpoint. It does not reduce the target checkpoint or comparison fields, change inputs or weaken comparisons. Default dual-instance warm-up remains available. An actual-WASM regression compares the seeded full static state with independent replay, then checks further input-driven fields. Local JavaScript coverage is now 69 tests; the already published preview's production source remains the nine-job-green `1e8c2ea` with 68 tests at that revision. The seeded authored lane passed locally with 180 fields and 3,738 actual GPU batches; CI now also requires it. The production app is unchanged by this tooling follow-up.
