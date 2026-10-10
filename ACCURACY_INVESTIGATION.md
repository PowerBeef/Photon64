# Remaining accuracy investigation

Investigated 10 October 2026 against Photon64 `0b499b1c318ad7ca317d4f13046427f29df4c2a6` (tree `7f0d4880230b3611623494cf4cd01b4f88465f24`). Production renderer/core semantics were not changed during this investigation. The external oracle remains pinned to Angrylion RDP Plus `9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc`; temporary instrumentation and explicitly altered diagnostic references are generated outside tracked source.

The remaining failures have different causes: framebuffer row aliasing, differences in pipeline feedback, and a screenshot gate comparing raw framebuffer references with VI scanout. They should not be treated as one texture-coordinate or random-dither problem. This report establishes specific reference discrepancies, not hardware certification or attribution of every residual pixel.

## Current measured baseline

These completed replays use the standard guest-write-aware, noise-zero diagnostic oracle from the preceding renderer pass. They are not stock-reference or hardware captures. Fresh shorter replays below reproduce the earliest remaining differences. Counts are cumulative across compared RDP frames, not unique screen locations; VI fields and RDP frames are different counters.

| Replay | VI fields | RDP frames | Pixels compared | Color differences | Beyond one dither step | Depth differences | Gate |
|---|---:|---:|---:|---:|---:|---:|---|
| Mario 64 | 4200 | 1915 | 147072000 | 0 | 0 | 0 | PASS |
| GoldenEye | 4600 | 2151 | 236948400 | 0 | 0 | 0 | PASS |
| Mario Kart | 3300 | 1252 | 96153600 | 19 | 19 | 0 | FAIL |
| Perfect Dark | 6500 | 2249 | 344826880 | 148112 | 9728 | 0 | FAIL |
| World Driver | 16762 | 8165 | 1131752960 | 54402 | 130 | 0 | FAIL |

The previous stale-memory depth counts are superseded by the guest-write-aware observer. Exact native-resolution GPU checkpoints using the complete HD backing storage pass in the bounded scenarios already documented in `VALIDATION_REPORT.md`. A reduced backing store aliases RDRAM addresses and is now rejected for exact 1x HD checks. These results do not validate all hidden bits, full machine state, all gameplay, or physical GPU performance.

## A1: Copy writes crossing a framebuffer row — confirmed reference discrepancy

Mario Kart's residual differences occur at VI fields 2214/2216. At RDP frame 709, pixel `(0,19)`, the reference copy operation with sequence 226563 draws logical `(320,18)` into a framebuffer whose stride is 320 pixels. Linear addressing maps that write onto physical `(0,19)`. The subsequent HUD copy on row 19 rejects its own pixel by alpha, leaving the row-crossing value visible. Photon64 retains `0x963f`; the reference retains `0x3183`.

A temporary isolated C-core probe removes only the width clip in `sw_render_batch`. Against the unchanged standard reference it completes the full 3300-field replay with **zero color/depth differences**, eliminating all 19 residuals. RDP frames (1252), compared pixels (96153600) and process calls (2878547) match the baseline. This is native diagnostic evidence; the corresponding GPU architecture change has not been implemented or tested. The production source and standard executables remain unchanged.

`src/rdp_pixel.h:sw_render_batch` clips logical X against `b_info.fb_width`; GPU tile binning and dispatch also assume framebuffer rows are independent. SetColorImage width is a memory stride, not an additional scissor boundary. The pinned reference's copy renderer computes a linear address from stride and logical X, including the inclusive endpoint in this example. Nintendo's rasterizer documentation also specifies four-pixel-boundary scissoring restrictions for copy/fill modes [S1]. That supports investigating burst endpoints; it does not by itself specify this exact alias case.

**Next implementation:** preserve logical source coordinates and traversal order while mapping writes to physical RDRAM. Handle row aliases in both C and GPU scheduling, including fill/copy endpoints, alpha rejection and hidden bits. Simply allowing one extra GPU invocation can race with the next row's invocation. Add synthetic narrow-stride/overshoot/wrapped-address cases before rerunning all 3300 fields. The fix must remove the 19 differences without creating color/hidden/depth or C/GPU regressions.

