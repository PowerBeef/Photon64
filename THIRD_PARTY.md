# Source, dependencies, and fixture provenance

Photon64's root license is MIT. npm package metadata now agrees with `LICENSE`; changing the manifest does not adjudicate the origin of every existing implementation detail.

| Material | Source / version | Usage / status |
|---|---|---|
| Angrylion RDP Plus | https://github.com/ata4/angrylion-rdp-plus at `9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc` | External native test oracle; not bundled in the HTML. External checkout contains `MAME License.txt` (including noncommercial redistribution conditions) and `CREDITS.txt`; preserve its notices and verify rights before redistribution. `src/rdp_pixel.h` acknowledges Angrylion formulations; a complete derivation/provenance review remains open. |
| krom homebrew tests and PNG references | Repository's existing `testroms/`; described in `AGENTS.md` as krom RSP tests; likely upstream https://github.com/PeterLemon/N64 | Existing fixtures retained unchanged. Exact upstream file revisions and applicable distribution permissions have not been independently verified here. Do not treat this table as a new license grant. |
| WASI SDK | https://github.com/WebAssembly/wasi-sdk release 34.0 | Build tool only; archive SHA256 verified against official release digest and pinned in `tools/versions.env`. |
| Playwright | npm 1.63.0, lockfile integrity | Development/browser testing dependency, Apache-2.0 package metadata. |
| Node WebGPU / Dawn | npm `webgpu` 0.6.2, lockfile integrity | Development GPU testing dependency; preserve package and binary notices if redistributed. |
| Pillow / NumPy / SciPy | Versions in `tools/requirements.txt` | Development image analysis; their own license notices apply. |
| User-supplied commercial cartridges | Local `roms/`, ignored by git; hashes only in generated local manifests | Test inputs only. No cartridge bytes are committed, uploaded to GitHub, fetched by setup, or included in the app bundle. |

Build outputs embed Photon64 core, frontend, shaders and existing art. `tools/romcheck.mjs` records exact artifact/fixture hashes for reproducibility; source revision and dirty state must accompany any distributed development build. No validated hardware-accuracy or broad compatibility claim accompanies this build.
