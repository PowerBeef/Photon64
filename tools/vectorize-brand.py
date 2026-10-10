#!/usr/bin/env python3
"""Trace the approved image-generation master; optional design tooling only.

Install tools/brand-requirements.txt, then run from any directory. The emulator
ships only SVG paths, never the master bitmap or the tracing dependencies.
"""
import hashlib
import io
import json
from pathlib import Path

import cairosvg
import numpy as np
import potrace
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / 'assets'
SOURCE = ASSETS / 'design/photon64-master.png'
CYAN = '#00d9de'
LIGHT = '#f3f6fa'
pixels = np.asarray(Image.open(SOURCE).convert('RGB'))
cyan = (pixels[:, :, 1] > 100) & (pixels[:, :, 2] > 100) & (pixels[:, :, 0] < 100)
white = pixels.min(axis=2) > 150

# Discard isolated raster specks rather than preserving them as logo geometry.
def clean(mask):
    labels, _ = ndimage.label(mask)
    sizes = np.bincount(labels.ravel())
    keep = sizes >= 50
    keep[0] = False
    return keep[labels]

cyan, white = clean(cyan), clean(white)


def trace(mask):
    """Fit cubic Beziers and sharp corners; preserve all letter counters."""
    commands = []
    def point(p):
        return f'{p.x:.1f},{p.y:.1f}'.replace('.0,', ',').removesuffix('.0')
    for curve in potrace.Bitmap(~mask).trace(turdsize=2, alphamax=0.7, opttolerance=0.2):
        commands.append('M' + point(curve.start_point))
        for segment in curve:
            if segment.is_corner:
                commands.append('L' + point(segment.c) + ' ' + point(segment.end_point))
            else:
                commands.append('C' + point(segment.c1) + ' ' + point(segment.c2) + ' ' + point(segment.end_point))
        commands.append('Z')
    return ''.join(commands)


def bounds(mask, padding=12):
    ys, xs = np.where(mask)
    return f'{xs.min()-padding} {ys.min()-padding} {xs.max()-xs.min()+1+2*padding} {ys.max()-ys.min()+1+2*padding}'


def svg(viewbox, paths, title='Photon64', color=None):
    color_attr = f' color="{color}"' if color else ''
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{viewbox}" fill-rule="evenodd" role="img" aria-label="{title}"{color_attr}><title>{title}</title>{paths}</svg>\n'


cyan_path, white_path = trace(cyan), trace(white)
paths = f'<path fill="{CYAN}" d="{cyan_path}"/><path fill="currentColor" d="{white_path}"/>'
viewbox = bounds(cyan | white)
(ASSETS / 'logo.svg').write_text(svg(viewbox, paths))
(ASSETS / 'logo-on-dark.svg').write_text(svg(viewbox, paths, color=LIGHT))
mark = cyan.copy()
mark[:, 500:] = False
mark_path = trace(mark)
(ASSETS / 'logo-mark.svg').write_text(svg(bounds(mark), f'<path fill="{CYAN}" d="{mark_path}"/>', 'Photon64 symbol'))
(ASSETS / 'logo-mark-mono.svg').write_text(svg(bounds(mark), f'<path fill="currentColor" d="{mark_path}"/>', 'Photon64 symbol'))
letters = cyan.copy()
letters[:, :500] = False
letter_paths = f'<path fill="{CYAN}" d="{trace(letters)}"/><path fill="currentColor" d="{white_path}"/>'
(ASSETS / 'wordmark.svg').write_text(svg(bounds(letters | white), letter_paths))
(ASSETS / 'wordmark-on-dark.svg').write_text(svg(bounds(letters | white), letter_paths, color=LIGHT))
banner = '<rect width="1280" height="400" rx="24" fill="#0d1117"/>'
banner += f'<svg x="100" y="70" width="1080" height="210" viewBox="{viewbox}" color="{LIGHT}">{paths}</svg>'
banner += '<text x="640" y="324" text-anchor="middle" fill="#9ba9bb" font-family="system-ui,-apple-system,Segoe UI,sans-serif" font-size="28" letter-spacing=".4">Nintendo 64. In your browser.</text>'
(ASSETS / 'banner.svg').write_text(svg('0 0 1280 400', banner, 'Photon64 — Nintendo 64 in your browser'))

# Compare filled silhouettes at the original resolution, separately by color.
render = svg(f'0 0 {pixels.shape[1]} {pixels.shape[0]}', paths, color=LIGHT)
png = cairosvg.svg2png(bytestring=render.encode())
result = np.asarray(Image.open(io.BytesIO(png)).convert('RGBA'))
actual_cyan = (result[:, :, 3] >= 128) & (result[:, :, 0] < 100)
actual_white = (result[:, :, 3] >= 128) & (result[:, :, 0] > 150)
metrics = {}
for name, original, traced in [('cyan', cyan, actual_cyan), ('lettering', white, actual_white)]:
    iou = (original & traced).sum() / (original | traced).sum()
    a = original ^ ndimage.binary_erosion(original)
    b = traced ^ ndimage.binary_erosion(traced)
    edge_error = max(ndimage.distance_transform_edt(~a)[b].max(), ndimage.distance_transform_edt(~b)[a].max())
    metrics[name] = {'silhouette_iou': round(float(iou), 6), 'max_edge_distance_source_pixels': round(float(edge_error), 3)}
    if iou < .99 or edge_error > 3:
        raise SystemExit(f'Conversion fidelity failed: {name}: {metrics[name]}')
report = {'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(), 'source_pixels': [pixels.shape[1], pixels.shape[0]], 'method': 'Two flat-color masks; isolated components below 50px discarded; Potrace cubic Bezier fitting; alphamax 0.7; 0.2px optimization tolerance; 0.1px coordinate precision.', 'metrics': metrics, 'svg_bytes': {p.name: p.stat().st_size for p in sorted(ASSETS.glob('*.svg'))}}
(ASSETS / 'design/vectorization.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
