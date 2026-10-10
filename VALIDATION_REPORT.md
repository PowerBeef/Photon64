# Code-grounded development report

Audit baseline: `ab28c09f4b059239c6467cabbe683701cc86ce4b`. The checkout matched the attached report exactly. The original report remains unchanged. This document reports the implementation milestone and qualifications, not a new accuracy certification.

## Verified source findings and corrections

| Finding | Code evidence / correction | Remaining limits |
|---|---|---|
| F01 false success | `games.sh` ignored GPU/parser failure and only matched VI/sync text; `oracle.c` returned zero after discrepancies; `cmp_ref.py` reused stale paths and normalized failures against their batch. Structured verdicts, explicit exit status, checkpoint completeness, all counted framebuffer/hidden/depth comparisons, temporary screenshot outputs and fixed thresholds replace those behaviors. | GPU machine-state predicate remains three counters, framebuffer coverage remains last batch. Coarse screenshot content counts are not pixel accuracy. |
| F02 lost saves | `flushSaves` cleared dirty before commit; states/metadata wrote in separate transactions. Storage outcomes now distinguish durable, temporary and failed; dirty is a generation cleared only after matching durable commit. States, library updates and deletions use single transactions; imports check outcomes. Same-cartridge replacement reads the battery after flushing its latest writes. | Hosted Chromium durable reload passed with one shipped fixture; quota/failure and broader browser matrices remain open. Temporary storage cannot survive page close; it is labelled and export is available. |
| F03 mixed sessions | ROM/state/reset/home/import crossed awaits with mutable globals. One invocation-order session queue suspends frames. State actions capture identity; ROM candidate checks and storage reads precede mutation; one bounded reusable cartridge allocation prevents invalid replacement and load-by-load heap accumulation. | Focused actual-WASM interleavings passed; broader UI/browser schedules and allocation stress remain. |
| F04 native bounds | Raw export used guest width for the host destination pitch. Destination width is capped independently, formats/dimensions are checked, guest origin is aligned and writer failures propagate. | Optional native helper only; output boundary tests do not prove all native-core safety. |
| F05 integer UB | Signed CPU wrap/shifts and branch displacement are made explicit; RDP edge products are widened before truncation. Sanitizers additionally exposed signed packed-span shifts, now cast to unsigned. | Reproduced vectors are covered; exhaustive arithmetic audit is still needed. |
| F06 FPU | Infinite operands were classified as finite overflow. Finish helpers now distinguish finite inputs; infinity arithmetic vectors pass. | Directed rounding, all exceptional values, conversions and full VR4300 FPU behavior remain open. No fenv-only workaround was introduced. |
| F07 partial stores | SWL/SWR/SDL/SDR performed guest reads before write translation. Masked bus stores validate write intent with the original VA, avoid guest MMIO reads and preserve store faults. | Missing/invalid/read-only translations, merge alignments and delay metadata covered; full address-mode conformance remains open. |
| F08 GPU readback | Map rejection removed tracking without applying bytes or failing the barrier. Pending ranges and an error latch survive failure; buffers clean up; obsolete generations cannot apply or acknowledge. Device loss and synchronous renderer initialization errors stop the session instead of silently continuing from stale memory. | No physical device-loss run or transparent checkpoint recovery. |
| F09 ranges | `syncRange` capped total work to scratch capacity. It now visits every chunk, including tail halfwords and RAM-end clipping, and rejects zero capacity. | Synthetic coverage and local software-Vulkan execution; physical large-target matrix remains open. |
| F10 allocation bounds | File size is checked before reading; counted streams cancel over-budget ZIP/gzip output. ZIP records/flags/CRC are validated before accepting a candidate. | Fuzzing, allocation peak profiling and broader malformed state structure validation remain. |
| F11 automation | Placeholder npm test and absent workflow are replaced with meaningful tests, a required baseline script and CI, including an independent browser job. | Hosted baseline validation and browser checks pass. The screenshot lane fails and remains visible; branch-protection settings were not changed. |
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

