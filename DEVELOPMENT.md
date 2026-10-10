# Reproducible development

The baseline is Node 24+, Python 3.12, a native C compiler (GCC 13 tested), and WASI SDK 34.0 for freestanding WASM. The exact SDK digest and Angrylion commit are in `tools/versions.env`. npm's lockfile pins Playwright 1.63.0 and Dawn's Node bindings 0.6.2. Python analysis requirements are pinned.

On Linux x86_64 or macOS:

```sh
tools/setup.sh
. out/env.sh
npm run validate
```

`setup.sh` installs dependencies under `.tools/`, `node_modules/`, and `.venv/`, and creates a pinned reference checkout at `../ref/angrylion-rdp-plus`. Set `REF` to use another location. An existing different revision is rejected instead of changing it. `tools/setup.sh --browser` also installs Playwright Chromium. On macOS, Homebrew LLVM/lld are used for WASM; `CC`/`WASI_SDK_PATH` and `NATIVE_CC` select compilers explicitly.

`npm run validate` builds the app, runs source-level JS regressions, native unit checks and sanitizers, the identical semantic corpus in O0/O3 WASM, synthetic Expansion Pak checks, and deterministic execution of the five shipped homebrew ROMs. It does not require a commercial ROM or GPU. `npm test` alone requires an existing `out/n64.wasm` for the actual-core lifecycle test.

Accuracy and GPU lanes have independent prerequisites and **must not be replaced by baseline smoke results**:

```sh
.venv/bin/python tools/cmp_ref.py testroms 120
tools/games.sh --check-roms
GPU_RUNNER=dawn tools/games.sh
GPU_RUNNER=browser tools/games.sh
```

Dawn uses the locally installed `webgpu` package. Linux defaults to Mesa lavapipe at `/usr/share/vulkan/icd.d/lvp_icd.json`; install `mesa-vulkan-drivers` through your OS package manager, or set `VK_ICD_FILENAMES` to a valid ICD file. On macOS use `GPU_BACKEND=metal`. A software Vulkan adapter exercises shaders/coherence, not physical GPU performance. Browsers require a working Chromium installation and WebGPU adapter. Missing adapters are explicit failures. Standalone `--noref`/`--noexact` runs are diagnostic SKIP (exit 2), never exact PASS. The suite explicitly accepts diagnostic completion only for its separate 4x seam analysis.

The GPU predicate compares VI RGB, last-batch framebuffer color/hidden/depth data and three execution counters (PC, RSP instructions, primitive count). Framebuffer inspection first uploads CPU-owned changes to the GPU cache; unused depth addresses otherwise report stale data unrelated to rendering. It does not compare the full CPU/FPU/RSP state. Coverage is written into the result. Missing framebuffer comparisons after rendering fail. Zero-render checkpoints can establish execution agreement but not rendering accuracy. The independent Angrylion lane fails on any counted color/depth/hidden mismatch or zero comparisons. It also checks all potentially touched color/depth ranges before SYNC_FULL image resynchronization.

The native oracle disables direct CPU store mappings only in its `RDP_ORACLE` build and observes masked CPU stores plus PI, SI and RSP DMA writes. It materializes queued software drawing before updating the alternate image, preserves untouched bits, and marks CPU-owned hidden bits on both sides through the generated reference adapter. Production builds retain their direct mappings. The required native build runs `out/oracle_test`: 958565 independent coordinate, guest-write, keying, renderer-stream and observer-mutation checks. Its generated adapter exposes only test/reference functions; the external checkout stays pinned and unchanged. Both sides zero random bits; this diagnostic policy is not a hardware noise test.

`RENDERER_ACCURACY_IMPLEMENTATION.md` describes ordered row aliases, retained pipeline feedback, key alpha and the expanded observer. `ACCURACY_INVESTIGATION.md` preserves the earlier causal traces. Required fixture validation is now split by output stage:

```sh
.venv/bin/python tools/cmp_raw_ref.py testroms 120
for rom in testroms/*.N64; do ./out/vitest "$rom" 120; done
```

The raw lane compares exact RGB pixels/dimensions to each exact PNG filename. The VI lane uses guest registers and the field sequence against the independent pinned reference; no PNG scaling or count tolerance is used. Both run within `npm run validate`. The old `cmp_ref.py` retains its five mixed-stage failures and unchanged thresholds as a CI diagnostic.

The native build also emits authored batches for actual shader testing:

```sh
node tools/dawntest.mjs testroms/RSPCP2VRCP.N64 1 "0" "" --fn tools/rdp_gpu_vectors.js
node tools/dawntest.mjs testroms/RSPCP2VRCP.N64 1 "0" "" --fn tools/rdp_gpu_vectors.js --hd 0
```