## A2: Perfect Dark first-cycle memory color — traced and full-run probe confirmed

A fresh 2912-field standard-reference replay first differs at RDP frame 1231 / VI field 2911: 34 color differences, none beyond one dither step, zero depth differences across 273230080 compared pixels. Target `(124,105)` ends as `0x108d` in Photon64 and `0x10cf` in the reference. Earlier traced textured operations agree in coordinates, sampled values and relevant combined output. The final two-cycle rectangle, sequence 453567, exposes a different first-cycle memory operand.

The reference reads the framebuffer into `pre_memory_color`, blends cycle 0 using the retained `memory_color`, then installs `pre_memory_color` for cycle 1. Photon64 calls `blender` twice with the current pixel's `memory` in `src/rdp_pixel.h:shade_and_blend`; `src/web/rdp.wgsl` uses the same operands. At the target, combined RGB is `(0,0,68)`, effective alpha is 147, retained memory RGB is `(48,48,80)`, and current memory RGB is `(24,32,48)`. Captured blend modes use the effective weights 18 and 14 with a right shift of five.

| First-cycle memory | First-cycle RGB | Final RGB | Stored RGB555 channels |
|---|---|---|---|
| Current, as Photon64 | `(10,14,59)` | `(16,21,54)` | `(16,16,48)` |
| Retained, then current, as reference | `(21,21,73)` | `(22,25,62)` | `(16,24,56)` |

This independently recomputed arithmetic reproduces both stored target values. The full controlled reference probe below removes the observed Perfect Dark residual as well. Debug lines printing the raw combiner result precede alpha dithering in Photon64; their apparent alpha difference is not an independent alpha bug.

**Next implementation:** retain the project's existing strict pinned-reference target and make the pipeline-feedback approximation explicit. paraLLEl-RDP's author documents the same first-cycle memory-color difference and deliberately uses a current-pixel interpretation [S2]. Nintendo documents two-cycle fog/blending at a higher level, without settling this retained-state edge case [S3]. Introduce ordered feedback state with standalone span-direction and reset/scanline/primitive boundary vectors. Identify draws requiring feedback and evaluate an ordered GPU pass or coherent software fallback; a pixel-local WGSL substitution cannot reproduce retained state generally. Obtain a hardware-derived expectation before claiming hardware accuracy. Keep the strict independent-reference lane failing until its actual discrepancies are resolved.

## A3: World Driver combined-color feedback — traced and full-run probe confirmed

A fresh 6862-field standard-reference replay first differs at RDP frame 3322 / VI field 6861: 114 color differences, none beyond one dither step, zero depth differences across 500582400 compared pixels. At target `(311,0)`, a textured primitive, sequence 445060, agrees in coordinates, sampled RGBA and combined RGB. A later one-cycle rectangle, sequence 445150, uses RGB selector `0x05000503`: its multiplier reads COMBINED. Its alpha add selector also reads combined alpha.

Photon64 initializes `in.combined` to zero for each pixel in C and `inp.combined = vec4<i32>(0)` in WGSL. The pinned reference retains `combined_color` between pixels and updates it in `combiner_1cycle`. At the rectangle target its resulting pixel color is `(28,28,28,255)` while Photon64's raw combined output is zero. After blending/dithering the stored values differ: Photon64 `0x0993`, reference `0x0995`. This is a feedback discrepancy, not evidence that the perspective divide or texture sample still fails at this target.

The reference's retained COMBINED behavior and paraLLEl-RDP's intentional zero initialization are explicitly discussed by the latter's author [S2]. Hardware pipeline timing may affect this behavior. Match the existing strict reference target with synthetic recurrence, span-direction and boundary tests while keeping hardware validation separate. The full controlled probe below removes all 54402 World Driver color differences, including the 130 larger differences. This strongly attributes the observed run to the combined-feedback policy; it does not establish hardware timing or eliminate unexercised issues.

### Controlled first-failure probes

Two temporary reference experiments changed only the behavior identified above; the production Photon64 core and standard oracle adapter stayed unchanged. In each prefix the RDP command-call count, RDP-frame count and compared-pixel count remain identical. Both standard prefixes still FAIL; the altered-reference outputs are causal diagnostics, not accepted accuracy verdicts.

