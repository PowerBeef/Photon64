# Photon64 agent guide

## Project and source map

Photon64 is an experimental Nintendo 64 emulator: a freestanding C core compiled to WASM, a JavaScript frontend, and native/WebGPU renderers. Read `DEVELOPMENT.md` for commands, `IMPLEMENTATION_PLAN.md` for priorities and acceptance criteria, `VALIDATION_REPORT.md` for evidence and open failures, `RENDERER_ACCURACY_IMPLEMENTATION.md` for current renderer changes, and `ACCURACY_INVESTIGATION.md` for historical traces and reference-policy limits. Deterministic boot runs or software/GPU agreement do not establish hardware accuracy.

- `src/n64.c` includes the core; `cpu.c`, `rsp.c`, `rdp.c`, `vi.c`, `bus.c`, `api.c` and `gpu.c` implement machine and host behavior.
- `src/web/app.js` owns sessions, persistence and UI; `gpu.js` owns GPU coherence; `*.wgsl` implement rendering/merge passes. `app.html` is the bundle template.
- `tools/` contains builds, tests, comparison gates and recorded inputs in `tools/inputs/`.
- `testroms/` contains shipped homebrew fixtures and reference images. `roms/` contains ignored local commercial fixtures.
- `out/photon64.html` is the canonical self-contained app; `out/n64.wasm` is the core. Outputs and dependencies are generated, not source.

## Working policy

- Work on `main`; do not create feature branches unless the user explicitly changes this policy.
- Inspect status and relevant code before editing. Preserve unrelated user changes and newer remote commits. Use `rg`/`rg --files` for source searches.
- Keep changes focused, maintain existing C/JavaScript style, and use ES module imports in `.mjs` tools.
- Implement authorized work autonomously. A connected account does not authorize unrelated publishing, destructive operations or credential changes.
- Before an authorized push, fetch/inspect remote `main`. Use a fast-forward update; GitHub connector ref updates must include the observed `expected_sha`. Do not force-push or bypass protections.
- Commit only source, tests and documentation. Never commit commercial ROM bytes, SDK/driver/browser packages, `node_modules/`, `.venv/`, or `out/`. Inspect the actual staged paths.
- Report changes, validation and unresolved failures. Distinguish local/published commits, queued/completed CI and unexecuted checks.

## Reproducible environment

Use Node 24+, Python 3.12 and a native C compiler (GCC 13 tested). `tools/versions.env` pins WASI SDK 34.0 with its archive digest and the Angrylion reference revision; npm and Python versions are pinned in lock/requirements files.

```sh
tools/setup.sh
. out/env.sh
npm run validate
```

Setup supports Linux x86_64 and macOS, installs local dependencies, and writes `out/env.sh`. Set `REF` to a separate pinned checkout; do not silently switch an existing checkout. `tools/setup.sh --browser` also installs Playwright Chromium. Use `npm ci`, not ad hoc dependency upgrades. In hosted Work environments, the runtime Node binary may need its provided runtime dependency directory added to `PATH` before setup.

`WASI_SDK_PATH` or `CC` select the WASM compiler; `NATIVE_CC` selects the native compiler for validation. `tools/build_native.sh` honors `REF`, `CFLAGS`, `LDFLAGS` and `REQUIRE_REFERENCE=1`. Missing required prerequisites fail; optional missing references remove stale oracle executables.

## Build and verification

- `npm run build`: compile WASM and assemble the HTML app.
- `npm run validate`: required baseline; native/WASM/app builds, JS regressions, native ASan/UBSan, identical O0/O3 WASM vectors, Expansion Pak checks, deterministic homebrew execution and comparator tests. Requires the pinned reference but no commercial ROM, browser or GPU.
- `npm test`: focused frontend/GPU/gate regressions; actual-core cases require an existing `out/n64.wasm`.
- Exact raw and independently referenced VI fixture lanes run within `npm run validate`. `.venv/bin/python tools/cmp_ref.py testroms 120` retains the superseded mixed-stage coarse diagnostic with unchanged thresholds/failures; CI records its log with `continue-on-error`.
- `npm run test:roms`: local commercial fixtures for 300 fields twice, with hashes. PASS means determinism, not compatibility.
- `tools/games.sh --check-roms`, then `GPU_RUNNER=dawn tools/games.sh`: recorded-input independent-reference and GPU/software lanes. Missing ROMs/adapters, incomplete checkpoints and counted mismatches fail.

Run checks appropriate to changes; documentation-only edits do not require rebuilding unchanged semantics. Add meaningful regressions for reproduced failures. Do not weaken thresholds or change expectations merely to get green results. Preserve PASS/FAIL/SKIP: `--noref` and `--noexact` are diagnostic SKIP. Gameplay GPU coverage remains VI RGB, last-batch framebuffer color/hidden/depth and three execution counters. Authored `rdp_gpu_vectors.js` batches additionally compare twelve retained registers; neither lane proves full machine-state conformance. Native oracle coverage includes all potentially touched targets and effective hidden bits. Upload CPU-owned changes before inspecting raw GPU-cache memory.

