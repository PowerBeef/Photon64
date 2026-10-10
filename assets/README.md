# Photon64 identity

A four-color light-beam **P** pairs with custom geometric **PHOTON64** lettering.
The accepted symbol and lettering retain their original geometry, recolored to
match the emulator UI: green, blue, red and yellow. The lettering is drawn as filled
paths, including its counters and optical spacing. It is a custom wordmark,
not a general-purpose alphabet or an installable font.

| Asset | Use | Bytes |
|---|---|---:|
| [logo-mark.svg](logo-mark.svg) | Four-color symbol, transparent background | 1,577 |
| [logo-mark-mono.svg](logo-mark-mono.svg) | Single-color symbol using `currentColor` | 1,303 |
| [logo.svg](logo.svg) | Full lockup for light backgrounds | 6,073 |
| [logo-on-dark.svg](logo-on-dark.svg) | Full lockup for dark backgrounds | 6,089 |
| [wordmark.svg](wordmark.svg) | Lettering alone for light backgrounds | 4,659 |
| [wordmark-on-dark.svg](wordmark-on-dark.svg) | Lettering alone for dark backgrounds | 4,675 |
| [banner.svg](banner.svg) | Project README header | 6,471 |

Every logo is a real vector: no embedded bitmap, font, script, external URL or
filter. Logo lettering uses `currentColor`; external light variants default to
black, and dark variants explicitly use `#f3f4f6`. The symbol uses green `#22b35c`, blue `#3b7bff`, red `#ea4335`,
and yellow `#f6c21c`; “64” is yellow. These match `src/web/app.html` and
`src/web/art.js`. A small vector pattern colors the P loop; the beam is red.
The banner's explanatory subtitle uses system sans-serif; the brand name is
entirely paths. Preserve each `viewBox` and scale uniformly.

## Image generation and faithful conversion

The built-in image-generation tool designed both the symbol and lettering.
[The refined source master](design/photon64-master.png) and [prompt notes](design/prompt.md)
are retained for reproducibility. The first transparency pass had distressed
edge artifacts; the second generation produced the clean opaque master used
for tracing. No font was substituted during conversion.

Two flat-color masks remove the background and discard isolated raster specks
below 50 source pixels. Potrace fits cubic Bezier paths and preserves sharp
corners and letter counters. Colors are normalized to the requested flat fills.
The original cyan master remains the geometry reference; its colors are not
the shipped palette. Fidelity checks render the original two masks separately
from the final palette. Rasterizing those SVG masks at the original 2172×724 resolution gives cyan
silhouette IoU **99.767%** and lettering IoU **99.577%**, with maximum boundary
distances of **1.414px** and **2px** respectively. These are geometric comparisons
to the cleaned masks, not a claim of pixel-identical gradients or antialiasing.
[vectorization.json](design/vectorization.json) records parameters, source hash,
measurements and file sizes.

Rebuild the assets with optional design dependencies:

```sh
.venv/bin/python -m pip install -r tools/brand-requirements.txt
.venv/bin/python tools/vectorize-brand.py
```

The application build reads `logo-on-dark.svg` and embeds its paths once in the
self-contained HTML. `UiArt.logo()` supplies the responsive home header; its
accessible name is Photon64. Neither the raster master nor design dependencies
are included in the HTML or required for normal setup/build. There are no
network font requests. General menu text retains the readable system font.

Changing `.github/release.json` publishes a new version; updating these assets
alone does not replace an existing release.
