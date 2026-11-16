"""Install check: are the player's game files and the built program what this build expects?

    python builder/snap_builder.py check --install-check --main <main> --sdk <sdk> --romfs <folder> [--install-dir <folder with the exe>]

What it compares, and against what (only hashes are stored; see builder/install_hashes.json and builder/versions.json):
  - the main and sdk NSO modules: build ID and decompressed-image hash against the five known game versions (1.0.0 to 2.0.1);
  - the extracted RomFS: the 2.0.1 check files (hashed paths and sizes) and the file-list hash from versions.json;
  - the built program folder: SnapRecomp.exe (or snap_boot*.exe), the DLLs next to it and the converted game images against
    install_record.json (written by the builder when it assembled the folder), plus the known-good list for the shared DLLs.

The answer is plain words: one summary line, then one line per finding. Exit code 0 only when nothing is wrong. Nothing printed here
holds a hash, a key or any content of a game file. The C++ twin for the launcher is host/mods/install_check.cpp; it reads the same
install_hashes.json (copied into launcher_data.json) and the same install_record.json.
"""
import hashlib
import json
import os
from pathlib import Path

from . import nso as nsomod
from . import versioncheck as vc

HASHES_NAME = 'install_hashes.json'
RECORD_NAME = 'install_record.json'
EXE_NAMES = ('SnapRecomp.exe', 'snap_boot.exe', 'snap_boot_gui.exe')

OK, MISSING, WRONG_VERSION, MODIFIED, UNKNOWN = 'ok', 'missing', 'wrong_version', 'modified', 'unknown'


class Finding:
    def __init__(self, what, state, text, version=None, checked=1):
        self.what, self.state, self.text, self.version, self.checked = what, state, text, version, checked

    @property
    def ok(self):
        return self.state == OK


def load_hashes(builder_dir):
    with open(Path(builder_dir) / HASHES_NAME, encoding='utf-8') as f:
        return json.load(f)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def _short(name):
    """'2.0.1 (v262144)' -> '2.0.1'."""
    return name.split(' (')[0]


def _names(versions):
    return ' or '.join(_short(v) for v in versions)


# ------------------------------------------------------------------------------------------------ program (NSO)

def check_nso(path, role, table, hashes):
    """One NSO module -> Finding. state ok / missing / wrong_version (a known older version, named) / modified / unknown."""
    what = f'{role} program file'
    want = vc.EXPECTED_NAME
    try:
        head = nsomod.read_header(path)
    except FileNotFoundError:
        return Finding(what, MISSING, f'{what} not found at "{path}". Pass the file itself, not the folder.')
    except nsomod.NsoError as e:
        return Finding(what, MODIFIED, f'{what} is not a usable NSO file ({e.message}). It is damaged or not the right file; extract it again.')
    by_id = [v['name'] for v in table['known_versions'] if v[role + '_build_id'] == head.build_id]
    try:
        digest = nsomod.image_digest(path, head)
    except nsomod.NsoError as e:
        return Finding(what, MODIFIED, f'{what} is damaged ({e.message}). Extract it again.', version=_names(by_id) or None)
    images = hashes['program_images']
    by_image = [name for name, d in images.items() if d.get(role) == digest and name in by_id]   # build ID and contents must agree
    if by_image:
        if want in by_image:
            return Finding(what, OK, f'{what} is game version {_short(want)}.', version=_short(want))
        return Finding(what, WRONG_VERSION, f'{what} is from game version {_names(by_image)}. This build needs {_short(want)}. '
                       f'Update the game to {_short(want)} and extract it again.', version=_names(by_image))
    if by_id:
        return Finding(what, MODIFIED, f'{what} says it is game version {_names(by_id)} but its contents do not match the published file: '
                       f'it was modified or damaged. Extract it again from an unmodified dump.', version=_names(by_id))
    return Finding(what, UNKNOWN, f'{what} is not from any game version this builder knows (1.0.0 to 2.0.1). It may be another game, '
                   f'another region or a modified file. Use the file from the {_short(want)} update.')


# ------------------------------------------------------------------------------------------------ RomFS