| Replay prefix | Standard color differences | Altered reference behavior | Diagnostic color/depth differences | RDP process calls |
|---|---:|---|---|---:|
| Perfect Dark, 2912 fields | 34 | Use current memory color before two-cycle blender cycle 0 | `0 / 0` | 1081988 |
| World Driver, 6862 fields | 114 | Clear combined RGBA before each one-cycle combiner evaluation | `0 / 0` | 899989 |

These probes remove the entire first-failure sets. Full-run probes then produce:

| Recorded replay | Standard color/depth differences | Diagnostic change | Diagnostic color/depth differences | RDP process calls, same in both runs |
|---|---|---|---|---:|
| Mario Kart, 3300 fields | `19 / 0` | Remove width clipping in an isolated native C core; standard reference unchanged | `0 / 0` | 2878547 |
| Perfect Dark, 6500 fields | `148112 / 0` | Current-memory reference probe | `0 / 0` | 3928632 |
| World Driver, 16762 fields | `54402 / 0` | Zero-COMBINED reference probe | `0 / 0` | 3404208 |

Each pair also has identical RDP-frame and compared-pixel totals from the baseline table. Perfect Dark's 9728 larger differences and World Driver's 130 larger differences disappear in these diagnostic runs. This strongly attributes the observed full-run divergences to the identified mechanisms. It does not establish hardware behavior or full machine-state equivalence: changing the reference-master framebuffer can influence guest execution even when these counts agree. The altered-reference results are not accepted PASS verdicts against the original target. The native row probe also does not validate the unimplemented GPU counterpart.

## A4: Shipped screenshot failures compare different output stages — confirmed

All five test ROMs have zero drawn RDP primitives in the native captures. Their upstream assembly writes text directly into a 640x480 RGBA32 framebuffer, enables interlace/resampling, and alternates VI Y-scale offsets. Fresh 120-field native captures with `-raw` match the corresponding repository reference PNG in **every RGB pixel**, without resizing, alignment changes, count normalization or relaxed tolerances.

| Fixture | Raw unequal RGB pixels | Raw/reference white, green, red counts |
|---|---:|---|
| RSPCP2VRCP | 0 | `48448, 4264, 1904` |
| RSPCP2VRCPH | 0 | `48484, 4264, 1980` |
| RSPCP2VRCPL | 0 | `48488, 4264, 1992` |
| RSPCP2VSAR | 0 | `37896, 4216, 2805` |
| RSPTransposeMatrixVMOV | 0 | `24588, 2592, 2420` |

The current `tools/cmp_ref.py` invokes the VI capture path and compares its color counts to these images. `src/vi.c` filters/scales, clamps eight pixels at the left and seven at the right when applicable, and weaves fields; raw framebuffer pixels undergo none of those operations. The screenshot gate still fails, with its original fixed tolerances intact. This failure does **not** establish a broken reciprocal, VSAR or transpose result in these fixtures. Exact raw image agreement establishes only the exercised outputs, not complete RSP conformance or accurate VI scanout.

**Next tooling:** make an explicitly named exact raw-framebuffer fixture lane and retain a separate VI lane. Obtain VI reference outputs with stated register values, field phase, origin, filtering and deinterlace policy; compare those with a pinned independent VI implementation and ultimately hardware. Do not label an unvalidated VI lane PASS merely because raw images match. The present count gate can remain a visible legacy check until its replacement is reviewed.

### Fixture provenance established, permissions still separate

All ten local `.N64`/`.png` git blob hashes match PeterLemon/N64 at revision `7085543e4a19d8c539fc9e0a4d2869e788b4ed4b`, fetched during this investigation [S4]. Paths are `RSPTest/CP2/{VRCP,VRCPH,VRCPL,VSAR}` and `RSPTest/CP2/LOADSTORE/TransposeMatrixVMOV`. This establishes an exact matching upstream revision. It does not establish the original capture configuration or grant distribution permission.

## Other concrete accuracy and measurement gaps

