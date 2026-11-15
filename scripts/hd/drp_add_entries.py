# SPDX-License-Identifier: MIT
"""Add brand-new entries to a DRP archive (local mod files only): the way to give a material a texture the game never shipped.

Why: the game loads a material's textures from the archive's dependency table, not by name lookup. The table sits after the
last chunk (the "footer", encrypted with the archive header's own xorshift stream, restarted at word 0) and is a list of
groups: [magic, n, child_groups, NUL-terminated name padded to 4, n chunk indices]. `MI_<label>.drl` lists the textures,
numatb and matinstb of one material. A new chunk appended to the archive and added to that group is loaded with the
material, so its matinstb can name it (no "decoy" entry overwritten).

  python -I scripts/hd/drp_add_entries.py --archive nx/Pack/Characters/master/CH_007.drp \
      --add CH_007_00_0_A00_Body_nor.nutexb=<file> --group MI_CH_007_00_0_A00_Body.drl --out <pack>/romfs
  python -I scripts/hd/drp_add_entries.py --dump nx/Pack/Characters/master/CH_007.drp     (print the groups)

Writes <out>/<archive> (original chunks byte-identical, new chunks appended raw, header and footer re-encrypted).
Every group with the given name gets the new entries (a group can repeat inside the table). Reads extracted/romfs-v262144;
never writes outside --out. The new entry's payload can be a small placeholder: the loose-file mod loader then serves the
real texture by the (now existing) entry name.
"""
import argparse, struct, sys, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import romfs_census as rc   # noqa: E402

RFS = ROOT / 'extracted' / 'romfs-v262144'
EXT_IDS = {'nutexb': 0x5C156DBC}


def ext_id(ext):
    return EXT_IDS.get(ext.lower(), zlib.crc32(ext.lower().encode()) & 0xFFFFFFFF)


def xor_words(buf, rng, start=0):
    """XOR every 32-bit word of buf (len multiple of 4) from word `start` with the stream."""
    n = len(buf) // 4
    w = list(struct.unpack_from('<%dI' % n, buf))
    for i in range(start, n):
        w[i] ^= rng.word()
    return bytearray(struct.pack('<%dI' % n, *w))


def read_archive(path):
    f = open(path, 'rb').read()
    if struct.unpack_from('<I', f, 0)[0] != rc.DRPF_MAGIC:
        raise SystemExit('%s: not a DRP' % path)
    info, ents = rc.parse_drp(str(path))
    if not ents or not info.get('crc_ok'):
        raise SystemExit('%s: header did not parse (%s)' % (path, info))
    table = f[0x14:0x94]
    seed = struct.unpack_from('<I', f, 4)[0]
    enc = bool(info['encrypted'])
    hd = bytearray(f[:0x94])
    if enc:
        r = rc.rng_for(seed, table)
        for off in (8, 0xC, 0x10):
            struct.pack_into('<I', hd, off, struct.unpack_from('<I', hd, off)[0] ^ r.word())
    end = 0x94 + sum(e['total'] for e in ents)
    foot = bytearray(f[end:])
    if len(foot) % 4:
        raise SystemExit('%s: footer is %d bytes, not a multiple of 4' % (path, len(foot)))
    if enc:
        foot = xor_words(foot, rc.rng_for(seed, table))
    return dict(raw=f, hd=hd, table=table, enc=enc, ents=ents, end=end, foot=foot)