def check_romfs(path, table):
    """The RomFS folder -> Finding, using the 2.0.1 check files and the file-list hash from versions.json."""
    what = 'RomFS (game data folder)'
    exp = table['romfs']
    p = Path(path)
    if not p.exists():
        return Finding(what, MISSING, f'{what} not found at "{path}". Pass the folder that holds the extracted RomFS.')
    if p.is_file():
        return Finding(what, UNKNOWN, f'"{path}" is a file. Pass the extracted RomFS folder.')
    root = vc.resolve_romfs_root(p)
    listing = vc.romfs_listing(root)
    if not listing:
        return Finding(what, MISSING, f'{what} at "{path}" has no files. Extract the RomFS there first.')
    by_key = {vc.path_key(r): s for r, s in listing}
    n = len(exp['sentinels'])
    missing = sum(1 for s in exp['sentinels'] if s['path_sha256'] not in by_key)
    wrong = sum(1 for s in exp['sentinels'] if s['path_sha256'] in by_key and by_key[s['path_sha256']] != s['size'])
    if missing == n:
        return Finding(what, UNKNOWN, f'"{path}" does not look like this game\'s RomFS: {len(listing)} files found and none of the {n} '
                       f'check files are in it. Pass the folder that holds the RomFS tree.', checked=len(listing))
    if missing or wrong:
        return Finding(what, WRONG_VERSION, f'{what} is not the {_short(vc.EXPECTED_NAME)} RomFS: {missing} of {n} check files are missing '
                       f'and {wrong} have the wrong size. It looks like the base game or an update before {_short(vc.EXPECTED_NAME)}. '
                       f'Apply the {_short(vc.EXPECTED_NAME)} update and extract it again.', version='older', checked=len(listing))
    if vc.listing_hash(listing) != exp['listing_sha256']:
        return Finding(what, MODIFIED, f'{what} has the {_short(vc.EXPECTED_NAME)} check files but its file list differs '
                       f'({len(listing)} files found, {exp["file_count"]} expected): files were added, removed or resized. '
                       f'Extract the patched RomFS into an empty folder.', version=_short(vc.EXPECTED_NAME), checked=len(listing))
    return Finding(what, OK, f'{what} is game version {_short(vc.EXPECTED_NAME)} ({len(listing)} files).', version=_short(vc.EXPECTED_NAME), checked=len(listing))


# ------------------------------------------------------------------------------------------------ the built folder

def find_exe(install_dir):
    for n in EXE_NAMES:
        if (Path(install_dir) / n).is_file():
            return Path(install_dir) / n
    return None


def build_record(install_dir, external=None, builder_version='', names=None):
    """The record of a freshly assembled folder: size and SHA-256 of the exe and every DLL next to it, plus files elsewhere
    (the converted game images and the NSO inputs) as name -> path. Written by the builder; trusted as the reference afterwards."""
    d = Path(install_dir)
    files = {}
    for e in sorted(d.iterdir()):
        if e.is_file() and e.suffix.lower() in ('.exe', '.dll') and (names is None or e.name in names):
            files[e.name] = {'size': e.stat().st_size, 'sha256': sha256_file(e)}
    ext = {}
    for k, p in sorted((external or {}).items()):
        p = Path(p)
        if p.is_file():
            ext[k] = {'path': str(p.resolve()), 'size': p.stat().st_size, 'sha256': sha256_file(p)}
    return {'format': 1, 'builder_version': builder_version, 'files': files, 'external': ext}


def write_record(install_dir, external=None, builder_version='', names=None):
    rec = build_record(install_dir, external, builder_version, names)
    (Path(install_dir) / RECORD_NAME).write_bytes(json.dumps(rec, indent=1).encode('utf-8'))
    return rec


