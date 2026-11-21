# SPDX-License-Identifier: MIT
"""Turn baked sun-shadow masks (8-bit PNG from `tools/hd-shadows`) into game textures in a local mod pack.

The game's `...^Shadow.nutexb` is BC3 (sRGB) with the shadow in alpha and black colour. This writes the same format at a
higher resolution, with a full mip chain, under `<pack>/nx/Pack/LevelComposite/master/<course>.drp/`, using the original
file's footer (name, tile, version) as the template.

    python -I scripts/hd/hdshadows_pack.py --masks <folder of png> --pack <mods folder>/hd-shadows-rebaked --course LC_001_A \
        --source <extracted LC_001_A.drp folder> --report <bake report.tsv> --fallback <Stage A pack>/.../LC_001_A.drp

Per map: the re-bake is used when its score in the bake report (shrunk to the original size, against the original) is at
least --min-bacc and --min-agree; otherwise the Stage A (upscaled) map from --fallback is copied; with neither the map is
left out, so the game's own is used. Prints one JSON line with the three counts.
"""
import argparse
import json
import os
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'builder' / 'snapbuild'))
import hdtex_nutexb as N  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent))
import hdshadows_orig as O  # noqa: E402

BC3_SRGB = 0x4a5
PACK_SUB = Path('nx/Pack/LevelComposite/master')


def encode_bc3_mask(a):
    """a: float32 (H, W) 0..255 -> block rows (ceil(H/4), ceil(W/4)*16) uint8, colour black. A side that is not a multiple
    of 4 (a small mip of a 168 or 336 map) is padded with its edge, as BC blocks cover it."""
    a = np.pad(a, ((0, -a.shape[0] % 4), (0, -a.shape[1] % 4)), mode='edge')
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


def composite(a, care_png, fallback, source=None):
    """The re-bake where it was scored (texels facing the sun, `<stem>.care.png` from the bake), the Stage A map elsewhere
    (back faces, gutters, see-through parts of cards), so nothing the re-bake did not judge changes from Stage A. A map
    with no Stage A file uses the game's own map, enlarged, there."""
    if not care_png or not Path(care_png).is_file():
        return a
    base = fallback if fallback and Path(fallback).is_file() else source
    if not base or not Path(base).is_file():
        return a
    care = np.asarray(Image.open(care_png).convert('L')) > 127
    t = N.parse(Path(base).read_bytes())
    b = O.alpha_of(t)[:t.h, :t.w].astype(np.float32)
    if b.shape != a.shape:
        b = np.asarray(Image.fromarray(b.astype(np.uint8)).resize((a.shape[1], a.shape[0]), Image.BILINEAR), dtype=np.float32)
    if care.shape != a.shape:
        return a
    return np.where(care, a, b)


def pack_one(png, template_path, dst, care=None, fallback=None):
    a = composite(np.asarray(Image.open(png).convert('L'), dtype=np.float32), care, fallback, template_path)
    h, w = a.shape
    tpl = N.parse(Path(template_path).read_bytes())
    rows = [encode_bc3_mask(m) for m in mips(a)]
    blob = N.build(tpl, BC3_SRGB, w, h, rows)
    tmp = Path(str(dst) + '.part')
    tmp.write_bytes(blob)
    os.replace(tmp, dst)


def read_report(path):
    """report.tsv of `hd-shadows bake`: file name -> (balanced accuracy, agreement, texels scored)."""
    out = {}
    if not path or not Path(path).is_file():
        return out
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        c = line.split('\t')
        if len(c) >= 9:
            out[c[2]] = (float(c[5]), float(c[6]), int(c[8]))
    return out


def good(score, min_bacc, min_agree):
    """A re-baked map replaces the game's only when, shrunk back to the original size, it matches the original well."""
    if score is None:
        return False
    bacc, agree, n = score
    if n < 32:   # almost nothing the game shows lit: keep the re-bake only if it agrees on what there is
        return agree >= max(min_agree, 0.9)
    return bacc >= min_bacc and agree >= min_agree


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--masks', required=True)
    ap.add_argument('--pack', required=True)
    ap.add_argument('--course', required=True)
    ap.add_argument('--source', required=True, help='the course pack folder under extracted/ (templates)')
    ap.add_argument('--report', help='report.tsv of the bake (per map scores); without it every mask is used')
    ap.add_argument('--fallback', help='the same course folder of the Stage A (upscaled) pack: used for maps whose re-bake '
                                       'scores too low and for maps with no re-bake')
    ap.add_argument('--min-bacc', type=float, default=0.85)
    ap.add_argument('--min-agree', type=float, default=0.88)
    a = ap.parse_args()
    dest = Path(a.pack) / PACK_SUB / f'{a.course}.drp'
    dest.mkdir(parents=True, exist_ok=True)
    rep = read_report(a.report)
    baked = fell = skipped = 0
    for src in sorted(Path(a.source).glob('*^Shadow.nutexb')):
        name = src.name
        png = Path(a.masks) / (src.stem + '.png')
        ok = png.is_file() and (not a.report or good(rep.get(name), a.min_bacc, a.min_agree))
        fb = Path(a.fallback) / name if a.fallback else None
        if ok:
            pack_one(png, src, dest / name, Path(a.masks) / (src.stem + '.care.png'), fb)
            baked += 1
            continue
        if fb and fb.is_file():
            tmp = dest / (name + '.part')
            shutil.copyfile(fb, tmp)
            os.replace(tmp, dest / name)
            fell += 1
        else:
            (dest / name).unlink(missing_ok=True)
            skipped += 1
    pj = Path(a.pack) / 'pack.json'
    if not pj.exists():
        pj.write_text(json.dumps({'name': 'HD shadows, re-baked (made on this PC)', 'version': '1.0.0', 'author': 'Snap Recomp Builder',
                                  'description': 'Sun shadow maps re-baked at higher resolution from your own game files. Not for sharing.'}, indent=1))
    print(json.dumps({'course': a.course, 'rebaked': baked, 'upscaled': fell, 'original': skipped}))


if __name__ == '__main__':
    main()