## Browser and GPU execution

```sh
node tools/browsercheck.mjs testroms/RSPCP2VRCP.N64
node tools/rdp_gpu_probe.mjs "roms/Mario Kart 64 (USA).z64"
```

`browsercheck.mjs` checks duplicate state saves, state restoration, IndexedDB battery persistence across reload, cached cartridge loading, quota failures, unavailable storage, invalid ROM replacement, presentation failure/reset, held-input release and page errors. `tools/device_loss.mjs` deliberately destroys a real Dawn device and requires lost barriers/reset to reject. Its independent CI job uses a shipped fixture; never upload commercial cartridges for CI.

The browser lane also checks keyboard/menu pause/resume and records traces, logs and screenshots for desktop and emulated mobile viewports. `BROWSER_ENGINE=webkit` selects Playwright WebKit; `BROWSER_CHANNEL=chrome`/`msedge` selects branded Chromium browsers. CDP CPU profiles are Chromium-only and SKIP in WebKit. `tools/responsivecheck.mjs` checks 15 viewport/DPR configurations, actual control reachability, safe-area exclusions, rotation, pad extremes, state-slot actions and presentation proportions. `tools/safaricheck.mjs` uses Apple's native macOS driver and shares DOM assertions through `tools/ui-audit.mjs`. Read `UI_COMPATIBILITY.md` for observed results and the native Safari OS-backed file-reader limitation; browser-backed File injection does not prove the native chooser. Preserve this distinction and physical-device limits in reports.

Traces may contain ROM bytes; keep commercial traces local. Use `tools/benchmark.mjs` for sequential fresh-process core+VI timings after recorded-input warmup, with final-state/image/audio agreement. `WASM_SYMBOLS=1 WASM_OUTPUT=out/n64-symbols.wasm ./build_wasm.sh` preserves function names for separate `--profile` replays. Shared-host field timing is not physical-device FPS.

Chromium harnesses must run outside the agent's command sandbox, where prior runs segfaulted. Use an approved outside-sandbox command, the user's terminal, or GitHub Actions. When the host disables escalation, do not repeatedly request it or change host security configuration. Document the unexecuted lane and use an authorized independent runner. GitHub access does not change local execution policy.

Use installed Playwright Chromium or `CHROME_EXECUTABLE`. Dawn uses the local npm `webgpu` bindings. On Linux, install an OS Vulkan driver or set `VK_ICD_FILENAMES` to an existing ICD; lavapipe is a software adapter. This workspace has generated `.tools/lvp_icd.json`, which is not portable. On macOS select `GPU_BACKEND=metal`. Name the tested adapter and distinguish shader/coherence evidence from physical performance and device-loss testing.

## Local ROM mapping

Quote paths with spaces. `tools/games.sh` resolves these names; a root `<short>.z64` overrides its `roms/` entry.

| Input key | Local fixture |
|---|---|
| `sm64` | `roms/Super Mario 64 (USA).z64` |
| `ge` | `roms/GoldenEye 007 (USA).z64` |
| `pd` | `roms/Perfect Dark (USA) (Rev 1).z64` |
| `mk64` | `roms/Mario Kart 64 (USA).z64` |
| `smash` | `roms/Super Smash Bros. (USA).z64` |
| `wdc` | `roms/World Driver Championship (USA).z64` |

## Integrations and next work

Use the GitHub plugin when available to inspect authenticated permissions, refs, code and Actions, and publish authorized source updates. Read generic Actions endpoints for push runs; the commit-workflow convenience tool currently filters to PR runs. Verify responses and remote content; uploading code does not mean CI passed. Use the local shell and committed harnesses for routine work. Do not assume Claude-specific DevTools, Context7, profiling or mobile-build MCP tools exist; discover exposed capabilities. Consult primary documentation when changing WebGPU/WGSL or platform behavior.

Priorities: residual independent renderer/reference discrepancies, broader VR4300 FPU hardware conformance beyond the exact-rational corpus, extended browser failure/lifecycle coverage, physical devices, and longer recorded-input coverage beyond the six bounded game scenarios. `THIRD_PARTY.md` records provenance; do not bundle the external oracle or claim a verified license chain where it remains unresolved.

## Current audit workflow

The frozen audit is `reports/audit-2026-10-10/REPORT.md`; implementation status is `reports/implementation-2026-10-10/REPORT.md`. Keep historical failures and label new evidence with source/core hashes. `MIGRATION.md` defines persistence and upgrade semantics, and `CAPABILITIES.md` separates implementation coverage from hardware conformance. `gpu_lifecycle.mjs` requires actual drawing on its authored cartridge; do not replace it with a zero-batch boot PASS. Preserve the separate intentionally failing RDP boundary-reference diagnostic until independent hardware evidence resolves the model disagreement. The required release job set is explicit in `tools/release-gate.cjs`; retries do not erase first-attempt evidence.
