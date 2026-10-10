"""Exact RGB comparison at the raw framebuffer stage; no scaling or tolerances."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import numpy as np
from PIL import Image


def difference(actual, reference):
    a, r = np.asarray(actual.convert('RGB')), np.asarray(reference.convert('RGB'))
    if a.shape != r.shape:
        return None
    return int(np.count_nonzero(np.any(a != r, axis=2)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('fields', type=int, nargs='?', default=120)
    args = parser.parse_args()
    if args.fields <= 0:
        parser.error('fields must be positive')
    roms = sorted(args.root.rglob('*.N64'))
    failed = 0
    with tempfile.TemporaryDirectory(prefix='photon-raw-') as tmp:
        for i, rom in enumerate(roms):
            ref = rom.with_suffix('.png')
            prefix = str(Path(tmp) / f'{i}_')
            try:
                subprocess.run(['out/native', str(rom), str(args.fields), '-raw', '-o', prefix,
                                '-e', str(args.fields)], check=True, capture_output=True, timeout=60)
                with Image.open(prefix + f'{args.fields:05d}.png') as actual, Image.open(ref) as expected:
                    bad = difference(actual, expected)
                ok = bad == 0
                print('PASS' if ok else 'FAIL', rom.name, 'raw RGB pixels different:', bad)
                failed += not ok
            except (OSError, subprocess.SubprocessError) as e:
                print('FAIL', rom.name, e)
                failed += 1
    print(f'{len(roms)} raw fixtures; {failed} failures')
    return int(bool(failed) or not roms)


if __name__ == '__main__':
    raise SystemExit(main())