Local browser execution remains blocked: the environment's approval policy rejected outside-sandbox execution, which `AGENTS.md` requires. After GitHub publication, an independent hosted Chromium job completed the persistence/lifecycle checks described below. Physical GPU, mobile, controller and audio-output quality remain untested. FPU directed rounding remains an explicit open correctness item. The emulator's 32-bit PC/address model, CACHE no-op and constant CPI timing are unchanged.

## Hosted CI follow-up

[Actions run 38016280375](https://github.com/PowerBeef/Photon64/actions/runs/38016280375), testing `ece83bc4255e4a63c799c5b3b6dd0c7c71d309aa`, completed on 10 October UTC (9 October in Toronto):

- `npm run validate`: PASS on the hosted runner.
- Independent browser job: PASS using shipped `testroms/RSPCP2VRCP.N64` and Playwright Chromium Headless Shell 153.0.8010.12. It checked duplicate state saves, state restoration, durable battery reload, cached cartridge loading and absence of page errors. This is one software-renderer browser/storage workload, not a commercial compatibility or physical-GPU matrix.
- Shipped reference-image step: FAIL, matching the previously documented content-count discrepancies. The workflow's overall failure is retained; browser success does not supersede the accuracy failure.

The GitHub plugin published commits with new commit metadata and therefore new SHAs, while all four uploaded file-tree SHAs matched their validated local counterparts exactly. The earlier handoff and local manifests retain their original snapshot identities; this CI follow-up identifies the actual published revision independently. No commercial ROM was uploaded for CI.

Use `IMPLEMENTATION_PLAN.md` for the next source changes, `DEVELOPMENT.md` for commands, and the generated handoff for logs and reviewable patches. This milestone protects progress and makes regression verdicts meaningful; existing accuracy discrepancies remain release blockers.
# Playtest and diagnostic tooling follow-up

The tooling published at `7bed8ea0482616dc7f459b13cc1531ccb67e2765` adds persistent software-core control (`coreplay.mjs`), repeatable core+VI benchmarks (`benchmark.mjs`), named WASM CPU profiles and browser trace/profile capture. The optimized symbolized WASM was checked section by section against the production binary: all non-custom sections match. Symbol names change metadata, not executable code/data.

## Executed capability checks

| Lane | Evidence | Limit |
|---|---|---|
| Local live core play | Super Mario 64 replay to field 4200; observed tutorial pages dismissed with A; analog movement and jump observed through PNGs at fields 4418/4428/4488 | Software core; excludes frontend and physical input devices |
| Reproducible scene benchmark | Three fresh-process 4200-field Mario replays; measured fields 3800–4199; mean 26.279, 27.843, 27.116 ms/field; identical final counters and RDRAM/audio/VI hashes | Castle/tutorial scene, not whole-game FPS; shared AMD EPYC 9V74 host, Node 24.19 |
| Active movement benchmark | Extended `sm64-play.txt` replay to field 4488, measured fields 4298–4487: 23.916 and 23.682 ms/field; p95 30.396/30.041 ms, p99 35.353/37.356 ms; both final states/images/audio agree | Short 190-field run/jump segment, software rendering; not a full-game performance average |
| Separate named CPU profile | Fourth replay agrees with benchmark final hashes; sampled self time: `rdp_flush` 46.9%, `n64_vi_render` 18.3%, `sample_texture` 10.9%, `rsp_run_n` 8.7% | Whole process, including boot/warmup; not just the timed scene; profile overhead excluded from benchmark repeats |
| Local WebGPU | Dawn/lavapipe Mario Kart 300-field probe with exact checkpoints 250/299 and event trace: PASS | Software Vulkan adapter; existing last-target/counter coverage, not physical GPU performance |
| Native image/audio capture | Mario 4200 fields; 70 PNGs, one-image-per-second silent preview and 2,203,424 stereo PCM frames | Diagnostic capture, not a browser pacing/audio-device test; WAV declares the final DAC rate (32,006 Hz) |
| Hosted desktop browser | Keyboard/menu pause-resume, 120-field progression, duplicate state saves, restore, battery persistence and cached reload: PASS; no page errors | Homebrew fixture, software renderer |
| Hosted mobile-emulated browser | Same checks: PASS; no page errors | Chromium viewport/touch emulation; not Safari, iOS or Android hardware |
| Required baseline | Local `npm run validate`: PASS; hosted validation step: PASS | Independent reference-image gate still fails |

[Actions run 38017435939](https://github.com/PowerBeef/Photon64/actions/runs/38017435939) tested `7bed8ea`. Desktop job `114110740293` and mobile-emulated job `114110740234` both succeeded and uploaded traces, profiles, screenshots and result JSON. Their paced samples were approximately 60.4 and 60.1 fields/s respectively, with tracing/profiling enabled; these are fixture pacing observations, not peak emulator throughput. The desktop benchmark fixture also passed two-run determinism. The baseline job's `npm run validate` step passed, but the separate shipped reference-image comparison failed, so the overall workflow remains failing. Browser logs report no available GPU adapters; browser rendering was explicitly software.

The local npm suite at `7bed8ea` has 15 legacy checks plus 35 Node test cases. Four added cases cover controller script validation, timing summaries, and an actual-WASM persistent session with invalid-command rejection and PNG decoding. A subsequent terminal-only fix releases stdin on `quit`, with a fifth regression case for callers that keep stdin open; focused tests and a live terminal exit check passed. The next broader CI run is separate evidence from the completed run above.

## Environment boundary

Playwright MCP 0.0.83 is installed and its protocol initialization/tool listing was verified. It is not attached to this chat's tool registry. The managed browser cannot reach the workspace's loopback server and explicitly rejects `file:` navigation. Local Chromium execution remains subject to the existing outside-sandbox requirement. These restrictions were not changed. The working development arrangement is local interactive core/native/Dawn diagnostics plus an independent hosted browser runner. No physical GPU/controller, real mobile browser, speaker latency or whole-game compatibility claim follows from these results.

## Single-pass implementation follow-up (October 2026)

The five follow-up areas now have code, regression coverage and replay tooling. This is a bounded development pass, not closure of every compatibility or hardware-accuracy issue.

- **Renderer:** C/WGSL texture shift stages now sign-extend 16-bit coordinates. The external oracle consumes complete commands in order, compares each SYNC_FULL, and uses an explicit generated adapter to zero random bits on both sides without changing its pinned checkout. Earlier reports incorrectly described the stock reference's fixed random sequence as zeroed. With the corrected diagnostic policy, the intermediate post-batching build compares Mario Kart 3300 fields at 354 color / 0 depth differences over 96,153,600 pixels; GoldenEye 4600 at 33 color / 5521 depth over 236,948,400; Perfect Dark 6500 at 148265 color / 13002 depth over 344,686,080. These lanes still FAIL. Counts across different noise policies are not a pure renderer-speed or accuracy improvement measure. An exhaustive 262144-value probe agrees with the reference depth compression table; a separate dz=1 normalization discrepancy is corrected and covered.
- **FPU:** portable SoftFloat 3e replaces host arithmetic and error estimates. Guest RN/RZ/RP/RM apply to S/D operations, square root and conversions. The wrapper handles legacy NaN polarity, canonical NaNs, E traps, enabled exceptions preserving destination, FS flush defaults and post-round conversion boundaries. The independently generated Fraction/isqrt corpus has 3628 arithmetic, conversion and comparison vectors, run verbatim in native and WASM O0/O3 tests. Finite corpus coverage is not exhaustive VR4300 hardware certification. SoftFloat notices are retained in source and standalone HTML.
- **Browser/failure paths:** synchronous frame/presentation errors now stop the session; focus/page loss clears keyboard/touch inputs; hidden pages do not advance fields. Lost Dawn devices reject even empty synchronization barriers and reset. Real Dawn device destruction passes the callback/barrier/reset probe. Desktop/mobile-emulated browser checks inject quota failure, unavailable IndexedDB and presentation failure, verify dirty retention, reset recovery and invalid candidate preservation. Both expanded hosted browser lanes PASS in run 38022153286 after correcting a test selector that counted the Add game tile as a saved cartridge.
- **Games:** added recorded Smash selection/match inputs and World Driver race navigation. Smash reaches the first match and passes six native GPU/software checkpoints through 8000 fields in Dawn/lavapipe. World Driver exposed a lost RSP event when an MMIO synchronization and event consumption occur in the same cycle; the early-return scheduler fix has a focused core regression. The corrected World Driver replay reaches Hawaii Practice Laps, then shows acceleration from 0 to 78 mph and left/right steering through field 16762, with 3276861 primitives. The script bypasses the unformatted controller-pak warning in the headless core; the frontend's formatting path is separate. All six 300-field software runs still pass twice with the original RDRAM hashes. These are bounded play segments, not completed games or physical controller/audio tests.
- **Performance:** texture loads flush only when their wrapped source interval can overlap pending color/depth writes, retaining conservative fallbacks. Mario's 4488-field run falls from 127049 to 5527 flushes, with identical RDRAM/audio/VI hashes, primitives, RSP count and PC. Core+VI measurements vary on the shared host; no overall FPS speedup is claimed. Candidate VI SIMD neighbor/noise changes passed 128 scalar-artifact image cases, but isolated medians showed about 3% improvement in one synthetic mode and 7% regression in another amid host variance. Those VI changes were rejected; the scalar VI remains. Candidate and gameplay measurements are retained in generated evidence.

The separate bundled screenshot-content gate still fails all five cases. The development baseline remains a separate result; neither unchanged fixture expectations nor small residual differences are treated as a pass.

### Published verification

[Actions run 38022153286](https://github.com/PowerBeef/Photon64/actions/runs/38022153286) tested `b2d045550a002197714a379e234c10781f9146d8`, containing the implementation published at `8146e709168dd10cb3d4e0769512c43283234fb9` and the browser selector correction. Its required `npm run validate` step passes: 15 legacy JavaScript checks, 38 Node cases, native sanitizers, 11176 native core checks and the same 11176 checks in each WASM O0/O3 build. Both independent Chromium jobs pass their complete expanded checklists with no page errors. The injected presentation exception is an expected console error. The overall workflow remains FAIL because its separate shipped reference-image gate still fails.

The final published native Mario Kart oracle replay reproduces 354 color / 0 depth discrepancies over 96153600 pixels at 3300 fields. Its FAIL verdict remains, including 124 pixels beyond one dither step. The GoldenEye/Perfect Dark counts above describe the intermediate build identified in the saved logs rather than a new claim about the published binary.

The extended final World Driver command-level oracle run completes all 16762 fields and compares 8165 RDP frames / 1131752960 pixels. It reports 54425 color and 784 depth mismatches, including 141 color pixels beyond one dither step in 48 frames: FAIL. Visual driving progress and independent rendering accuracy are separate results. A fresh six-cartridge 300-field/two-run check of the published core also passes with all original RDRAM hashes.

## Renderer accuracy follow-up

The comparison harness previously left CPU/PI/SI/RSP writes stale in the alternate framebuffer image between SYNC_FULL boundaries. It also lacked a guest-write observer for the reference's hidden bits. This produced false depth comparisons, including GoldenEye boot memory at depth address zero. Oracle builds now disable direct CPU store mappings, materialize queued drawing before mirroring masked stores, copy the actual DMA destination bytes and reset CPU-owned hidden bits on both sides. Untouched alternate bits are preserved even if the CPU repeats the master's existing value. Production CPU mappings remain unchanged. The generated adapter leaves the pinned external checkout unchanged. Historical depth counts above are superseded by the results below; removing those false positives is a harness correction, not a depth-rendering improvement.

A separate renderer defect was traced at Mario Kart field 1916, pixel (161,117), primitive 120603. Its perspective coordinate S=-51599 was passed straight to the tile shift, which wrapped it to a positive 16-bit coordinate and sampled yellow. The reference saturates it to -32768 and samples blue. C and WGSL now saturate sampling coordinates before tile shifts, including copy and pipelined samples. The divider's wider outputs remain available to LOD. With exactly the same corrected harness and zero-random-bit policy on both revisions, the original renderer versus the repaired renderer reports:

| Recorded replay | Compared pixels | Color differences before | Color differences after | Depth differences on both |
|---|---:|---:|---:|---:|
| Mario Kart, 3300 fields | 96,153,600 | 354 | 19 | 0 |
| GoldenEye, 4600 fields | 236,948,400 | 1 | 0 | 0 |
| Perfect Dark, 6500 fields | 344,826,880 | 148,116 | 148,112 | 0 |

Additional final independent replays: Mario 4200 fields compares 1915 RDP frames / 147072000 pixels with zero color/depth differences (PASS); World Driver 16762 fields compares 8165 RDP frames / 1131752960 pixels with 54402 color / zero depth differences, including 130 color pixels beyond one dither step in 42 frames (FAIL). GoldenEye's 2151 RDP-frame comparison now passes. Mario Kart, Perfect Dark and World Driver still fail the zero-tolerance accuracy gate. Perfect Dark's old pixel totals differ because the corrected reference memory protocol changes the reference-master execution; only the same-harness table is a controlled renderer comparison.

The required baseline exercises 589824 coordinate checks against the pinned reference's divider and sampling clamp: every signed 16-bit W with signed extrema and deterministic arbitrary S/T inputs. All pass. Another 96 checks exercise real masked stores, the CPU's SW path, TLB/KSEG write mapping, aligned/misaligned PI DMA, RSP DMA, wrapping SI DMA, deferred frame-boundary synchronization, hidden bits and queued-fill/write ordering. Six repaired-coordinate vectors also run in native sanitizer and WASM O0/O3 core tests. No hardware-noise or complete RDP-conformance claim follows from these finite tests.

The 19 residual Mario Kart differences are isolated to two RDP frames at fields 2214/2216. A reference trace shows primitive 226563 copying through inclusive x=320 on source row 18 into the next row's x=0; Photon64 clips at framebuffer width. This requires matching physical-memory aliasing and ordered GPU binning, rather than another texture clamp. It is the next renderer item. Perfect Dark and World Driver still need their own first-command traces. All five shipped screenshot-content checks remain FAIL with unchanged tolerances.

Local `npm run validate` passes: 15 legacy JS checks, 40 Node cases, native ASan/UBSan checks, 11182 core checks in native and each WASM O0/O3 build, Expansion Pak cases, deterministic homebrew execution and comparator tests. The coordinate/write suite totals 589920 passing checks. A fresh six-cartridge 300-field/two-run check passes with the original RDRAM hashes. Dawn/lavapipe exact checks pass Mario Kart native and HD-at-1x at fields 1916/2214/2216/3299, GoldenEye native and HD-at-1x at 2600/3800/4599, and Perfect Dark native at 5900/6499. These are full paired-core replays with six-field GPU windows, comparing VI RGB, last-batch framebuffer color/hidden/depth and three execution counters. They are not continuous-GPU or physical-device tests.

An initial HD-at-1x diagnostic incorrectly used a two-million-halfword buffer instead of full RDRAM storage. Address aliasing caused 158 apparent Mario Kart framebuffer differences; the full-storage replay passes. Exact 1x runners now reject this aliased configuration before execution. An initial Perfect Dark checkpoint at 4300 fails for missing rendered-framebuffer coverage within its GPU window; it is superseded by the actual-rendering checkpoints above, without relaxing the predicate. The local browser lane was not rerun; publication triggers the separate hosted workflow.

The World Driver Dawn/lavapipe native-resolution comparison completes 16762 fields and passes seven exact checkpoints (`300,6000,13999,15541,15941,16401,16761`). This run uses `--window 6`: both cores replay the full input sequence, while GPU rendering runs only around the checked fields. It verifies the documented VI/last-target/counter predicate, not continuous GPU execution, the high-resolution path, full machine state or physical hardware. The default `tools/games.sh wdc` configures ten checkpoints and continuous native/HD1 lanes; those expanded default lanes were not executed in this pass.