These vectors compare framebuffer/hidden data and twelve retained registers across twelve batches, separately from VI/gameplay verdicts. Dependent/two-cycle batches use ordered dispatch; independent batches retain parallel rasterization. `stats.orderedBatches` reports scheduling counts. Low-format software fallback synchronizes prior GPU work and uploads its retained/hidden state before subsequent GPU batches. Ordered rendering can be slower; physical-device performance remains unmeasured.

To reproduce the Mario Kart accuracy replay and native/HD-at-1x parity around the repaired pixel:

```sh
./out/oracle_nn "roms/Mario Kart 64 (USA).z64" 3300 -i "$(cat tools/inputs/mk64.txt)"
node tools/dawntest.mjs "roms/Mario Kart 64 (USA).z64" 3300 \
  "1916,2214,2216,3299" "$(cat tools/inputs/mk64.txt)" --window 6
node tools/dawntest.mjs "roms/Mario Kart 64 (USA).z64" 3300 \
  "1916,2214,2216,3299" "$(cat tools/inputs/mk64.txt)" --window 6 --hd 0
```

Set the adapter variables described above first. `--window 6` runs complete paired-core replays, with GPU rendering active only in the six fields leading into each checkpoint; it is not a continuous-GPU replay. Keep the full RDRAM-sized buffer for exact 1x checks. The optional `--hdwords` workaround for restricted adapters introduces address aliasing and must not be used for an exact framebuffer verdict.

Local commercial fixtures belong in ignored `roms/` and are never fetched by setup or CI:

```sh
npm run test:roms
node tools/romcheck.mjs roms --frames 1200 --output out/commercial-1200.json
```

The manifest records source revision/dirty state, WASM SHA256, runtime, ROM identities, frames, statistics and two-run memory/audio hashes. PASS here means deterministic software execution only. `tools/run_wasm.mjs` remains a diagnostic timing utility. Recorded input scripts exist for all six local games; see `tools/games.sh` for their lengths and checkpoints and `VALIDATION_REPORT.md` for executed results.

CI automatically executes the baseline, exact raw fixtures and independent VI fixture gate on push/PR. The legacy mixed-stage screenshot diagnostic remains visible with its unchanged failure counts and an uploaded log. Independent browser jobs exercise Chromium, branded Chrome/Edge and macOS WebKit using the same built artifact and a shipped homebrew fixture. A separate macOS job uses native Safari WebDriver. Any mismatch in a required lane makes the workflow fail; only the superseded mixed-stage diagnostic uses `continue-on-error`. Repository branch-protection settings are managed separately; adding a workflow does not enforce required checks in settings.

See `IMPLEMENTATION_PLAN.md` for remaining FPU, renderer/reference, browser lifecycle and device-matrix work. See `THIRD_PARTY.md` for fixture and reference provenance.

## This execution environment

The installed SDK and extracted lavapipe driver are under `.tools/`; the checked package identities are in `out/host-provenance.json`. Use `VK_ICD_FILENAMES="$PWD/.tools/lvp_icd.json"` for the local Dawn lane. `tools/setup.sh` itself was successfully exercised with npm and the pinned Python requirements.

Chrome for Testing was installed manually under `.tools/chrome/chrome-linux64` after resuming a truncated archive. Set `CHROME_EXECUTABLE` to its `chrome` executable, or use the standard Playwright installation on your own machine:

```sh
CHROME_EXECUTABLE="$PWD/.tools/chrome/chrome-linux64/chrome" node tools/browsercheck.mjs "roms/Super Mario 64 (USA).z64"
```

`browsercheck.mjs` checks actual IndexedDB persistence across reload, cached ROM loading, duplicate state writes, state restoration, keyboard pause/resume and menu resume. It records a 120-field paced sample, console/page errors, screenshots and a Playwright trace under `out/browser-evidence/`, including evidence on failure. Chromium also records a CDP CPU profile; WebKit reports that profile as SKIP. `BROWSER_ENGINE=webkit` selects WebKit; `BROWSER_CHANNEL=chrome` or `msedge` selects a branded Chromium browser. `BROWSER_MOBILE=1` enables a mobile viewport/touch context; this is emulation, not an iPhone or Android device. `BROWSER_EVIDENCE` changes the artifact directory. Traces can contain loaded cartridge data: keep commercial-game evidence local.

`tools/responsivecheck.mjs` visits 15 viewport/DPR configurations, measures actual control reachability and canvas proportions, and exercises menus, rotation, safe-area spacing and keyboard/touch navigation. `tools/safaricheck.mjs` runs shared geometry assertions and real clicks/keys through Apple's native macOS Safari driver. See [UI_COMPATIBILITY.md](UI_COMPATIBILITY.md) for evidence, reproduction commands and remaining hardware checks.

