# SPDX-License-Identifier: MIT
"""Cut-out masks of a course pack's alpha-tested materials, for the HD shadows bake (leaves, grass and fence cards cast
shadows through their alpha, not as solid quads).

For every `MI_*.matinstb` of the pack this reads the `baseColorTexture` name, decodes that texture's alpha at a mip of at
most `--max` pixels and, when the alpha really cuts the surface (at least `--min-cut` of the texels below 50%), writes
`<out>/<material>.png` (8-bit alpha). Opaque materials get no file, so the bake treats them as solid.

    python scripts/hd/hdshadows_alpha.py --pack <LC_001_A.drp folder> --out <folder> [--extra <other pack>...]

The output is game-derived: keep it in the local data folder, never commit it.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'builder' / 'snapbuild'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import hdtex_nutexb as N  # noqa: E402
import hdshadows_orig as O  # noqa: E402

BC1 = (0x480, 0x485)
BC3 = (0x4a0, 0x4a5)
BC7 = (0x4e0, 0x4e5)
RGBA8 = (0x400, 0x405)


def tokens(b):
    """Length-prefixed ASCII strings of a matinstb, in file order."""
    out, i = [], 0
    while i + 5 < len(b):
        n = struct.unpack_from('<I', b, i)[0]
        if 3 <= n < 200 and i + 4 + n < len(b) and b[i + 4 + n] == 0 and all(32 <= c < 127 for c in b[i + 4:i + 4 + n]):
            out.append(b[i + 4:i + 4 + n].decode())
            i = (i + 4 + n + 1 + 3) & ~3
        else:
            i += 4
    return out


def base_color(path):
    t = tokens(Path(path).read_bytes())
    for k, s in enumerate(t):
        if s == 'baseColorTexture':
            for s2 in t[k + 1:k + 3]:
                if s2.endswith('.nutexb'):
                    return s2
    return None


def bc1_alpha(blocks):
    """BC1 blocks (n, 8) -> (n, 16) alpha 0/255 (punch-through when colour0 <= colour1)."""
    c0 = blocks[:, 0].astype(np.uint32) | (blocks[:, 1].astype(np.uint32) << 8)
    c1 = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 8)
    bits = np.zeros(len(blocks), np.uint32)
    for i in range(4):
        bits |= blocks[:, 4 + i].astype(np.uint32) << np.uint32(8 * i)
    idx = np.stack([(bits >> np.uint32(2 * k)) & np.uint32(3) for k in range(16)], 1)
    transparent = (c0 <= c1)[:, None] & (idx == 3)
    return np.where(transparent, 0.0, 255.0).astype(np.float32)


def alpha_of(tex, mip):
    lb = N.level_blocks(tex, mip)
    hb, row = lb.shape
    if tex.fmt in BC3:
        wb = row // 16
        a = O.bc4(lb.reshape(hb * wb, 16)[:, :8])
    elif tex.fmt in BC1:
        wb = row // 8
        a = bc1_alpha(lb.reshape(hb * wb, 8))
    elif tex.fmt in BC7:
        import texture2ddecoder as T
        wb = row // 16
        rgba = np.frombuffer(T.decode_bc7(lb.tobytes(), wb * 4, hb * 4), np.uint8).reshape(hb * 4, wb * 4, 4)
        return rgba[:, :, 3].astype(np.float32)
    elif tex.fmt in RGBA8:
        return lb.reshape(hb, -1, 4)[:, :, 3].astype(np.float32)
    else:
        return None
    return a.reshape(hb, wb, 4, 4).transpose(0, 2, 1, 3).reshape(hb * 4, wb * 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pack', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--extra', nargs='*', default=[], help='more folders to look for textures in')
    ap.add_argument('--max', type=int, default=512)
    ap.add_argument('--min-cut', type=float, default=0.01)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    dirs = [Path(a.pack)] + [Path(x) for x in a.extra]
    n = cut = 0
    for mi in sorted(Path(a.pack).glob('*.matinstb')):
        n += 1
        tname = base_color(mi)
        if not tname:
            continue
        src = next((d / tname for d in dirs if (d / tname).is_file()), None)
        if src is None:
            continue
        try:
            tex = N.parse(src.read_bytes())
            mip = 0
            while mip + 1 < tex.mips and max(tex.w >> mip, tex.h >> mip) > a.max:
                mip += 1
            al = alpha_of(tex, mip)
        except Exception as e:  # noqa: BLE001 - one odd texture must not stop the pack
            print(f'{mi.stem}: {tname}: {e}', file=sys.stderr)
            continue
        if al is None:
            continue
        al = al[:max(1, tex.h >> mip), :max(1, tex.w >> mip)]
        frac = float((al < 127.5).mean())
        if frac < a.min_cut:
            continue
        Image.fromarray(np.clip(al, 0, 255).astype(np.uint8)).save(out / f'{mi.stem}.png')
        cut += 1
    print(f'{n} materials, {cut} alpha-tested masks written to {out}')


if __name__ == '__main__':
    main()
