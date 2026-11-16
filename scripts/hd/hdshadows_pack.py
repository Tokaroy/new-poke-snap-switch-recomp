# SPDX-License-Identifier: MIT
"""Turn baked sun-shadow masks (8-bit PNG from `tools/hd-shadows`) into game textures in a local mod pack.

The game's `...^Shadow.nutexb` is BC3 (sRGB) with the shadow in alpha and black colour. This writes the same format at a
higher resolution, with a full mip chain, under `<pack>/nx/Pack/LevelComposite/master/<course>.drp/`, using the original
file's footer (name, tile, version) as the template.

    python -I scripts/hd/hdshadows_pack.py --masks <folder of png> --pack <mods folder>/hd-shadows --course LC_001_A \
        --source <extracted LC_001_A.drp folder>
"""
import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'builder' / 'snapbuild'))
import hdtex_nutexb as N  # noqa: E402

BC3_SRGB = 0x4a5
PACK_SUB = Path('nx/Pack/LevelComposite/master')


def encode_bc3_mask(a):
    """a: float32 (H, W) 0..255, H and W multiples of 4 -> block rows (H/4, W/4*16) uint8, colour black."""
    h, w = a.shape
    blk = a.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
    hi = blk.max(1)
    lo = blk.min(1)
    flat = hi - lo < 0.5
    span = np.where(flat, 1.0, hi - lo)
    lvl = np.clip(np.rint((blk - lo[:, None]) / span[:, None] * 7), 0, 7).astype(np.int64)   # 0 = a1(lo), 7 = a0(hi)
    # palette order of the 8-value mode (a0 > a1): index 0 = a0, 1 = a1, 2..7 = a0 + (a1 - a0) * k/7 (k = 1..6)
    code = np.where(lvl == 7, 0, np.where(lvl == 0, 1, 8 - lvl))
    code = np.where(flat[:, None], 0, code)
    bits = np.zeros(len(blk), dtype=np.uint64)
    for i in range(16):
        bits |= code[:, i].astype(np.uint64) << np.uint64(3 * i)
    out = np.zeros((len(blk), 16), np.uint8)
    a0 = np.rint(hi).astype(np.uint8)
    a1 = np.where(flat, a0, np.rint(lo)).astype(np.uint8)
    out[:, 0] = a0
    out[:, 1] = a1
    for i in range(6):
        out[:, 2 + i] = ((bits >> np.uint64(8 * i)) & np.uint64(255)).astype(np.uint8)
    # colour half: both endpoints black, all indices 0
    return out.reshape(h // 4, (w // 4) * 16)


def mips(a):
    levels = [a]
    while min(levels[-1].shape) > 4:
        c = levels[-1]
        h, w = c.shape[0] // 2 * 2, c.shape[1] // 2 * 2
        levels.append(c[:h, :w].reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3)))
    return levels


def pack_one(png, template_path, dst):
    a = np.asarray(Image.open(png).convert('L'), dtype=np.float32)
    h, w = a.shape
    tpl = N.parse(Path(template_path).read_bytes())
    rows = [encode_bc3_mask(m) for m in mips(a)]
    blob = N.build(tpl, BC3_SRGB, w, h, rows)
    tmp = Path(str(dst) + '.part')
    tmp.write_bytes(blob)
    os.replace(tmp, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--masks', required=True)
    ap.add_argument('--pack', required=True)
    ap.add_argument('--course', required=True)
    ap.add_argument('--source', required=True, help='the course pack folder under extracted/ (templates)')
    a = ap.parse_args()
    dest = Path(a.pack) / PACK_SUB / f'{a.course}.drp'
    dest.mkdir(parents=True, exist_ok=True)
    n = 0
    for png in sorted(Path(a.masks).glob('*.png')):
        name = png.stem + '.nutexb'
        src = Path(a.source) / name
        if not src.is_file():
            continue
        pack_one(png, src, dest / name)
        n += 1
    pj = Path(a.pack) / 'pack.json'
    if not pj.exists():
        pj.write_text(json.dumps({'name': 'HD shadows', 'version': '1.0.0', 'author': 'local build',
                                  'description': 'Sun shadow maps baked at higher resolution from your own game files.'}, indent=1))
    print(f'{n} shadow textures written to {dest}')


if __name__ == '__main__':
    main()
