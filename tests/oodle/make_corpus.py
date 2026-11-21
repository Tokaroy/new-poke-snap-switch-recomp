"""Collects real Oodle streams from the local romfs DRP archives for tests/oodle (project code, ours).

  python -I tests/oodle/make_corpus.py [--root CHECKOUT] [--out build/oodle/corpus.bin] [--per-ext K] [--max-mb M] [--census]

Writes game-derived bytes, so the output stays under build/ (git-ignored, never committed). Record layout, little endian:
u32 stream size, u32 decoded size, u32 CRC32 of the decoded data (the DRP chunk's word 0), then the stream (the payload
after the 'Oodl' tag, exactly what the game passes to OodleLZ_Decompress at main+0x7240). Picks K entries per file type
spread over all archives plus the largest of each type under 40 MB, stopping at --max-mb of decoded data. --census prints
the codec of every stream's first block (byte 1 & 0x7f: 6 Kraken, 10 Mermaid, 11 Selkie, 12 Hydra, 13 Leviathan).
"""
import argparse
import collections
import ctypes
import struct
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', type=Path, default=SRC)
    ap.add_argument('--out', type=Path, default=SRC / 'build' / 'oodle' / 'corpus.bin')
    ap.add_argument('--per-ext', type=int, default=6)
    ap.add_argument('--max-mb', type=float, default=400)
    ap.add_argument('--census', action='store_true')
    a = ap.parse_args()
    sys.path.insert(0, str(a.root / 'scripts'))
    import romfs_census as rc   # noqa: E402 (the checkout's DRP reader)
    rc.set_belownormal()
    rfs = a.root / 'extracted' / 'romfs-v262144'
    xor = ctypes.CDLL(str(a.root / 'tools' / 'oodle_build' / 'drp_xor.dll'))
    xor.drp_xor.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    byext = collections.defaultdict(list)
    for p in sorted(rfs.rglob('*.drp')):
        _, ents = rc.parse_drp(p)
        for e in ents:
            if e['oodle']:
                byext[e['ext']].append((p, e))
    pick = []
    for ext, lst in sorted(byext.items()):
        small = [x for x in lst if x[1]['dsize'] <= 40 << 20] or lst
        step = max(1, len(small) // a.per_ext)
        sel = small[::step][:a.per_ext] + [max(small, key=lambda x: x[1]['dsize'])]
        seen = set()
        for x in sel:
            k = (str(x[0]), x[1]['idx'])
            if k not in seen:
                seen.add(k)
                pick.append(x)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    codecs = collections.Counter()
    total = n = 0
    with open(a.out, 'wb') as o:
        for p, e in pick:
            if total + e['dsize'] > a.max_mb * (1 << 20):
                continue
            with open(p, 'rb') as f:
                table = f.read(0x94)[0x14:0x94]
                f.seek(e['off'])
                buf = bytearray(f.read(e['total']))
            if e['encrypted']:
                seed = struct.unpack_from('<I', buf, 0)[0]
                r = rc.rng_for(seed, table)
                st = (ctypes.c_uint32 * 4)(r.x, r.y, r.z, r.w)
                cb = (ctypes.c_char * len(buf)).from_buffer(buf)
                xor.drp_xor(ctypes.addressof(cb), len(buf) // 4, ctypes.addressof(st))
                del cb
            crc = struct.unpack_from('<I', buf, 0)[0]
            d0 = e['data_off'] - e['off']
            stream = bytes(buf[d0 + 4:d0 + e['csize']])
            codecs[stream[1] & 0x7f] += 1
            o.write(struct.pack('<3I', len(stream), e['dsize'], crc) + stream)
            total += e['dsize']
            n += 1
    print(f'{n} streams, {total / (1 << 20):.1f} MB decoded -> {a.out}')
    print('first-block codec ids:', dict(codecs))


if __name__ == '__main__':
    main()
