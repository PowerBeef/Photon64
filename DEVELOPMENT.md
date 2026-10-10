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

The GPU predicate compares VI RGB, last-batch framebuffer color/hidden/depth data and three execution counters (PC, RSP instructions, primitive count). Framebuffer inspection first uploads CPU-owned changes to the GPU cache; unused depth addresses otherwise report stale data unrelated to rendering. It does not compare the full CPU/FPU/RSP state. Coverage is written into the result. Missing framebuffer comparisons after rendering fail. Zero-render checkpoints can establish execution agreement but not rendering accuracy. The independent Angrylion lane fails on any counted color/depth mismatch or zero comparisons.

Local commercial fixtures belong in ignored `roms/` and are never fetched by setup or CI:

```sh
npm run test:roms
node tools/romcheck.mjs roms --frames 1200 --output out/commercial-1200.json
```

The manifest records source revision/dirty state, WASM SHA256, runtime, ROM identities, frames, statistics and two-run memory/audio hashes. PASS here means deterministic software execution only. `tools/run_wasm.mjs` remains a diagnostic timing utility. Recorded input scripts exist for Mario, GoldenEye, Perfect Dark and Mario Kart; Smash and World Driver currently have smoke coverage only.

CI automatically executes the baseline and shipped screenshot-reference gate on push/PR. An independent browser job installs Chromium and runs persistence/lifecycle checks with a shipped homebrew fixture, without commercial ROMs. Any existing reference discrepancy intentionally makes the workflow fail. Repository branch-protection settings are managed separately; adding a workflow does not enforce required checks in settings.

See `IMPLEMENTATION_PLAN.md` for remaining FPU, renderer/reference, browser lifecycle and device-matrix work. See `THIRD_PARTY.md` for fixture and reference provenance.

## This execution environment

The installed SDK and extracted lavapipe driver are under `.tools/`; the checked package identities are in `out/host-provenance.json`. Use `VK_ICD_FILENAMES="$PWD/.tools/lvp_icd.json"` for the local Dawn lane. `tools/setup.sh` itself was successfully exercised with npm and the pinned Python requirements.

Chrome for Testing was installed manually under `.tools/chrome/chrome-linux64` after resuming a truncated archive. Set `CHROME_EXECUTABLE` to its `chrome` executable, or use the standard Playwright installation on your own machine:

```sh
CHROME_EXECUTABLE="$PWD/.tools/chrome/chrome-linux64/chrome" node tools/browsercheck.mjs "roms/Super Mario 64 (USA).z64"
```

`browsercheck.mjs` checks actual IndexedDB persistence across reload, cached ROM loading, duplicate state writes and state restoration. Local execution remains blocked: `AGENTS.md` requires outside-sandbox execution and this environment rejected escalation. Installing Chromium does not remove that policy limitation. After publication, the independent browser job passed with the shipped RSP fixture in [Actions run 38016280375](https://github.com/PowerBeef/Photon64/actions/runs/38016280375). That hosted result does not establish local-browser, physical-GPU or commercial-game coverage. See `VALIDATION_REPORT.md` for the exact tested revision and the separate reference-image failure.
