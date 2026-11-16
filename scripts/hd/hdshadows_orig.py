# SPDX-License-Identifier: MIT
"""Dump the game's own sun-shadow textures (`<level>^<model>^<hash>^<shape>^<n>^Shadow.nutexb`, BC3 with the shadow in
alpha) of one course pack to 8-bit PNG masks, for the HD shadows bake tool's calibration and size lookup.

    python -I scripts/hd/hdshadows_orig.py --pack <LC_001_A.drp folder> --out <folder>

The output is game-derived: keep it in the local data folder, never commit it.
"""
import argparse
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'builder' / 'snapbuild'))
import hdtex_nutexb as N  # noqa: E402


def bc4(blocks):
    """BC4 blocks (n, 8) uint8 -> (n, 16) float32 values 0..255."""
    a0 = blocks[:, 0].astype(np.float32)
    a1 = blocks[:, 1].astype(np.float32)
    bits = np.zeros(len(blocks), dtype=np.uint64)
    for i in range(6):
        bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = np.stack([(bits >> np.uint64(3 * k)) & np.uint64(7) for k in range(16)], 1).astype(np.int64)
    pal = np.zeros((len(blocks), 8), np.float32)
    pal[:, 0] = a0
    pal[:, 1] = a1
    g = a0 > a1
    for k in range(1, 7):
        pal[:, k + 1] = np.where(g, ((7 - k) * a0 + k * a1) / 7, 0)
    for k in range(1, 5):
        pal[:, k + 1] = np.where(g, pal[:, k + 1], ((5 - k) * a0 + k * a1) / 5)
    pal[:, 6] = np.where(g, pal[:, 6], 0)
    pal[:, 7] = np.where(g, pal[:, 7], 255)
    return np.take_along_axis(pal, idx, 1)


def alpha_of(tex):
    lb = N.level_blocks(tex, 0)
    hb, wb16 = lb.shape
    wb = wb16 // 16
    a = bc4(lb.reshape(hb * wb, 16)[:, :8]).reshape(hb, wb, 4, 4)
    return a.transpose(0, 2, 1, 3).reshape(hb * 4, wb * 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pack', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    n = 0
    for f in sorted(os.listdir(a.pack)):
        if not f.endswith('^Shadow.nutexb'):
            continue
        dst = out / (f[:-len('.nutexb')] + '.png')
        if dst.exists():
            continue
        tex = N.parse(Path(a.pack, f).read_bytes())
        if tex.fmt not in (0x4a0, 0x4a5) or not tex.plain_2d:
            continue
        Image.fromarray(alpha_of(tex).astype(np.uint8)).save(dst)
        n += 1
        if a.limit and n >= a.limit:
            break
    print(f'{n} masks written to {out}')


if __name__ == '__main__':
    main()
