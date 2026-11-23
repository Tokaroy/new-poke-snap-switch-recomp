"""Which texture packs change the same game files, and which pack wins (read only).

Reads the mods folder the way the game does (mods.json order, first enabled pack wins; runtime/mods/mods_core.cpp) and counts, for every
pair of enabled packs, the files both change. Packs are folders or .zip files. Nothing is changed or uploaded.

  python -I scripts/mods/overlap_report.py [--mods-root DIR] [--out DIR] [--identical]

Default root: SNAP_MODS_DIR or %APPDATA%\\SnapRecomp\\mods. Output (default analysis\\modorder): overlap.json and overlap.md.
--identical also hashes the files in each overlap, to split "same bytes" from "different pictures" (reads only the shared files).
"""
import argparse
import hashlib
import json
import os
import sys
import zipfile
from collections import defaultdict
from pathlib import Path


def default_root():
    e = os.environ.get('SNAP_MODS_DIR')
    return Path(e) if e else Path(os.environ.get('APPDATA', '.')) / 'SnapRecomp' / 'mods'


def read_order(root):
    p = root / 'mods.json'
    out = []
    if p.is_file():
        for e in json.loads(p.read_text(encoding='utf-8-sig')).get('packs', []):
            if isinstance(e, str):
                out.append((e, True))
            elif isinstance(e, dict):
                i = e.get('id') or e.get('folder')
                if i:
                    out.append((i, bool(e.get('enabled', True))))
    return out


def key_of(rel):
    """(archive, entry stem, ext) as the loader keys it, or None for a file that is not a DRP entry (pack.json, readme, romfs)."""
    parts = rel.replace('\\', '/').split('/')
    low = [x.lower() for x in parts]
    if low[0] == 'romfs':
        return ('romfs', '/'.join(low[1:]), '')
    for i, part in enumerate(low):
        if part.endswith('.drp'):
            if len(parts) != i + 2:
                return None
            name = low[i + 1]
            stem, dot, ext = name.rpartition('.')
            if not dot:
                stem, ext = name, ''
            if stem.endswith('.' + ext) and ext:   # the game stores some names with their extension
                stem = stem[: -len(ext) - 1]
            return ('/'.join(low[: i + 1]), stem, ext)
    return None


def files_of(root, pid):
    """{key: (relative path, size, opener)} of one pack."""
    d = root / pid
    out = {}
    if d.is_dir():
        for f in d.rglob('*'):
            if f.is_file():
                rel = f.relative_to(d).as_posix()
                k = key_of(rel)
                if k:
                    out[k] = (rel, f.stat().st_size, f)
    elif (root / (pid + '.zip')).is_file():
        z = zipfile.ZipFile(root / (pid + '.zip'))
        names = [i for i in z.infolist() if not i.is_dir()]
        pre = ''
        for i in names:
            if i.filename.lower().endswith('pack.json'):
                pre = i.filename[: -len('pack.json')]
                break
        for i in names:
            if i.filename.startswith(pre):
                rel = i.filename[len(pre):]
                k = key_of(rel)
                if k:
                    out[k] = (rel, i.file_size, (z, i))
    return out


def digest(o):
    h = hashlib.sha1()
    if isinstance(o, Path):
        with open(o, 'rb') as f:
            for b in iter(lambda: f.read(1 << 20), b''):
                h.update(b)
    else:
        z, i = o
        with z.open(i) as f:
            for b in iter(lambda: f.read(1 << 20), b''):
                h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mods-root', default=None)
    ap.add_argument('--out', default='analysis/modorder')
    ap.add_argument('--identical', action='store_true')
    a = ap.parse_args()
    root = Path(a.mods_root) if a.mods_root else default_root()
    order = [(i, en) for i, en in read_order(root) if en and ((root / i).is_dir() or (root / (i + '.zip')).is_file())]
    packs = {}
    for pid, _ in order:
        packs[pid] = files_of(root, pid)
    ids = [i for i, _ in order]
    owners = defaultdict(list)   # key -> pack ids in priority order
    for pid in ids:
        for k in packs[pid]:
            owners[k].append(pid)
    pair = defaultdict(lambda: {'files': 0, 'same_bytes': 0, 'examples': []})
    for k, ow in owners.items():
        if len(ow) < 2:
            continue
        w = ow[0]
        for loser in ow[1:]:
            e = pair[(w, loser)]
            e['files'] += 1
            if len(e['examples']) < 5:
                e['examples'].append(packs[loser][k][0])
            if a.identical and packs[w][k][1] == packs[loser][k][1] and digest(packs[w][k][2]) == digest(packs[loser][k][2]):
                e['same_bytes'] += 1
    # a pack that wins nothing: every file it changes is beaten by a higher pack
    stat = {}
    for pid in ids:
        total = len(packs[pid])
        lost = sum(1 for k in packs[pid] if owners[k][0] != pid)
        stat[pid] = {'files': total, 'lost_to_higher_pack': lost, 'used': total - lost}
    res = {'mods_root': str(root), 'order': ids,
           'packs': stat,
           'pairs': [{'winner': w, 'loser': l, **v} for (w, l), v in sorted(pair.items(), key=lambda t: -t[1]['files'])]}
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'overlap.json').write_text(json.dumps(res, indent=1), encoding='utf-8')
    lines = ['# Pack overlap', '', 'Order (first wins): ' + ', '.join(ids), '', '| pack | files | beaten by a higher pack | used |', '|---|---|---|---|']
    for pid in ids:
        s = stat[pid]
        lines.append('| %s | %d | %d | %d |' % (pid, s['files'], s['lost_to_higher_pack'], s['used']))
    lines += ['', '| winner | loser | shared files | same bytes |', '|---|---|---|---|']
    for p in res['pairs']:
        lines.append('| %s | %s | %d | %s |' % (p['winner'], p['loser'], p['files'], p['same_bytes'] if a.identical else 'not checked'))
    if not res['pairs']:
        lines.append('| none | | 0 | |')
    (out / 'overlap.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main())
