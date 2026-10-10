# Photon64 identity

The light-aperture mark uses four console-inspired colors around a transparent
spark. The wordmark is original vector geometry, including the forward-leaning
blue `64`. There are no embedded images, font files, external resources, or
scripts in these SVGs.

| Asset | Use | Bytes |
|---|---|---:|
| [logo-mark.svg](logo-mark.svg) | Compact icon; transparent background | 417 |
| [logo.svg](logo.svg) | Full wordmark; dark lettering for light backgrounds | 1,111 |
| [logo-on-dark.svg](logo-on-dark.svg) | Full wordmark; light lettering for dark backgrounds | 1,127 |
| [banner.svg](banner.svg) | Project README header | 1,691 |

The logo files use only paths and groups. Lettering uses `currentColor`, so an
inline logo can inherit the application's text color. The dark-background
variant sets an explicit light color for use as an external image. The banner's
subtitle uses a system sans-serif fallback, without loading a font.

To use the mark or wordmark in the self-contained app, inline the SVG directly
in the HTML template or the existing JavaScript artwork helpers. A separate
image URL would make the app depend on another file. Preserve the `viewBox` and
give the inline SVG a CSS width or height; it will scale without raster assets.

Edit these SVGs as the source artwork. Keep the aperture transparent, the four
segments separate, and the aspect ratio intact. The previous PNG banner and its
platform-specific raster generator have been replaced by `banner.svg`.

This asset update changes the project page only; the emulator's existing UI
artwork is not replaced automatically.
