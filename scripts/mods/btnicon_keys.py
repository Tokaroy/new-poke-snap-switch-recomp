"""Button icon mod helper: data keys of the game's small UI textures (analysis/mods/button_icons.md).

The renderer (runtime/nvn/nvn_btnicon.cpp) keys a texture by FNV-1a 64 of all of its swizzled data (textures up to 64 KB only), which
are the bytes of the nutexb file before the 0xb0-byte footer. This script computes the same key for every nutexb under a folder, so a
key seen in a run (SNAP_BTNICON_TRACE) can be traced back to the extracted file. Output (local only, never committed: it names game files):

  python -I scripts/mods/btnicon_keys.py <folder> <out.tsv> [max_size]
  columns: key  w  h  nvn_format  mips  data_size  path

Only code lives here; no game data.
"""
import os, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests' / 'nvn'))
import nutexb_ref as nx  # noqa: E402


def fnv(b):
    h = 1469598103934665603
    for c in b:
        h ^= c
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def key_of(path):
    t = nx.parse(str(path))
    bpb, bw, bh, kind, srgb = nx.FORMATS[t['fmt']]
    lv, _ = nx.layout(t['w'], t['h'], 1, bpb, bw, bh, t['mips'], 1)
    total = sum(x[6] for x in lv)
    return fnv(t['data'][:total]), t


def main():
    folder, out = Path(sys.argv[1]), sys.argv[2]
    mx = int(sys.argv[3]) if len(sys.argv) > 3 else 512
    rows = []
    for f in sorted(folder.rglob('*.nutexb')):
        try:
            t = nx.parse(str(f))
        except Exception:
            continue
        if t['dim'] != 2 or t['layers'] != 1 or t['w'] > mx or t['h'] > mx or t['w'] < 8 or t['h'] < 8 or t['fmt'] not in nx.FORMATS:
            continue
        k, t = key_of(f)
        rows.append('%016x\t%d\t%d\t0x%x\t%d\t%d\t%s' % (k, t['w'], t['h'], t['fmt'], t['mips'], t['data_size'], f.relative_to(folder).as_posix()))
    with open(out, 'w', newline='\n') as o:
        o.write('\n'.join(rows) + '\n')
    print(len(rows), 'textures')


if __name__ == '__main__':
    main()