def check_install_dir(install_dir, hashes):
    """The built folder -> (findings, notes). Notes are things that are not wrong but could not be checked."""
    d = Path(install_dir)
    out, notes = [], []
    what = 'built program folder'
    if not d.is_dir():
        return [Finding(what, MISSING, f'{what} not found at "{install_dir}". Pass the folder that holds SnapRecomp.exe.')], notes
    exe = find_exe(d)
    if not exe:
        out.append(Finding('game program', MISSING, f'No game program (SnapRecomp.exe) in "{install_dir}". Run the setup or the builder again.'))
    known = {k.lower(): v for k, v in hashes.get('runtime_files', {}).items() if isinstance(v, list)}
    rec = None
    rp = d / RECORD_NAME
    if rp.is_file():
        try:
            rec = json.loads(rp.read_text(encoding='utf-8'))
            if not isinstance(rec.get('files'), dict):
                raise ValueError('no files list')
        except ValueError:
            out.append(Finding('install record', MODIFIED, f'{RECORD_NAME} in "{install_dir}" cannot be read. Run the builder again to rewrite it.'))
            rec = None
    else:
        notes.append(f'{RECORD_NAME} is not next to the program (the build is older than this check), so the program file itself and the '
                     f'converted game images could not be compared. Run the builder again, or add --write-record to record what is there now.')
    checked = 0
    flagged = set()
    if rec:
        for name, e in sorted(rec['files'].items()):
            p = d / name
            checked += 1
            if not p.is_file():
                out.append(Finding(name, MISSING, f'{name} is missing from "{install_dir}". Run the builder again to restore it.'))
            elif p.stat().st_size != e.get('size') or sha256_file(p) != e.get('sha256'):
                flagged.add(name.lower())
                out.append(Finding(name, MODIFIED, f'{name} was changed after the build (size or contents differ from the build record). '
                                   f'Run the builder again to restore it.'))
        for key, e in sorted(rec.get('external', {}).items()):
            p = Path(e.get('path', ''))
            checked += 1
            label = {'main_elf': 'converted main game image', 'sdk_elf': 'converted sdk game image',
                     'main_nso': 'main program file used for the build', 'sdk_nso': 'sdk program file used for the build'}.get(key, key)
            if not p.is_file():
                out.append(Finding(label, MISSING, f'The {label} is missing ("{p.name}" in "{p.parent}"). Run the builder again.'))
            elif p.stat().st_size != e.get('size') or sha256_file(p) != e.get('sha256'):
                out.append(Finding(label, MODIFIED, f'The {label} ("{p.name}") changed after the build. Run the builder again.'))
    for dll in sorted(d.glob('*.dll')):
        want = known.get(dll.name.lower())
        if want is not None and dll.name.lower() not in flagged:
            checked += 1
            if sha256_file(dll) not in want:
                out.append(Finding(dll.name, MODIFIED, f'{dll.name} is not the file that ships with this build (it is modified, damaged or a different version). '
                                   f'Run the builder again to restore it.'))
    if not out:
        out.append(Finding(what, OK, f'{what}: {checked} files match.' if checked else f'{what}: nothing recorded to compare.', checked=checked))
    return out, notes


# ------------------------------------------------------------------------------------------------ all of it

class Report:
    def __init__(self):
        self.findings = []
        self.notes = []
        self.mixed = None   # sentence when the files come from two game versions

    @property
    def problems(self):
        return [f for f in self.findings if not f.ok]

    @property
    def ok(self):
        return bool(self.findings) and not self.problems and not self.mixed

    def lines(self):
        pr = self.problems
        if self.ok:
            head = f'Install check: everything matches ({len(self.findings)} parts checked, game version {_short(vc.EXPECTED_NAME)}).'
        elif not self.findings:
            head = 'Install check: nothing was checked.'
        elif self.mixed:
            head = f'Install check: {self.mixed}'
        else:
            kinds = {WRONG_VERSION: 'wrong game version', MODIFIED: 'modified or damaged', MISSING: 'missing', UNKNOWN: 'not recognized'}
            head = f'Install check: {len(pr)} problem{"s" if len(pr) != 1 else ""} found (' + ', '.join(sorted({kinds[f.state] for f in pr})) + ').'
        out = [head.strip()]
        for f in self.findings:
            out.append(('  ok:      ' if f.ok else '  PROBLEM: ') + f.text)
        for n in self.notes:
            out.append('  note:    ' + n)
        return out


def mixed_update(findings):
    """A sentence when the program files and the RomFS come from two different game versions, else None."""
    main = next((f for f in findings if f.what.startswith('main')), None)
    rom = next((f for f in findings if f.what.startswith('RomFS')), None)
    if not main or not rom:
        return None
    new = _short(vc.EXPECTED_NAME)
    if main.state == WRONG_VERSION and rom.state in (OK, MODIFIED) and rom.version == new:
        return (f'mixed update: the main program file is from game version {main.version} but the RomFS is {new}. '
                f'Update the game to {new} and extract the main file again.')
    if main.state == OK and rom.state == WRONG_VERSION:
        return (f'mixed update: the main program file is {new} but the RomFS is from an older version (the base game or an earlier update). '
                f'Apply the {new} update and extract the RomFS again.')
    return None


def run(table, hashes, main=None, sdk=None, romfs=None, install_dir=None):
    r = Report()
    if main:
        r.findings.append(check_nso(main, 'main', table, hashes))
    if sdk:
        r.findings.append(check_nso(sdk, 'sdk', table, hashes))
    if romfs:
        r.findings.append(check_romfs(romfs, table))
    if install_dir:
        f, n = check_install_dir(install_dir, hashes)
        r.findings += f
        r.notes += n
    r.mixed = mixed_update(r.findings)
    if not r.findings:
        r.notes.append('Nothing was given to check. Pass --main, --sdk, --romfs and/or --install-dir.')
    return r