Local execution remains blocked: `AGENTS.md` requires outside-sandbox execution and this environment rejected escalation. Installing Chromium or Playwright MCP does not remove that limitation. The managed cloud browser is in a separate network namespace (workspace localhost is unreachable), and its policy rejects `file:` URLs. Do not tunnel or change host policy to work around those boundaries. The independent browser job passed its original shipped-fixture checks in [Actions run 38016280375](https://github.com/PowerBeef/Photon64/actions/runs/38016280375); expanded checks have separate run evidence. That hosted result does not establish local-browser, physical-GPU or commercial-game coverage.

## Gameplay benchmarks and debugging

`node tools/coreplay.mjs ROM [output-directory]` opens a persistent software-core session controlled by JSON lines on stdin. This gives an agent direct field stepping, controller input, machine inspection and PNG snapshots without a browser. `step` holds buttons/axes for a field count, `replay` uses the existing absolute-field input script, and `snapshot` captures the last VI image without advancing the machine. For example:

```json
{"op":"replay","fields":4200,"inputs":"tools/inputs/sm64.txt"}
{"op":"snapshot"}
{"op":"step","fields":120,"input":"A+X=80"}
{"op":"snapshot"}
{"op":"inspect"}
{"op":"quit"}
```

The live session excludes the frontend, browser audio and GPU. Use it to investigate gameplay visually and choose the next inputs; use the independent browser runner for frontend behavior. Core sessions and image captures remain local.

Run sequential fresh-process replays, measuring a selected field range after boot/menu navigation. This includes software CPU/RSP/RDP execution **and VI rendering**, unlike the older core-only timing utility. Each repeat must complete, produce valid VI fields and agree in final counters and RDRAM/audio/image hashes. Samples and mean/p50/p95/p99/max times, peak process RSS, host/runtime identities and content hashes are written to `summary.json` and per-run files. A worker timeout fails rather than hanging indefinitely. These are shared-host measurements per emulated field, not rendered-game FPS or a prediction of phone performance.

```sh
npm run bench -- "roms/Super Mario 64 (USA).z64" \
  --frames 4200 --warmup 3800 --inputs tools/inputs/sm64.txt \
  --repeats 3 --output out/playtest-sm64
```

Use `--frames 3300 --warmup 2800 --inputs tools/inputs/mk64.txt` for Mario Kart, `4600/4000/ge.txt` for GoldenEye, and `6500/5700/pd.txt` for Perfect Dark. Smash has `tools/inputs/smash.txt` through an 8000-field match segment; World Driver has `tools/inputs/wdc.txt` with menu and race navigation. These scripts replay bounded scenarios; they do not establish whole-game compatibility.

The original Mario script finishes at the castle tutorial dialog. `tools/inputs/sm64-play.txt` extends it with the observed dialog dismissal, run and jump. Use `--frames 4488 --warmup 4298 --inputs tools/inputs/sm64-play.txt` to measure that short active movement segment rather than the tutorial scene.

For named V8 profiles, preserve WASM function names in a separate optimized build, then use `--profile`. The profiled replay is separate from benchmark repeats and must produce the same final hashes. Its profile covers the entire process including boot and warmup; timing statistics exclude warmup. Open `replay.cpuprofile` in a CPU-profile viewer.

```sh
. out/env.sh
WASM_SYMBOLS=1 WASM_OUTPUT=out/n64-symbols.wasm ./build_wasm.sh
npm run bench -- "roms/Super Mario 64 (USA).z64" \
  --frames 4200 --warmup 3800 --inputs tools/inputs/sm64.txt \
  --wasm out/n64-symbols.wasm --profile --output out/playtest-sm64
```

For visual/audio inspection, the native harness already accepts the same input scripts. The image sequence can be converted to a video with FFmpeg. Native WAV output uses the last reported sample rate for the stream, so games that change DAC rates require segmented audio analysis before drawing quality conclusions.

```sh
mkdir -p out/capture-sm64
out/native "roms/Super Mario 64 (USA).z64" 4200 \
  -i "$(cat tools/inputs/sm64.txt)" -o out/capture-sm64/frame- -e 60 \
  -wav out/capture-sm64/audio.wav
VK_ICD_FILENAMES="$PWD/.tools/lvp_icd.json" node tools/dawntest.mjs \
  "roms/Mario Kart 64 (USA).z64" 3300 "2800,3299" \
  "$(cat tools/inputs/mk64.txt)" --trace 10 --images out/mk64-gpu
```

Use the sanitizer baseline for CPU/memory failures, `n64_debug_state()` and exact replay counters for machine-state diagnosis, Dawn's `--trace` for GPU copy-back ordering, and the independent oracle for renderer accuracy. Do not infer audio-device latency, physical controller behavior, WebGPU device performance or complete game compatibility from these lanes. See `VALIDATION_REPORT.md` for executed evidence and unresolved failures.