| Item | Code evidence and qualification | Next check |
|---|---|---|
| Chroma keying is incomplete | SetKeyR/SetKeyGB store width/center/scale in `src/rdp.c`; `key_width` is never consumed and SetOtherModes does not propagate key-enable bit 8. There is no key-alpha path in C/WGSL. Nintendo specifies programmable key alpha [S5]. No commercial residual is attributed to this gap here. | Synthetic key-enable on/off and width/scale/center boundary vectors; implement both renderers against an independent expectation. |
| Independent oracle misses hidden-state differences | `tools/oracle.c:compare` checks framebuffer color and 16-bit depth bytes. Existing adapter hidden-bit exports are used by observer tests, not per-frame commercial hidden-state comparisons. | Compare color coverage and depth hidden bits with an explicit guest-ownership policy; investigate the first primitive before propagation. |
| Oracle format/region coverage is bounded | The comparator treats every non-RGBA32 target as 16-bit, derives height from current scissor, caps it at 480, and compares the final target at SYNC_FULL. It does not fully validate I4/I8 targets, previous offscreen targets, or every touched RDRAM byte. Native I4/I8 batches deliberately fall back to software in GPU mode. | Format-aware comparison and touched-range tracking, including wrapped and aliased targets. Add negative fixtures proving missing regions cannot pass. |
| Pipeline feedback extends beyond these two traces | First-cycle COMBINED, two-cycle memory color, next-pixel alpha comparison and texel feedback have special reference behavior [S2]. The C alpha test uses current-pixel cycle-0 alpha; the reference pipelines next-pixel alpha in its two-cycle span loop. | Isolated synthetic vectors for each dependence. The latter cases are source differences, not yet demonstrated causes of a remaining game pixel. |
| Noise-zero tests do not validate production entropy | The diagnostic adapter zeros random bits, retaining combiner bias `0x20`. Production noise is generated per pixel; real hardware sequence/statistics are not established. | Validate range, bias, correlation and visible effects separately; preserve a deterministic arithmetic lane. |
| CPU/cache/timing limits remain | CPU PC/address paths are 32-bit, CACHE is a no-op, CPI is configurable constant, and VI scheduling uses an approximate field period. SoftFloat/direct-rounding work is implemented; broader hardware-derived FPU edge conformance is still open. | Separate CPU/cache/FPU/timing hardware corpus; do not count renderer agreement as machine conformance. |
| Coverage of devices and games remains finite | Selected GPU windows and two hosted Chromium viewport jobs pass; physical GPU/mobile/controller/audio quality and longer gameplay are unverified. Smash has deterministic/gameplay evidence, not an independent-reference result in the five-replay table. | Extend recorded scenarios and named physical-device testing, including continuous GPU execution. |

## Implementation sequence

1. Fix physical framebuffer row aliasing in C and GPU with ordered writes and synthetic coverage; replay Mario Kart 3300 fields and the unaffected passing Mario/GoldenEye scenarios.
2. Separate exact raw fixture validation from independently referenced VI validation, including interlace field/register snapshots. This resolves the diagnosis of the current CI failure without weakening thresholds.
3. Add command/pixel diagnostics and synthetic feedback vectors, then match retained COMBINED and first-cycle memory color against the existing strict pinned-reference target, with an ordered pass or coherent fallback where needed. Keep hardware validation separate. Both full-run diagnostic discrepancies disappear under the stated reference probes; implementation still needs to satisfy the original reference target.
4. Expand oracle comparison to hidden bits, low-bit-depth targets and all touched framebuffer ranges; localize the remaining larger Perfect Dark/World Driver differences with that observer.
5. Implement chroma keying and broaden TMEM/LOD/alpha/VI edge vectors. Continue CPU/FPU/timing and real-device work as distinct accuracy tracks.

Candidate upstream test sources at the verified PeterLemon revision include `RDP/CombinerOverflow`, `RDP/AlphaCompare`, `RDP/AlphaCoverage`, and `RCP/VI/CoverageTest`. The first, second and fourth assembly sources were inspected: they exercise combiner overflow/two-cycle arithmetic, alpha compare modes, and VI coverage respectively. They are useful additions to the five RSP fixtures, but their expected capture settings and hardware provenance must be established before using their PNGs as VI or hardware oracles. No additional ROM binary was downloaded or executed in this investigation.

