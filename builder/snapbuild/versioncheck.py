"""Game-version check: the user's main NSO, sdk NSO and romfs must be the 2.0.1 (v262144) files.

Expected values live in builder/versions.json (build IDs, decompressed-image SHA-256 values, hashed romfs sentinels, one
listing hash). Messages say what was found, what is expected and what to do next. A hash of a user file is never printed.
"""
import hashlib
import json
import os
from pathlib import Path

from . import nso as nsomod

EXPECTED_NAME = '2.0.1 (v262144)'


def load_table(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def path_key(rel):
    """Hashed form of a romfs-relative path (lowercase, forward slashes); the tables never hold game file names."""
    return hashlib.sha256(rel.replace(chr(92), '/').lower().encode('utf-8')).hexdigest()


# ------------------------------------------------------------------------------------------------ NSO

def check_nso(path, role, table):
    """Check one NSO. Returns (problems, matched_versions): problems is a list of message strings (empty = good);
    matched_versions names every known game version whose build ID this file carries."""
    label = f'{role} file'
    exp = table['nso'][role]
    try:
        head = nsomod.read_header(path)
    except FileNotFoundError:
        return [f'{label} not found at "{path}": check the path (pass the file, not the folder).'], []
    except nsomod.NsoError as e:
        return [f'{label}: {e.message}.'], []
    matched = [v['name'] for v in table['known_versions'] if v[role + '_build_id'] == head.build_id]
    if head.build_id != exp['build_id']:
        old = [v for v in table['known_versions'] if v[role + '_build_id'] == head.build_id and v['name'] != EXPECTED_NAME]
        if old:
            names = ' or '.join(v['name'] for v in old)
            return [f'{label} is from game version {names}. Expected {EXPECTED_NAME}. Update the game to 2.0.1 and extract again.'], matched
        return [f'{label} is not from game version {EXPECTED_NAME}: its build ID is not one this builder knows. '
                f'Use the "{role}" file from the 2.0.1 update.'], matched
    try:
        digest = nsomod.image_digest(path, head)
    except nsomod.NsoError as e:
        return [f'{label}: {e.message}.'], matched
    if digest != exp['image_sha256']:
        return [f'{label} has the 2.0.1 build ID but its contents do not match the published file: it is modified or damaged. '
                f'Extract it again from an unmodified dump.'], matched
    return [], matched


# ------------------------------------------------------------------------------------------------ romfs

def resolve_romfs_root(path):
    """The folder that directly holds the romfs tree: path itself, or path/romfs when the dump tool made that wrapper."""
    p = Path(path)
    if p.is_dir() and (p / 'romfs').is_dir() and not any(c.is_file() for c in p.iterdir()):
        sub = [c for c in (p / 'romfs').iterdir()]
        if sub:
            return p / 'romfs'
    return p


def romfs_listing(root):
    """Sorted [(relative lowercase path with '/', size)] of every file under root (stat only, no content reads)."""
    out = []
    stack = [(str(root), '')]
    while stack:
        d, rel = stack.pop()
        with os.scandir(d) as it:
            for e in it:
                r = rel + e.name.lower()
                if e.is_dir(follow_symlinks=False):
                    stack.append((e.path, r + '/'))
                elif e.is_file(follow_symlinks=False):
                    out.append((r, e.stat().st_size))
    out.sort()
    return out


def listing_hash(listing):
    h = hashlib.sha256()
    for rel, size in listing:
        h.update(f'{rel}\t{size}\n'.encode('utf-8'))
    return h.hexdigest()


def check_romfs(path, table, main_is_2_0_x=False):
    """Check the romfs folder. Returns (problems, root)."""
    exp = table['romfs']
    p = Path(path)
    if not p.exists():
        return [f'romfs folder not found at "{path}": pass the folder that holds the extracted romfs.'], p
    if p.is_file():
        return [f'romfs path "{path}" is a file: pass the extracted romfs folder.'], p
    root = resolve_romfs_root(p)
    listing = romfs_listing(root)
    if not listing:
        return [f'romfs folder "{path}" has no files in it: extract the romfs there first.'], root
    by_key = {path_key(r): s for r, s in listing}
    missing = sum(1 for s in exp['sentinels'] if s['path_sha256'] not in by_key)
    wrong = sum(1 for s in exp['sentinels'] if s['path_sha256'] in by_key and by_key[s['path_sha256']] != s['size'])
    n = len(exp['sentinels'])
    hint = ' The main file is the same in 2.0.0 and 2.0.1, so a 2.0.0 game gives this result: apply the 2.0.1 update to the romfs.' if main_is_2_0_x else ''
    if missing == n:
        return [f'this folder does not look like the extracted romfs of this game ({len(listing)} files found, none of the {n} check files present). '
                f'Pass the folder that holds the romfs tree.'], root
    if missing or wrong:
        return [f'romfs is not the 2.0.1 romfs: {missing} of {n} check files are missing and {wrong} have the wrong size '
                f'({len(listing)} files found, {exp["file_count"]} expected). It looks like the base game or an older update: '
                f'apply the 2.0.1 update to the romfs and extract it again.{hint}'], root
    if listing_hash(listing) != exp['listing_sha256']:
        return [f'romfs has the 2.0.1 check files but its file list differs from 2.0.1 ({len(listing)} files found, {exp["file_count"]} expected): '
                f'extra, missing or resized files. Extract the patched romfs into an empty folder.'], root
    return [], root


# ------------------------------------------------------------------------------------------------ all three

class Result:
    def __init__(self):
        self.problems = []
        self.notes = []
        self.romfs_root = None

    @property
    def ok(self):
        return not self.problems


def check_all(main_path, sdk_path, romfs_path, table):
    r = Result()
    pm, vm = check_nso(main_path, 'main', table)
    ps, _vs = check_nso(sdk_path, 'sdk', table)
    r.problems += pm + ps
    main_2_0 = not pm and any(v.startswith('2.0.0') for v in vm)
    pr, r.romfs_root = check_romfs(romfs_path, table, main_2_0)
    r.problems += pr
    if not pm and any(v.startswith('2.0.0') for v in vm):
        r.notes.append('main matches both 2.0.0 and 2.0.1 (the file is identical).')
    return r
