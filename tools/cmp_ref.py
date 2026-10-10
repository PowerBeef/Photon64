"""Coarse RSP screenshot gate, with fixed reference tolerances (not pixel accuracy)."""
import glob
import os
import subprocess
import sys
import tempfile
from PIL import Image
import numpy as np

def counts(im):
    a = np.asarray(im.convert('RGB').resize((640, 480))).astype(int)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    return np.array([((r > 140) & (g > 140) & (b > 140)).sum(),
                     ((g > 100) & (r < 90) & (b < 90)).sum(),
                     ((r > 140) & (g < 90) & (b < 90)).sum()])

def acceptable(actual, reference):
    # Fixed 4% green / 6% red / 6% text count tolerances; a black frame
    # cannot normalize itself into a PASS. This remains a coarse content gate.
    return all(abs(int(a) - int(r)) <= max(20, int(r) * t)
               for a, r, t in zip(actual, reference, (0.06, 0.04, 0.06)))

def main():
    root = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    if frames <= 0:
        raise ValueError('frames must be positive')
    roms = sorted(glob.glob(root + '/**/*.N64', recursive=True))
    bad, compared = 0, 0
    with tempfile.TemporaryDirectory(prefix='photon-ref-') as temp:
        for index, rom in enumerate(roms):
            base = rom[:-4]
            refs = sorted(set(glob.glob(glob.escape(base) + '*.png')))
            if not refs:
                print('FAIL missing reference:', rom); bad += 1; continue
            prefix = os.path.join(temp, str(index) + '_')
            try:
                subprocess.run(['./out/native', rom, str(frames), '-o', prefix, '-e', str(frames)],
                               capture_output=True, timeout=60, check=True)
                actual = counts(Image.open(prefix + f'{frames:05d}.png'))
                references = [counts(Image.open(r)) for r in refs]
                compared += 1
                ok = any(acceptable(actual, ref) for ref in references)
                print('PASS' if ok else 'FAIL', os.path.basename(rom), actual.tolist(),
                      'references', [r.tolist() for r in references])
                bad += not ok
            except (OSError, subprocess.SubprocessError) as e:
                print('FAIL', rom, e); bad += 1
    print(f'{compared} ROMs compared; {bad} failures (fixed count tolerances)')
    return int(bool(bad) or not compared)

if __name__ == '__main__':
    sys.exit(main())
