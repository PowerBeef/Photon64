# Photon64 image-generation direction

Tool: built-in `image_gen`, two passes. The approved raster is
`photon64-master.png`; the initial distressed transparency pass is discarded.

## Initial design prompt

Use case: logo-brand. Create a completely new, polished visual identity for Photon64, an experimental Nintendo 64 emulator in one small browser HTML file. Deliver a single horizontal logo lockup on a truly transparent background: an original compact symbol on the left and the exact name "PHOTON64" on the right. Design both the symbol and the lettering from scratch. The symbol should be a memorable geometric P monogram suggesting a photon/light pulse and motion: a bold rounded outer loop with a decisive diagonal negative-space cut, readable at 24 pixels. Use one solid electric cyan/turquoise color for the symbol. Typography: custom substantial geometric sans-serif lettering, slightly wide, balanced optical spacing, rounded-square bowls, confident straight strokes, a few purposeful angular cuts; mature and precise, not playful hand lettering or thin monoline text. PHOTON in solid off-white and 64 in the same cyan as the symbol. Horizontal baseline, excellent legibility, no overlapping characters. The six letters P H O T O N followed by digits 6 4, exactly once. The monogram and wordmark must feel like a single carefully art-directed identity. Flat opaque fills only, very crisp clean silhouette edges, large high-resolution front-on artwork for accurate contour tracing to SVG. No gradients, glow, shadows, textures, mockup, bevel, cartridge outline, four-color pinwheel, extra words, labels, trademark marks, or background shapes. Leave comfortable transparent margins. The output is the actual logo artwork, not a presentation board.

## Refinement prompt used for the approved source

Refine this logo into pristine flat vector-style master artwork. Preserve the core symbol silhouette and exact custom PHOTON64 letter designs, but eliminate every scratch, speckle, distressed edge, outline, hole artifact, gradient and shadow. Render solid flat cyan (#00D9DE) and solid off-white (#F3F6FA) on a solid dark charcoal (#0D1117) background. All intentional counters must be dark background with smooth perfect edges. Give each letter a clear small gap so the text has optical spacing; do not touch adjacent letters. P H O T O N 6 4 must read distinctly and exactly. Preserve the P light-beam monogram but clean its diagonal negative space into a single smooth decisive cut. No new elements, no explanatory text, no mockup, no texture, no transparency. Horizontal lockup, large crisp artwork, comfortable margins. This clean source will be accurately traced to real SVG paths.

## Conversion policy

Trace the generated geometry instead of replacing it with a system font or
inventing approximate glyphs. Flatten the two requested fills; remove the
background, accidental tiny specks and raster texture. Keep intended counters,
the separated diagonal beam, relative letter shapes and original spacing.
Keep both the source and conversion measurements. Only paths enter the app.
