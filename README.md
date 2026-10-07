# Photon64

A Nintendo 64 emulator that runs entirely in the browser: a C core compiled to
WebAssembly, a WebGPU renderer with an exact software fallback, shipped as one
self-contained HTML file. No server, no install — open it and drop in a ROM.

## Features

- Cycle-minded C core (CPU, RSP, RDP, VI) running as SIMD128 WebAssembly
- WebGPU renderer for speed, pixel-exact software renderer as fallback
- Single-file app: game library, drag-and-drop (including `.zip`), save states
- Full input: remappable keyboard, gamepads, and touch controls
- Differential test suite: software renderer checked against Angrylion,
  WebGPU checked against software, per-game regression runs

## Quick start

1. Build `out/photon64.html` (below), or use a prebuilt copy.
2. Open it in a recent Chrome or Edge (WebGPU required for the fast renderer).
3. Drop a ROM dump onto the window and play.

## Controls

Default keyboard layout (remappable in Settings):

| N64 | Keyboard |
|---|---|
| Stick | Arrow keys |
| A / B | X / C |
| Z / Start | Z / Enter |
| L / R | Q / E |
| C buttons | I / K / J / L |
| D-pad | T / G / F / H |
| Walk modifier | Shift |
| Fast-forward | Tab |
| Pause | P |
| Save state | F2 |
| Menu | Esc |

## Building from source

Prerequisites (macOS): `brew install llvm lld`, Node 24+, Python 3 with
`pillow numpy scipy` (`tools/requirements.txt`), and your own ROM dumps — see
`AGENTS.md` for the full setup.

```sh
./build_wasm.sh        # C core -> out/n64.wasm
node tools/build.mjs   # bundle -> out/photon64.html
```

Native helpers (headless runs, oracle differential tests):

```sh
tools/build_native.sh  # out/native, out/oracle_nn (needs ../ref/angrylion-rdp-plus), out/rsptest
```

## Testing

```sh
tools/games.sh [sm64|ge|pd|mk64 ...]   # full regression suite (needs ROMs, see AGENTS.md ## ROMs)
tools/games.sh --check-roms            # verify ROM resolution without running tests
.venv/bin/python tools/cmp_ref.py testroms   # krom RSP test ROMs vs references
./out/rsptest                          # RSP vector-op unit tests (no ROM needed)
```

Details, GPU-runner selection, and the MCP-assisted workflow live in
[`AGENTS.md`](AGENTS.md).

## Layout

- `src/` — C core plus the `src/web/` frontend (JS, WGSL shaders, HTML template)
- `tools/` — bundler, Node/Python test harnesses, RSP unit tests
- `testroms/` — krom RSP test ROMs and reference screenshots
- `roms/` — local ROM library (never committed; see `.gitignore`)
- `out/` — build outputs (`n64.wasm`, `photon64.html`, native binaries)

## ROMs

Photon64 plays dumps of cartridges you own. It reads `.z64`, `.n64`, `.v64`,
`.rom`, `.bin`, and `.zip`, auto-detecting byte order — no ROMs are included.

## License

MIT — see [LICENSE](LICENSE).
