# Current capability and conformance boundaries

This describes development source, not certification or a guarantee that every title works. The current audit implementation and evidence are tracked in [the implementation report](reports/implementation-2026-10-10/REPORT.md).

| Area | Implemented | Evidence / remaining boundary |
|---|---|---|
| CPU addresses | 32-bit effective addresses, mapped/direct segments, user/supervisor protection, original merge-load exception addresses | Native and identical O0/O3 WASM regressions. Full 64-bit addressing is absent. |
| CPU floating point | Existing SoftFloat-backed f32/f64 with VR4300 policy | Exact-rational corpus exists; broader hardware trap/NaN/flush conformance remains open. |
| Cache/timing | Functional loads/stores and constant CPI scheduler | CACHE has no cache model. Cache aliases, LL/SC edge cases, CP0/RSP timing and DMA scheduling require independent conformance work. |
| CIC | IPL3 checksum identification and packed 6105 challenge response; dummy complement for other selected models | Sixteen authored patterns compared to pinned ares ISC implementation. Physical protocol vectors and a demonstrated protected-game checkpoint remain required. Unknown boot code defaults to 6102. |
| Save hardware | EEPROM, SRAM, Flash and four Controller Paks; ID table plus manual per-cartridge override | Existing native peripheral tests and host persistence regressions. Overrides have durable backup/swap recovery. The detection table is not comprehensive. |
| Controllers | Four standard browser gamepads, persistent port slots, per-port pak/rumble; keyboard/touch on port 1 | Mocked four-port/disconnect tests. Real controller models and wireless reconnect/rumble need device tests. |
| Native RDP / VI | Existing integer pixel pipeline and VI filters | Pinned independent raw/VI/reference lanes. Random-bit comparison policy is explicit. Boundary-address model disagreement is retained, not labelled hardware-correct. |
| WebGPU | Native coherent rendering, display-only 2x/4x, retained feedback, scale/loss/readback ownership and cleanup | Mandatory real Dawn/lavapipe vectors, device loss and continuous authored drawing. Software-adapter results do not establish mobile/discrete-GPU speed or driver behavior. |
| Two-cycle rendering | Ordered execution preserving shared feedback | Counts/samples/readback/frame timings instrumented. Dependency partitioning, bounded long-dispatch optimization and physical pacing baselines remain research gates. |
| Persistence | Exact cartridge identities, verified legacy migration, battery/state transactions and one writer per game | Native/core and actual-WASM host regressions plus browser matrix. No storage-eviction immunity; unsupported arbitration uses temporary mode. |
| Standalone app | Embedded WASM/shaders/art/notices and browser fallback | Automated file-origin core/import/storage/audio initialization lane. Actual mobile filesystem choosers and physical WebGPU/audio output require devices. |
| Accessibility | Sibling library play/remove buttons, existing focus/keyboard/responsive controls | Automated geometry/focus checks. VoiceOver, 200% text, contrast and assistive-device sign-off are still required. |

## Independent conformance work sequence

1. Resolve the RDP installed-RAM/24-bit addressing disagreement against permitted hardware captures, then implement one tested contract in C, WGSL, CPU/GPU tracking and texture/VI paths.
2. Obtain separately licensed CIC vectors and record protected-game inputs/checkpoint, region/revision, initial saves and expected observable response. Compare protocol bytes before expanding compatibility claims.
3. Add authored independent renderer streams for next-pixel alpha/texel replacement/LOD, span and batch boundaries, retained blender/depth shifts, TMEM format/wrap transitions and VI interlace/filter edges. Require zero native and HD-at-1x differences before scheduling changes.
4. Add hardware-derived FPU exception/trap/NaN/flush expectations with trace provenance and native/O0/O3 equivalence. Do not infer trap behavior from exact arithmetic alone.
5. Design the 64-bit address/segment architecture, separate translation from privilege, and model cache/timing/LL-SC behavior in independently reviewable milestones. Preserve the bounded 32-bit regressions.
6. Capture desktop integrated/discrete GPU and physical mobile frame/input/audio/memory baselines. Optimize ordered work only after identifying dependency-safe splits; carry feedback across every chunk and rerun exact lanes. Worker/OffscreenCanvas changes remain conditional on measured main-thread stalls and standalone/Safari support.