## Reproduction and evidence limits

Use the existing native build and the pinned generated noise-zero adapter documented in `DEVELOPMENT.md`. Do not replace the pinned checkout with instrumentation. Replay inputs are `tools/inputs/{mk64,pd,wdc}.txt`.

```sh
./out/oracle_nn "roms/Perfect Dark (USA) (Rev 1).z64" 2912 -i "$(cat tools/inputs/pd.txt)" -d out/pd-first- -vv
./out/oracle_nn "roms/World Driver Championship (USA).z64" 6862 -i "$(cat tools/inputs/wdc.txt)" -d out/wdc-first- -vv
./out/native testroms/RSPCP2VRCP.N64 120 -raw -o out/vrcp-raw- -e 120
```

Compare decoded RGB pixels of `out/vrcp-raw-00120.png` against `testroms/RSPCP2VRCP.png`; repeat for the other four fixtures. Pixel traces and command/TMEM dumps stay local because they can contain cartridge-derived payloads. Derived operand summaries above are sufficient to assess the specific causes. Temporary altered references are causal probes, never the production oracle or a substitute PASS.

For the causal experiments, copy the current C source or generated reference and its included modules into a separate ignored directory. The row probe deletes only the `end_x >= b_info.fb_width` clamp in the copied `sw_render_batch`. The Perfect Dark probe assigns `wstate->memory_color = wstate->pre_memory_color` immediately before `blender_2cycle_cycle0` in each of the four reference two-cycle span variants. The World Driver probe clears all four `wstate->combined_color` components immediately before `combiner_1cycle` in each of its three one-cycle span variants. Keep `NOISE_ZERO`, guest-write hooks, inputs and compiler options identical to the standard lane; compile separate executables. Do not overwrite `out/oracle_nn`, modify the pinned checkout or treat these probes as accuracy gates.

The tested WASM SHA256 is `010dbc2a5ee49bacd73009e0fceb246013ae3a2ec42edbd78833572668f20304`; RDP WGSL SHA256 is `7149bcbf2c44c981f6dbfc475df29585184738b0bfc45002f698fe41ee2070d4`. Hosted Actions run [38025675113](https://github.com/PowerBeef/Photon64/actions/runs/38025675113) completed on the investigated commit: `npm run validate` PASS, both browser jobs PASS, shipped reference screenshot step FAIL, overall workflow FAIL. Documentation-only publication does not constitute a new renderer validation run.

## Primary sources consulted

- **S1:** Nintendo, [Programming Manual 12.3: Rasterizer](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro12/12-03.html), historical Nintendo documentation mirrored by ultra64.ca; copy/fill scissor restrictions. The exact linear-address behavior is inspected in the pinned reference `render_spans_copy`, not inferred solely from the manual.
- **S2:** Themaister, [paraLLEl-RDP README at `1cecd042`](https://github.com/Themaister/parallel-rdp/blob/1cecd042b2619bc505c12bfdc713808386f2b54d/README.md), especially intentional reference differences. Its deliberate choices inform the uncertainty of feedback behavior; they are not new hardware measurements. `parallel-rdp/video_interface.cpp` and `shaders/vi_scale.frag` at that revision were also inspected.
- **S3:** Nintendo, [Programming Manual 12.7: Blender](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro12/12-07.html), two-cycle fog, framebuffer blending and alpha comparison.
- **S4:** Peter Lemon, [N64 at `7085543e`](https://github.com/PeterLemon/N64/tree/7085543e4a19d8c539fc9e0a4d2869e788b4ed4b), matching fixture blobs and original assembly, including direct text writes and alternating Y-scale offsets.
- **S5:** Nintendo, [Programming Manual 12.6: Color Combiner](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro12/12-06.html), key-alpha behavior and programmable key parameters.
- **S6:** [Angrylion RDP Plus at `9c8b9ed3`](https://github.com/ata4/angrylion-rdp-plus/tree/9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc), inspected locally: `src/core/n64video/rdp/{rasterizer,combiner,blender,dither,zbuffer,rdram}.c` and `vi.c`. This is the independent implementation reference; no hardware was connected for this investigation.
