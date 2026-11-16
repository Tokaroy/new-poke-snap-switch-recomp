"""Check an HD shadows pack against the original shadow maps, every course.

Each output map shrunk back to the original size must match the original alpha.

python -I scripts/hd/hdshadows_verify.py <originals master dir> <pack master dir> [maps per pack]

Both folders hold one `LC_*.drp` folder per course pack (`nx/Pack/LevelComposite/master`). The originals come from
`hdtex --from-romfs` (`<work>/romfs-src/...`), the pack from `<mods>/hd-shadows/...`. Uses a fixed seed, so a rerun
samples the same maps.
"""
import os
import random
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / 'builder' / 'snapbuild'))
sys.path.insert(0, str(HERE))
import hdtex_nutexb as N  # noqa: E402
import hdshadows_orig as O  # noqa: E402


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    per = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    random.seed(7)
    sampled = bad = 0
    rows = []
    for pk in sorted(os.listdir(out)):
        files = sorted(f for f in os.listdir(out / pk) if f.endswith('Shadow.nutexb'))
        mad, agree = [], []
        for f in random.sample(files, min(per, len(files))):
            a = O.alpha_of(N.parse((src / pk / f).read_bytes())).astype(np.float32) / 255
            b = O.alpha_of(N.parse((out / pk / f).read_bytes())).astype(np.float32) / 255
            h, w = a.shape
            H, W = b.shape
            if H % h or W % w:
                bad += 1
                continue
            s = H // h
            d = b.reshape(h, s, w, s).mean((1, 3))
            mad.append(np.abs(d - a).mean())
            agree.append(((d > .5) == (a > .5)).mean())
            sampled += 1
        if mad:
            rows.append((pk, len(files), len(mad), np.mean(mad), min(agree)))
            print('%-14s files %4d sampled %2d  mean|diff| %.4f  worst agreement %.4f' % rows[-1], flush=True)
    print('sampled %d, size mismatches %d' % (sampled, bad))
    if rows:
        print('mean|diff| %.4f, worst agreement %.4f' % (np.mean([r[3] for r in rows]), min(r[4] for r in rows)))


if __name__ == '__main__':
    main()
