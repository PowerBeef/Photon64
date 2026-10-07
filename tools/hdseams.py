# Joint check for high-resolution test dumps (tools/dawntest.mjs --hd L --images ...): inside a region drawn from texture
# tiles, the step from one emulated pixel to the next must not stand out from the steps between its sub-pixels.
#   hdseams.py image.png S x0 y0 x1 y1 [name [limit [axes]]]   (region in emulated pixels of the 320-wide picture; S = scale;
#                                                               axes: "y", "x" or "yx" - which directions have joints to check)
# Prints, per axis, mean boundary step / mean inside step (1 when the picture is interpolated evenly; tiles that stop
# interpolating at their edge give 2-4, lines between tiles more) and the worst boundary relative to the typical one.
# Exit 1 if the first looks like seams.
import sys
import numpy as np
from PIL import Image
im = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(int)
S = int(sys.argv[2]); x0, y0, x1, y1 = map(int, sys.argv[3:7]); name = sys.argv[7] if len(sys.argv) > 7 else ''
limit = float(sys.argv[8]) if len(sys.argv) > 8 else 1.75
axes = sys.argv[9] if len(sys.argv) > 9 else 'yx'
if im.shape[1] == 640 * S: im = im[:, ::2]         # dumps are 640 S wide: two output pixels per sub-pixel of a 320-wide picture
sx = sy = S
a = im[y0 * sy:y1 * sy, x0 * sx:x1 * sx]
bad = False
for axis, s in ((0, sy), (1, sx)):
    d = np.abs(np.diff(a, axis=axis)).mean(axis=(1 - axis, 2))      # step between neighbouring rows / columns
    k = np.arange(len(d)) % s
    b, inside = d[k == s - 1], d[k != s - 1]
    ratio, worst = b.mean() / max(inside.mean(), 1e-6), b.max() / max(np.median(b), 1e-6)
    if 'yx'[axis] not in axes: continue
    seam = ratio > limit
    bad |= seam
    print('%-28s %s  boundary/inside %.2f  worst/typical %.1f  %s' % (name, 'yx'[axis], ratio, worst, 'SEAM' if seam else 'ok'))
sys.exit(1 if bad else 0)
