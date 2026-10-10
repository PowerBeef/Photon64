# Code-grounded development report

Audit baseline: `ab28c09f4b059239c6467cabbe683701cc86ce4b`. The checkout matched the attached report exactly. The original report remains unchanged. This document reports the implementation milestone and qualifications, not a new accuracy certification.

## Verified source findings and corrections

| Finding | Code evidence / correction | Remaining limits |
|---|---|---|
| F01 false success | `games.sh` ignored GPU/parser failure and only matched VI/sync text; `oracle.c` returned zero after discrepancies; `cmp_ref.py` reused stale paths and normalized failures against their batch. Structured verdicts, explicit exit status, checkpoint completeness, all counted framebuffer/hidden/depth comparisons, temporary screenshot outputs and fixed thresholds replace those behaviors. | GPU machine-state predicate remains three counters, framebuffer coverage remains last batch. Coarse screenshot content counts are not pixel accuracy. |
| F02 lost saves | `flushSaves` cleared dirty before commit; states/metadata wrote in separate transactions. Storage outcomes now distinguish durable, temporary and failed; dirty is a generation cleared only after matching durable commit. States, library updates and deletions use single transactions; imports check outcomes. Same-cartridge replacement reads the battery after flushing its latest writes. | Real browser quota/reload matrix is unexecuted. Temporary storage cannot survive page close; it is labelled and export is available. |
| F03 mixed sessions | ROM/state/reset/home/import crossed awaits with mutable globals. One invocation-order session queue suspends frames. State actions capture identity; ROM candidate checks and storage reads precede mutation; one bounded reusable cartridge allocation prevents invalid replacement and load-by-load heap accumulation. | Focused actual-WASM interleavings passed; broader UI/browser schedules and allocation stress remain. |
| F04 native bounds | Raw export used guest width for the host destination pitch. Destination width is capped independently, formats/dimensions are checked, guest origin is aligned and writer failures propagate. | Optional native helper only; output boundary tests do not prove all native-core safety. |
| F05 integer UB | Signed CPU wrap/shifts and branch displacement are made explicit; RDP edge products are widened before truncation. Sanitizers additionally exposed signed packed-span shifts, now cast to unsigned. | Reproduced vectors are covered; exhaustive arithmetic audit is still needed. |
| F06 FPU | Infinite operands were classified as finite overflow. Finish helpers now distinguish finite inputs; infinity arithmetic vectors pass. | Directed rounding, all exceptional values, conversions and full VR4300 FPU behavior remain open. No fenv-only workaround was introduced. |
| F07 partial stores | SWL/SWR/SDL/SDR performed guest reads before write translation. Masked bus stores validate write intent with the original VA, avoid guest MMIO reads and preserve store faults. | Missing/invalid/read-only translations, merge alignments and delay metadata covered; full address-mode conformance remains open. |
| F08 GPU readback | Map rejection removed tracking without applying bytes or failing the barrier. Pending ranges and an error latch survive failure; buffers clean up; obsolete generations cannot apply or acknowledge. Device loss and synchronous renderer initialization errors stop the session instead of silently continuing from stale memory. | No physical device-loss run or transparent checkpoint recovery. |
| F09 ranges | `syncRange` capped total work to scratch capacity. It now visits every chunk, including tail halfwords and RAM-end clipping, and rejects zero capacity. | Synthetic coverage and local software-Vulkan execution; physical large-target matrix remains open. |
| F10 allocation bounds | File size is checked before reading; counted streams cancel over-budget ZIP/gzip output. ZIP records/flags/CRC are validated before accepting a candidate. | Fuzzing, allocation peak profiling and broader malformed state structure validation remain. |
| F11 automation | Placeholder npm test and absent workflow are replaced with meaningful tests, a required baseline script and CI. | Hosted Actions were not run; branch-protection settings were not changed. The screenshot lane currently fails and remains visible. |
| F12 reproducibility | GCC vector portability and `-lm` linking are fixed; SDK digest, oracle commit, Node/Dawn/Playwright and Python dependencies are pinned. Setup was exercised successfully. Missing required references fail, and missing optional references remove stale oracle binaries. | macOS and physical device environments remain untested. |
| F13 metadata | npm license now matches root MIT; dependency and fixture provenance/uncertainties are documented in `THIRD_PARTY.md`. | Upstream fixture revisions/permissions and formulation derivation are not fully verified. |

The JoyBus follow-up is also fixed: a TX value masked to zero is rejected before adjacent bytes can become a command. The memory-view test now grows the same WASM memory instead of constructing a second factory; Expansion Pak tests assert actual field counts instead of `true == true`.

## Baseline evidence

