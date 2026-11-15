# SPDX-License-Identifier: MIT
"""Point an existing texture slot of a material at another texture file (local mod files only).

Use: fur ("flocky") bodies bind the shared noise normal `dummy_flocky_texture` (Bouffalant BodyB, Scorbunny Body, Aipom ...)
or an empty name `.nutexb` (Grookey Body);
pointing that slot at an authored `<label>_nor` (added to the archive with drp_add_entries.py, or already there) gives the
body real relief with its own shader, no twin swap. Rewrites the slot's file string and its crc32 (crc of "<file>.nutexb",
the way the game stores it) with the round-trip FlatBuffers encoder of make_material_mod.py.

  python -E -P scripts/hd/matinstb_repoint.py --pack <pack> --material CH_097/MI_CH_097_00_0_A00_BodyB \
      --old dummy_flocky_texture.nutexb --new CH_097_00_0_A00_Body_nor.nutexb
Writes <pack>/nx/Pack/Characters/master/<CH>.drp/<label>.matinstb. Reads extracted/drp-v262144 only.
"""
import argparse, struct, sys, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_material_mod as mm   # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pack', required=True)
    ap.add_argument('--material', required=True, help='CH_097/MI_CH_097_00_0_A00_BodyB')
    ap.add_argument('--old', required=True, help='current file string of the slot, with .nutexb')
    ap.add_argument('--new', required=True, help='new file string, with .nutexb')
    a = ap.parse_args()
    if not (a.old.endswith('.nutexb') and a.new.endswith('.nutexb')):
        raise SystemExit('--old and --new must include .nutexb (that is how the game stores slot files)')
    ch, label = a.material.split('/')
    src = mm.CHARS / (ch + '.drp') / (label + '.matinstb')
    raw = src.read_bytes()
    # FlatBuffers string = u32 length + bytes + NUL; matching the length too keeps ".nutexb" (an empty slot) from
    # matching the tail of every other file name
    if raw.count(struct.pack('<I', len(a.old)) + a.old.encode() + b'\0') != 1:
        raise SystemExit('%s: file string %s not found exactly once' % (src, a.old))
    co, cn = struct.pack('<I', zlib.crc32(a.old.encode())), struct.pack('<I', zlib.crc32(a.new.encode()))
    if raw.count(co) != 1:
        raise SystemExit('%s: crc of %s found %d times (want 1)' % (src, a.old, raw.count(co)))
    out = mm.rewrite_matinstb(src, {a.old: a.new}, None, None)
    if out.count(co) != 1:
        raise SystemExit('re-encoded file: old crc found %d times' % out.count(co))
    out = out.replace(co, cn)
    dst = Path(a.pack) / 'nx/Pack/Characters/master' / (ch + '.drp') / (label + '.matinstb')
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(out)
    print('%s: %s -> %s (%d bytes) -> %s' % (label, a.old, a.new, len(out), dst))


if __name__ == '__main__':
    main()
