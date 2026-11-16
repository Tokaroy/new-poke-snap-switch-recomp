# SPDX-License-Identifier: MIT
r"""Builds the local parallax table (docs/pom.md) from the user's own extracted level textures.

  python -I scripts\hd\make_pom_table.py [--src <LevelComposite\master dir>] [--out FILE]

Every *_dif.nutexb under --src whose name says ground / sand (class 1), rock / stone (class 2) or bark (class 3) gets one line
`<key> <class> <stem>`; <key> is FNV-1a 64 of the first 4096 bytes of the file (the texture data the game holds), which the renderer
computes the same way (runtime/nvn/nvn_pom.cpp, as the shiny mod). Default output: build\pom\pom_table.txt, which is gitignored; the
renderer also looks in %APPDATA%\SnapRecomp\mods\pom\pom_table.txt, or SNAP_POM_TABLE=<file>. Nothing is downloaded and no game
data is printed.
"""
import argparse, os, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BARK = re.compile(r'bark', re.I)
ROCK = re.compile(r'(rock|stone)', re.I)
GROUND = re.compile(r'(ground|sand)', re.I)
SKIP = re.compile(r'(waterground|quicksand|backdrop|sky|leaf|flower)', re.I)


def fnv_key(data, n=4096):
    h = 1469598103934665603
    for b in data[:n]:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def classify(stem):
    s = re.sub(r'^SM_ENV_[0-9A-Za-z]+?_', '', stem)
    if SKIP.search(s):
        return 0
    if BARK.search(s):
        return 3
    if ROCK.search(s):
        return 2
    if GROUND.search(s):
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=str(ROOT / 'extracted' / 'drp-v262144' / 'nx' / 'Pack' / 'LevelComposite' / 'master'))
    ap.add_argument('--out', default=str(ROOT / 'build' / 'pom' / 'pom_table.txt'))
    a = ap.parse_args()
    rows = {}
    for dp, _, fs in os.walk(a.src):
        for f in fs:
            if not f.endswith('_dif.nutexb'):
                continue
            stem = f[:-len('_dif.nutexb')]
            c = classify(stem)
            if not c:
                continue
            with open(os.path.join(dp, f), 'rb') as fh:
                data = fh.read(4096)
            rows.setdefault(fnv_key(data), (c, stem))
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, 'w', newline='\n') as fh:
        for k in sorted(rows):
            fh.write('%016x %d %s\n' % (k, rows[k][0], rows[k][1]))
    n = [sum(1 for v in rows.values() if v[0] == c) for c in (1, 2, 3)]
    print('%d textures (ground %d, rock %d, bark %d) -> %s' % (len(rows), n[0], n[1], n[2], out))


if __name__ == '__main__':
    sys.exit(main())