- WASI SDK 34.0 archive SHA256 matched the official release digest. Node 24.19.0, GCC 13.3.0, Python 3.12 and the exact dependency versions are recorded in the handoff's host manifest.
- The final `npm run validate` baseline builds native/WASM and the single HTML app, runs 15 JS validation checks and 31 Node regression cases, native unit/sanitizer lanes, the same 279 core checks in O0/O3 WASM, Expansion Pak checks, deterministic bundled ROM execution, and two Python comparator tests.
- Native sanitizer lanes cover RSP 18 checks, JoyBus 17, existing CPU 8, audit semantic vectors 279, and raw output 29 boundary checks. No ASan/UBSan report appeared in these lanes. This is focused coverage, not whole-program proof.
- All six supplied commercial ROMs complete 300 fields twice with identical RDRAM/audio hashes and execution summaries; hashes match the pre-change 300-frame baseline. This is deterministic software execution evidence only.
- The real Dawn 0.6.2 + Mesa lavapipe adapter passes Mario Kart's 300-frame native and HD-at-1x checkpoint probes. Full recorded-input results are recorded separately below.
- Native 300-frame reference comparisons: Mario and Mario Kart have zero counted color/depth differences; GoldenEye has four depth mismatches; Perfect Dark has zero compared pixels and fails the required comparison lane.
- Fixed screenshot-content thresholds reject all five bundled reference cases. Geometry, field scaling and reference assumptions must be investigated; these count discrepancies do not independently prove RSP opcode failures.

## Commercial 300-field execution corpus

| Cartridge | Primitives | RDRAM hash | Audio-ring hash | Qualification |
|---|---:|---|---|---|
| Super Mario 64 | 49,917 | a4914520 | 52f2d682 | Rendered boot/title execution |
| Mario Kart 64 | 3,887 | d8dccd60 | fd5ec5ce | Rendered boot execution |
| GoldenEye 007 | 4 | ec4b583e | 5e509dc5 | Sparse rendered execution; native VI screenshot is dark |
| Perfect Dark Rev 1 | 0 | 3aec92b9 | 5e509dc5 | Boot execution only; no rendering claim |
| Super Smash Bros. | 8,462 | 56b6c45b | 2690a52b | Rendered boot execution |
| World Driver Championship | 16,378 | 653ced98 | 5e509dc5 | Rendered boot; native harness shows no Controller Pak (frontend formats it separately) |

`out/commercial-final.json` includes ROM SHA256/size, runtime, source state, exact WASM hash, both execution logs and verdict definitions. ROM payloads are excluded from the handoff and all source commits.

## Extended renderer evidence

The extended game suite used intermediate development builds while final allocator/tooling checks were completed. Its results are exploratory evidence; they are not represented as a single immutable release artifact run. The final baseline and short GPU probes are rerun against the final build. Renderer/CPU changes in this milestone do not change the recorded 300-field hashes.

- Mario: 4,200 frames with the repository's recorded inputs; Angrylion compared 147,072,000 pixels over 1,915 RDP frames with zero counted color/depth differences. Both GPU paths passed six complete checkpoints, including framebuffer/hidden/depth and the stated execution counters.
- GoldenEye: 4,600-frame independent comparison reported 1,593,029 color mismatches (0.67231%) and 5,521 depth mismatches (0.00233%). Corrected full native and HD-at-1x GPU runs each pass seven checkpoints.
- Perfect Dark: 6,500-frame independent comparison reported 227,301 color mismatches (0.06594%) and 13,002 depth mismatches (0.00377%). Corrected native and HD-at-1x GPU runs each pass ten checkpoints.
- Mario Kart: 3,300-frame independent comparison reported 114,476 color mismatches (0.11906%) and zero depth mismatches. Both GPU paths pass eight checkpoints. All five separate 4x seam heuristics pass; that diagnostic lane is not an exact rendering oracle.

The first GoldenEye GPU run and Perfect Dark native run falsely reported unused-depth differences. At GoldenEye frame 700, the last batch has no depth use and depth address zero; the test inspected stale GPU-cache copies of CPU-owned boot memory. `readFb` is a raw cache inspector. The harness now calls `syncRange` before framebuffer inspection, which uploads CPU-owned changes without replacing GPU-owned words. Both GoldenEye frame-700 probes then pass with unchanged renderer code and strict mismatch thresholds. Original failure logs are retained and superseded by the corrected logs; independent Angrylion differences above remain failures. Required gates also reject omitted framebuffer comparisons after rendering has occurred.

The composed extended suite correctly exits 1 because independent oracle comparisons fail. Superseding the GPU harness false positives does not make that suite pass. Do not interpret a missing GPU adapter, an empty comparison or a diagnostic SKIP as an exact PASS.

## Boundary of this milestone

Browser checks were prepared and Chromium installed, but the environment's approval policy rejected outside-sandbox execution, which `AGENTS.md` requires. No browser persistence, page lifecycle, physical GPU, mobile, controller or audio-output quality result is claimed. FPU directed rounding remains an explicit open correctness item. The emulator's 32-bit PC/address model, CACHE no-op and constant CPI timing are unchanged.

Use `IMPLEMENTATION_PLAN.md` for the next source changes, `DEVELOPMENT.md` for commands, and the generated handoff for logs and reviewable patches. This milestone protects progress and makes regression verdicts meaningful; existing accuracy discrepancies remain release blockers.
