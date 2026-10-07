# AGENTS.md
## Code Map

- `src/` — C core (`n64.c` plus cpu/rsp/rdp/vi/bus/api/gpu) and `src/web/` frontend (`app.html` template, `app.js`/`gpu.js`/`pad.js`/`art.js`, `*.wgsl` shaders)
- `tools/` — `build.mjs` bundler, Node/Python test harnesses, and `tools/inputs/` recorded input scripts (`sm64|ge|pd|mk64.txt`)
- `roms/` — local ROM library for manual and regression testing (not source); see `## ROMs` for the file mapping
- `build_wasm.sh`, `photon64.html` — WASM build script and distributable single-file app at the repo root (`out/` holds intermediate build outputs)
- `node_modules/`, `.venv/` — generated: local Playwright install and Python venv (see `## Setup`; never edit by hand)
- `testroms/` — krom RSP test ROMs (`.N64`) plus shipped reference screenshots (`.png`); see `## Validation`

## Conventions

- Use `import ... from ...` syntax in `.mjs` files.

## Setup (macOS, one time)

- `brew install llvm` — Apple clang cannot link wasm32; `build_wasm.sh` picks Homebrew LLVM automatically (`CC` or `WASI_SDK_PATH` override).
- `npm i playwright && npx playwright install chromium` — run from the repo root; installs the `playwright` package locally and the browsers to `~/Library/Caches/ms-playwright`. Ignore the "install dependencies first" warning if it appears — it only means the command ran outside the repo.
- `python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt` — Pillow/numpy/scipy for the image-diff scripts; `games.sh` uses the venv automatically.
- Clone `ata4/angrylion-rdp-plus` to `../ref/angrylion-rdp-plus` (next to the repo root) to enable the `out/oracle_nn` differential step.

## Build

- `./build_wasm.sh` — compile `src/n64.c` to `out/n64.wasm` (needs Homebrew LLVM on macOS; honors `CC`).
- `tools/build_native.sh` — build `out/native` always, plus `out/oracle_nn` when the Angrylion checkout is present.
- `node tools/build.mjs` — assemble `out/photon64.html` from template + WASM + shaders + `src/web/*.js`.

## Validation

- `node tools/run_wasm.mjs "roms/<game>.z64" [frames]` — headless WASM perf + parity checks; takes any ROM path (see `## ROMs`).
- `tools/games.sh [sm64|ge|pd|mk64 ...]` — full regression suite; resolves each short name from `roms/` (a `<short>.z64` file at the root overrides the library copy). Skips missing ROMs.
- `tools/games.sh --check-roms [...]` — report resolved ROM paths without running tests; exits nonzero if any requested ROM is missing. Run this first when the suite skips games.
- `node tools/dawntest.mjs ...` / `node tools/gputest.mjs ...` — WebGPU-vs-software harnesses backing `games.sh` (`<rom> <frames> "<checks>" "[inputs]"`); the suite auto-selects Dawn bindings when present, else headless Chromium (`GPU_RUNNER=dawn|browser` forces one). `tools/*.py` summarize and diff the logs.
- `.venv/bin/python tools/cmp_ref.py testroms [frames] [out-prefix]` — run the krom RSP test ROMs in `out/native` and compare green/red counts against the shipped reference screenshots.

## ROMs

Quote paths — file names contain spaces. Short names are the `games.sh` / `tools/inputs/` keys, and the mapping lives in `rom_for()` inside `tools/games.sh`. UI harnesses without a ROM argument default to the Mario ROM:

- `sm64` → `roms/Super Mario 64 (USA).z64`
- `ge` → `roms/GoldenEye 007 (USA).z64`
- `pd` → `roms/Perfect Dark (USA) (Rev 1).z64`
- `mk64` → `roms/Mario Kart 64 (USA).z64`
- Extras with no `games.sh` or input-script coverage: `roms/Super Smash Bros. (USA).z64`, `roms/World Driver Championship (USA).z64`

## MCP Tooling

- `chrome-devtools` — verify `photon64.html` and `tools/*.html` harnesses in a real browser: `new_page`/`navigate_page` to load, `take_snapshot` + `take_screenshot` to inspect UI, `list_console_messages` for `[core]`/JS errors, `performance_start_trace`/`performance_stop_trace` for frame-time work, `emulate` for mobile/touch viewports.
- `cua_driver` — end-to-end runs beyond DevTools: `browser_set_input_files` to load a ROM from `roms/` through the file picker (`.z64` accepted), `browser_click`/`browser_pointer` for trusted input, `start_recording`/`replay_trajectory` for repeatable regression trajectories.
- `context7` — look up current WebGPU/WGSL and Web API docs (`resolve_library_id`, then `query_docs`) before changing `src/web/gpu.js` or `*.wgsl` shaders.
- `axiom` — optional native profiling: `axiom_xcprof_record` against the `tools/native.c` / `tools/oracle.c` headless harnesses, `axiom_xcprof_analyze` for CPU/hang attribution, `axiom_xcprof_compare` for before/after gating.
- `github` / `mobilebuild` — not applicable here: no git remote or Xcode project in this workspace.
- Division of labor — committed Playwright scripts for repeatable runs; the DevTools MCP for interactive debugging of failures the suite found.