def parse_groups(foot):
    w = struct.unpack_from('<%dI' % (len(foot) // 4), foot)
    out, i = [], 0
    if not w:
        return out
    mag = w[0]
    while i < len(w):
        if w[i] != mag:
            raise SystemExit('footer: no group magic at word %d' % i)
        n, kids = w[i + 1], w[i + 2]
        j = (i + 3) * 4
        e = foot.index(b'\0', j)
        name = foot[j:e].decode('ascii')
        k = (e + 4) // 4
        out.append(dict(magic=mag, n=n, kids=kids, name=name, idx=list(w[k:k + n])))
        i = k + n
    return out


def build_groups(groups):
    b = bytearray()
    for g in groups:
        b += struct.pack('<III', g['magic'], len(g['idx']), g['kids'])
        nm = g['name'].encode('ascii')
        b += nm + b'\0' * (4 - len(nm) % 4)
        b += struct.pack('<%dI' % len(g['idx']), *g['idx'])
    return b


def make_chunk(name, ext, data, table, enc):
    nm = name.encode('ascii')
    hdr_len = 0x18 + ((len(nm) + 4) & ~3)
    pad = (-len(data)) % 4
    body = bytearray(hdr_len + len(data) + pad)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    struct.pack_into('<6I', body, 0, crc, rc.RESD, len(body), ext_id(ext), len(data), len(data))
    body[0x18:0x18 + len(nm)] = nm
    body[hdr_len:hdr_len + len(data)] = data
    if enc:
        body = xor_words(body, rc.rng_for(crc, table), start=1)
    return bytes(body)


def write_archive(a, adds, group_names, out_path):
    ents, groups = a['ents'], parse_groups(a['foot'])
    names = {e['name'].lower() for e in ents}
    new_chunks, new_idx = [], []
    for fname, data in adds:
        stem, ext = fname.rsplit('.', 1)
        if fname.lower() in names or stem.lower() in names:
            raise SystemExit('entry %s already exists in the archive; use a loose-file override instead' % fname)
        new_idx.append(len(ents) + len(new_chunks))
        # the chunk name keeps the extension, like every shipped entry ("CH_097_00_0_A00_Body_dif.nutexb"): the game finds a
        # material's textures by that full name. Without it the entry loads but the lookup fails and the stage start hangs
        # (2026-11-14: every rom-layer run stalled before the ride).
        new_chunks.append(make_chunk(fname, ext, data, a['table'], a['enc']))
    hit = 0
    for g in groups:
        if g['name'] in group_names:
            # shipped groups list textures first, then the numatb and matinstb: keep new textures ahead of the material files
            mat = [k for k, i in enumerate(g['idx']) if i < len(ents) and ents[i]['ext'].lower() in ('numatb', 'matinstb')]
            at = mat[0] if mat else len(g['idx'])
            g['idx'][at:at] = new_idx
            hit += 1
    missing = [n for n in group_names if not any(g['name'] == n for g in groups)]
    if missing:
        raise SystemExit('no group named %s' % ', '.join(missing))
    hd = bytearray(a['hd'])
    struct.pack_into('<I', hd, 0x10, len(ents) + len(new_chunks))
    seed = zlib.crc32(bytes(hd[0x10:0x94])) & 0xFFFFFFFF
    struct.pack_into('<I', hd, 4, seed)
    foot = build_groups(groups)
    if a['enc']:
        r = rc.rng_for(seed, a['table'])
        for off in (8, 0xC, 0x10):
            struct.pack_into('<I', hd, off, struct.unpack_from('<I', hd, off)[0] ^ r.word())
        foot = xor_words(foot, rc.rng_for(seed, a['table']))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(hd)
        f.write(a['raw'][0x94:a['end']])
        for c in new_chunks:
            f.write(c)
        f.write(foot)
    return hit


def verify(path, adds, group_names):
    a = read_archive(path)
    by = {e['name']: e for e in a['ents']}
    for fname, data in adds:
        e = by.get(fname)
        if not e:
            raise SystemExit('verify: %s missing' % fname)
        raw = bytearray(a['raw'][e['off']:e['off'] + e['total']])
        if a['enc']:
            raw = xor_words(raw, rc.rng_for(struct.unpack_from('<I', raw, 0)[0], a['table']), start=1)
        hl = e['data_off'] - e['off']
        got = bytes(raw[hl:hl + e['dsize']])
        if got != data or (zlib.crc32(got) & 0xFFFFFFFF) != struct.unpack_from('<I', raw, 0)[0]:
            raise SystemExit('verify: %s payload or crc differs' % fname)
    idx = {e['name']: e['idx'] for e in a['ents']}
    for g in parse_groups(a['foot']):
        if g['name'] in group_names:
            for fname, _ in adds:
                if idx[fname] not in g['idx']:
                    raise SystemExit('verify: %s not in group %s' % (fname, g['name']))
    return len(a['ents'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--archive', help='archive path under rom:/, e.g. nx/Pack/Characters/master/CH_007.drp')
    ap.add_argument('--add', action='append', default=[], help='<entry name.ext>=<file>')
    ap.add_argument('--group', action='append', default=[], help='dependency group that gets the new entries (repeatable)')
    ap.add_argument('--out', help='output root (the archive is written at <out>/<archive>)')
    ap.add_argument('--src', default=str(RFS), help='source RomFS root (default extracted/romfs-v262144)')
    ap.add_argument('--dump', help='print the dependency groups of this archive and exit')
    a = ap.parse_args()
    if a.dump:
        ar = read_archive(Path(a.src) / a.dump)
        for g in parse_groups(ar['foot']):
            print('%-40s kids=%d %s' % (g['name'] or '(root)', g['kids'],
                                        ' '.join('%s.%s' % (ar['ents'][i]['name'], ar['ents'][i]['ext']) for i in g['idx'])))
        return
    if not (a.archive and a.add and a.group and a.out):
        ap.error('--archive, --add, --group and --out are required')
    adds = []
    for s in a.add:
        n, p = s.split('=', 1)
        adds.append((n, Path(p).read_bytes()))
    src = Path(a.src) / a.archive
    out = Path(a.out) / a.archive
    ar = read_archive(src)
    hit = write_archive(ar, adds, set(a.group), out)
    n = verify(out, adds, set(a.group))
    print('%s: %d entries (+%d), %d group(s) updated -> %s' % (a.archive, n, len(adds), hit, out))


if __name__ == '__main__':
    main()
