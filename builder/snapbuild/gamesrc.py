"""The player's own game files: find them in a folder, work out which update they hold, and (for raw dumps) extract main, sdk and the
patched RomFS into the work folder.

Two kinds of folder are accepted:
  (a) already extracted: the NSO files "main" and "sdk" plus the extracted RomFS tree;
  (b) raw dump: the base game (XCI or NSP), the update NSPs and a prod.keys file.

Rules this module keeps (owner decisions):
  * nothing is ever written into the folder the player chose; extraction goes into the work folder only;
  * the keys file is read in place by NSTool: its contents are never copied, logged, printed or sent anywhere, and tool output is
    filtered before it is shown;
  * the setup never downloads, requests or points to game files, keys or firmware: a missing piece gets a plain "this folder has ..."
    message and nothing more.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

from . import containers
from .util import StepError
from . import safedel

BASE_TITLE = '0100f4300bf2c000'
PATCH_TITLE = '0100f4300bf2c800'
WANTED_VERSION = 262144          # 2.0.1; its main is also the main of 2.0.0, and the 2.0.1 romfs is the one the build checks

VERSION_NAMES = {0: '1.0.0', 65536: '1.1.0', 131072: '1.2.0', 196608: '2.0.0', 262144: '2.0.1'}

DUMP_EXT = ('.xci', '.nsp')
UNSUPPORTED_EXT = ('.xcz', '.nsz')
SCAN_DEPTH = 3

# romfs scratch estimate for the space check, in bytes (measured on 2.0.1: base program NCA 7.5 GB, patched RomFS 6.8 GB, update NCA 1.5 GB)
BASE_NCA_GB = 7.5
ROMFS_GB = 6.8
UPDATE_NCA_GB = 1.5


def version_name(v):
    if v in VERSION_NAMES:
        return VERSION_NAMES[v]
    return f'v{v}'


# ------------------------------------------------------------------------------------------------ keys

def emulator_key_candidates(env=None):
    """prod.keys files at the usual places of other programs. They are only offered: the player is asked before one is used."""
    env = os.environ if env is None else env
    out = []
    appdata = env.get('APPDATA')
    home = env.get('USERPROFILE') or str(Path.home())
    if appdata:
        a = Path(appdata)
        out += [a / 'Ryujinx' / 'system' / 'prod.keys']
        for n in ('yuzu', 'sudachi', 'suyu', 'eden', 'citron'):
            out.append(a / n / 'keys' / 'prod.keys')
    out.append(Path(home) / '.switch' / 'prod.keys')
    seen, res = set(), []
    for p in out:
        k = str(p).lower()
        if k not in seen and p.is_file():
            seen.add(k)
            res.append(p)
    return res


def find_keys_in(folder, depth=2):
    """prod.keys inside the chosen folder (not above it)."""
    folder = Path(folder)
    best = None
    stack = [(folder, 0)]
    while stack:
        d, lvl = stack.pop()
        try:
            it = list(os.scandir(d))
        except OSError:
            continue
        for e in it:
            if e.is_file() and e.name.lower() == 'prod.keys':
                return Path(e.path)
            if e.is_dir(follow_symlinks=False) and lvl < depth:
                stack.append((Path(e.path), lvl + 1))
    return best


# ------------------------------------------------------------------------------------------------ NSTool

_HEX_RUN = re.compile(r'[0-9a-fA-F]{24,}')


def scrub(text, limit=600):
    """Tool output that is safe to show or log: long hex runs (could be key material) are removed."""
    t = _HEX_RUN.sub('<hex>', text)
    t = ' '.join(t.split())
    return t[:limit]


class NsTool:
    """Runs NSTool with the keys file. Output is never returned raw to callers that log it: use scrub()."""

    def __init__(self, exe, keys, runner=None):
        self.exe = str(exe)
        self.keys = str(keys)
        self._runner = runner

    def run(self, args, timeout=3600):
        cmd = [self.exe, '-k', self.keys] + [str(a) for a in args]
        run = self._runner or subprocess.run
        try:
            r = run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout,
                    creationflags=0x00004000 if os.name == 'nt' else 0)
        except subprocess.TimeoutExpired:
            raise StepError('reading the game files took too long and was stopped.')
        except OSError as e:
            raise StepError(f'could not start the game file reader ({e}).')
        return r.returncode, (r.stdout or '') + (r.stderr or '')


def key_problem(output):
    """True when NSTool's complaint is about keys (missing, wrong, too old)."""
    o = output.lower()
    return any(w in o for w in ('key', 'keyset', 'decrypt', 'titlekey', 'rights'))


KEY_ERROR = ('The keys file does not work for these game files (a key is missing or too old for this update). '
             'The setup cannot supply keys; use a complete, recent prod.keys that goes with these files.')


def parse_cnmt_text(text):
    info = {'contents': []}
    m = re.search(r'Version:\s+\S+ \(v(\d+)\)', text)
    info['version'] = int(m.group(1)) if m else None
    m = re.search(r'TitleId:\s+0x([0-9a-f]+)', text)
    info['title_id'] = m.group(1) if m else None
    m = re.search(r'Type:\s+(\w+) \(\d+\)\n\s+Attributes', text)
    info['meta_type'] = m.group(1) if m else None
    for m in re.finditer(r'Type:\s+(\w+) \(\d+\)\s+Id:\s+([0-9a-f]{32})\s+Size:\s+(0x[0-9a-f]+)\s+Hash:\s+([0-9a-f]{64})', text):
        info['contents'].append({'type': m.group(1), 'id': m.group(2), 'size': int(m.group(3), 16), 'sha256': m.group(4)})
    return info


# ------------------------------------------------------------------------------------------------ scanning

class Dump:
    """One XCI or NSP file found in the folder."""

    def __init__(self, path):
        self.path = Path(path)
        st = self.path.stat()
        self.size = st.st_size
        self.mtime = int(st.st_mtime)
        self.kind = self.path.suffix.lower().lstrip('.')
        self.titles = []        # filled by identify()
        self.members = {}
        self.error = None

    def brief(self):
        return {'path': str(self.path), 'size': self.size, 'mtime': self.mtime, 'kind': self.kind}


class Scan:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.extracted = None       # {'main','sdk','romfs'} when kind (a) is complete
        self.partial_extracted = []  # what an extracted-style folder is missing
        self.dumps = []
        self.unsupported = []
        self.keys = None
        self.emulator_keys = []
        self.notes = []

    @property
    def kind(self):
        if self.extracted:
            return 'extracted'
        if self.dumps:
            return 'dump'
        return None

    def summary(self):
        return {'folder': str(self.folder), 'kind': self.kind,
                'extracted': {k: str(v) for k, v in self.extracted.items()} if self.extracted else None,
                'missing': self.partial_extracted,
                'dumps': [d.brief() for d in self.dumps], 'unsupported': [str(p) for p in self.unsupported],
                'keys': str(self.keys) if self.keys else None, 'emulator_keys': [str(p) for p in self.emulator_keys]}


def _is_nso(p):
    try:
        with open(p, 'rb') as f:
            return f.read(4) == b'NSO0'
    except OSError:
        return False


def _dirs(folder, depth):
    out = []
    stack = [(Path(folder), 0)]
    while stack:
        d, lvl = stack.pop()
        out.append(d)
        if lvl >= depth:
            continue
        try:
            for e in os.scandir(d):
                if e.is_dir(follow_symlinks=False):
                    stack.append((Path(e.path), lvl + 1))
        except OSError:
            pass
    return sorted(out, key=lambda p: (len(p.parts), str(p).lower()))


def _find_extracted(folder):
    """(found, missing). Looks for NSO files main and sdk and a romfs tree (a folder holding the game's own top folders)."""
    mains, romfs = [], []
    for d in _dirs(folder, SCAN_DEPTH):
        for nm in ('main', 'main.nso', 'main.uncompressed.nso'):
            if (d / nm).is_file() and _is_nso(d / nm):
                sdk = next((d / s for s in ('sdk', 'sdk.nso', 'sdk.uncompressed.nso') if (d / s).is_file() and _is_nso(d / s)), None)
                if sdk:
                    mains.append((d / nm, sdk))
                    break
        names = set()
        try:
            names = {e.name.lower() for e in os.scandir(d) if e.is_dir(follow_symlinks=False)}
        except OSError:
            pass
        if {'nx', 'common'} <= names:
            romfs.append(d)
        elif d.name.lower() == 'romfs' and names:
            romfs.append(d)
    missing = []
    if not mains:
        missing.append('the "main" and "sdk" files')
    if not romfs:
        missing.append('the extracted romfs folder')
    if mains and romfs:
        return {'main': mains[0][0], 'sdk': mains[0][1], 'romfs': romfs[0]}, []
    return None, missing


def scan(folder, env=None):
    """Look into the chosen folder. Cheap: names, sizes and a few headers; no hashing, no decryption."""
    folder = Path(folder)
    if not folder.is_dir():
        raise StepError(f'the folder "{folder}" is not there (is the drive connected?).')
    s = Scan(folder)
    ext, missing = _find_extracted(folder)
    if ext:
        s.extracted = ext
    else:
        s.partial_extracted = missing
    for d in _dirs(folder, SCAN_DEPTH - 1):
        try:
            entries = list(os.scandir(d))
        except OSError:
            continue
        for e in entries:
            if not e.is_file():
                continue
            low = e.name.lower()
            if low.endswith(DUMP_EXT):
                try:
                    s.dumps.append(Dump(e.path))
                except OSError:
                    pass
            elif low.endswith(UNSUPPORTED_EXT):
                s.unsupported.append(Path(e.path))
    s.dumps.sort(key=lambda d: d.path.name.lower())
    s.keys = find_keys_in(folder)
    s.emulator_keys = emulator_key_candidates(env)
    if not s.extracted and not s.dumps:
        if s.unsupported:
            s.notes.append('This folder has compressed files (.nsz or .xcz). The setup reads .xci and .nsp files; unpack them to .nsp or .xci first.')
        else:
            s.notes.append('No game files found here: expected the extracted files (main, sdk and the romfs folder) or .xci/.nsp files with a prod.keys file.')
    return s


# ------------------------------------------------------------------------------------------------ identify

def identify(dump, nstool, scratch, say=print):
    """Fill dump.titles from the cnmt files inside the container (small NCAs, read with the keys)."""
    scratch = Path(scratch)
    try:
        mem = containers.members(dump.path)
    except containers.ContainerError as e:
        dump.error = str(e)
        return dump
    except OSError as e:
        dump.error = f'{dump.path.name} cannot be read ({e}).'
        return dump
    dump.members = mem
    tik = next((n for n in mem if n.lower().endswith('.tik')), None)
    tik_path = None
    work = scratch / 'ident'
    work.mkdir(parents=True, exist_ok=True)
    try:
        if tik:
            off, size, _ = mem[tik]
            tik_path = containers.copy_member(dump.path, off, size, work / tik, check_free=False)
        for name in sorted(n for n in mem if n.lower().endswith('.cnmt.nca')):
            off, size, part = mem[name]
            if size > 64 * 1024 * 1024:
                continue
            nca = containers.copy_member(dump.path, off, size, work / name, check_free=False)
            part0 = work / (name + '.d')
            if part0.exists():
                safedel.rmtree(part0, ignore_errors=True)
            args = (['--tik', tik_path] if tik_path else []) + ['--part0', part0, nca]
            rc, out = nstool.run(args)
            cn = next(part0.glob('*.cnmt'), None) if part0.exists() else None
            if rc != 0 or not cn:
                dump.error = KEY_ERROR if key_problem(out) or not cn else 'this file could not be read.'
                continue
            rc, text = nstool.run(['-t', 'cnmt', cn])
            info = parse_cnmt_text(text)
            if info['title_id'] and info['version'] is not None:
                info['file'] = dump.path
                info['cnmt'] = name
                info['tik'] = tik
                dump.titles.append(info)
    finally:
        safedel.rmtree(work, ignore_errors=True)
    if dump.titles:
        dump.error = None
    return dump


def identify_all(sc, nstool, scratch, say=print):
    for d in sc.dumps:
        say(f'Reading {d.path.name} ...')
        identify(d, nstool, scratch, say)
    return sc


class Plan:
    def __init__(self, base, patch):
        self.base = base        # (Dump, cnmt info)
        self.patch = patch

    def brief(self):
        return {'base': {'file': str(self.base[0].path), 'version': self.base[1]['version']},
                'patch': {'file': str(self.patch[0].path), 'version': self.patch[1]['version']}}


def pick(sc):
    """Choose the base game and the 2.0.1 update, or raise a plain message saying what this folder has."""
    bases, patches = [], []
    for d in sc.dumps:
        for t in d.titles:
            if t['title_id'] == BASE_TITLE:
                bases.append((d, t))
            elif t['title_id'] == PATCH_TITLE:
                patches.append((d, t))
    errors = [d.error for d in sc.dumps if d.error and not d.titles]
    if not bases:
        if errors and any(e == KEY_ERROR for e in errors):
            raise StepError(KEY_ERROR)
        raise StepError('this folder has no base game file for New Pokemon Snap (an .xci or .nsp of the game itself).')
    if not patches:
        raise StepError('this folder has the base game only (version 1.0.0). The build needs the 2.0.1 update, which is a separate .nsp file.')
    best = max(patches, key=lambda x: x[1]['version'])
    match = [p for p in patches if p[1]['version'] == WANTED_VERSION]
    if not match:
        v = best[1]['version']
        if v < WANTED_VERSION:
            raise StepError(f'this folder has updates up to {version_name(v)}; the build needs the 2.0.1 update.')
        raise StepError(f'this folder has update {version_name(v)}, which is newer than the 2.0.1 this build supports.')
    base = min(bases, key=lambda x: x[1]['version'])
    return Plan(base, match[0])


# ------------------------------------------------------------------------------------------------ space

def space_estimate(plan, extracted_already=False):
    """Plain numbers for the space line (bytes): peak scratch while extracting, and what stays."""
    gb = 1 << 30
    if extracted_already:
        return {'peak': 0, 'stays': 0, 'note': ''}
    peak = int((BASE_NCA_GB + ROMFS_GB + UPDATE_NCA_GB) * gb)
    stays = int(ROMFS_GB * gb)
    note = ('The extracted game data (about %.0f GB) stays on disk because the game reads it while it runs; '
            'only the temporary copies are removed after extraction.' % ROMFS_GB)
    return {'peak': peak, 'stays': stays, 'note': note}


# ------------------------------------------------------------------------------------------------ extraction

def _prog(nca_from_cnmt):
    return next((c for c in nca_from_cnmt['contents'] if c['type'] == 'Program'), None)


def _copy_hash(dump, name, dest, progress=None):
    off, size, _ = dump.members[name]
    h = hashlib.sha256()
    done = 0
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + '.part')
    free = shutil.disk_usage(dest.parent).free
    if free < size + (512 << 20):
        raise StepError(f'not enough free space in {dest.parent} to copy {size / 2**30:.1f} GB (only {free / 2**30:.1f} GB free). '
                        'Free some space or pick another work folder.')
    try:
        with open(dump.path, 'rb') as src, open(tmp, 'wb') as out:
            src.seek(off)
            while done < size:
                b = src.read(min(containers.CHUNK, size - done))
                if not b:
                    raise StepError(f'{dump.path.name} is cut short (the copy of this file is incomplete).')
                out.write(b)
                h.update(b)
                done += len(b)
                if progress:
                    progress(done, size)
        os.replace(tmp, dest)
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass
    return h.hexdigest()


def extract(plan, nstool, game_dir, say=print, progress=None, keep_scratch=False):
    """Extract main, sdk (from the 2.0.1 update) and the patched RomFS into game_dir. Writes only inside game_dir.

    Returns {'main','sdk','romfs', 'hashes': {...}}. progress(stage, fraction) is optional.
    """
    game_dir = Path(game_dir)
    scratch = game_dir / '_scratch'
    exefs = game_dir / 'exefs'
    romfs = game_dir / 'romfs'
    romfs_tmp = game_dir / 'romfs.partial'
    done_marker = game_dir / 'extracted.json'
    hashes = {}
    base_d, base_t = plan.base
    patch_d, patch_t = plan.patch
    pprog, bprog = _prog(patch_t), _prog(base_t)
    if not pprog or not bprog:
        raise StepError('the game files have no program data (damaged or not the game).')

    def prog(stage, a, b=1.0):
        if progress:
            progress(stage, min(1.0, a / b if b else 1.0))

    game_dir.mkdir(parents=True, exist_ok=True)
    if scratch.exists():
        safedel.rmtree(scratch, ignore_errors=True)
    scratch.mkdir()
    try:
        # 1. the update's program NCA (1.5 GB) and its ticket
        say('Copying the update data into the work folder ...')
        pnca = scratch / (pprog['id'] + '.nca')
        hashes['update_program_nca'] = _copy_hash(patch_d, pprog['id'] + '.nca', pnca, lambda a, b: prog('copy_update', a, b))
        tik = None
        if patch_t.get('tik'):
            tik = scratch / patch_t['tik']
            containers.copy_member(patch_d.path, *patch_d.members[patch_t['tik']][:2], tik, check_free=False)
        targs = ['--tik', tik] if tik else []
        # 2. main and sdk
        say('Reading main and sdk ...')
        if exefs.exists():
            safedel.rmtree(exefs, ignore_errors=True)
        rc, out = nstool.run(targs + ['--part0', exefs, pnca])
        if not ((exefs / 'main').exists() and (exefs / 'sdk').exists()):
            raise StepError(KEY_ERROR if key_problem(out) else 'main and sdk could not be read from the update file (' + scrub(out, 160) + ').')
        prog('exefs', 1)
        # 3. the base game's program NCA (7.5 GB)
        say('Copying the base game data into the work folder (the large step) ...')
        bnca = scratch / (bprog['id'] + '.nca')
        hashes['base_program_nca'] = _copy_hash(base_d, bprog['id'] + '.nca', bnca, lambda a, b: prog('copy_base', a, b))
        # 4. the patched RomFS (6.8 GB)
        say('Extracting the game data (romfs) ...')
        if romfs_tmp.exists():
            safedel.rmtree(romfs_tmp, ignore_errors=True)
        rc, out = nstool.run(targs + ['--basenca', bnca, '--part1', romfs_tmp, pnca], timeout=7200)
        if not romfs_tmp.is_dir() or not any(romfs_tmp.iterdir()):
            raise StepError(KEY_ERROR if key_problem(out) else 'the game data (romfs) could not be extracted (' + scrub(out, 160) + ').')
        if romfs.exists():
            safedel.rmtree(romfs, ignore_errors=True)
        os.replace(romfs_tmp, romfs)
        prog('romfs', 1)
    finally:
        if not keep_scratch:
            safedel.rmtree(scratch, ignore_errors=True)
    info = {'main': str(exefs / 'main'), 'sdk': str(exefs / 'sdk'), 'romfs': str(romfs), 'hashes': hashes,
            'update_version': patch_t['version'], 'base_version': base_t['version']}
    done_marker.write_text(json.dumps(info, indent=1), encoding='utf-8')
    return {'main': exefs / 'main', 'sdk': exefs / 'sdk', 'romfs': romfs, 'hashes': hashes}


def extracted_already(game_dir):
    """Paths of an earlier extraction into game_dir when it is complete, else None (resume support)."""
    game_dir = Path(game_dir)
    try:
        info = json.loads((game_dir / 'extracted.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    m, s, r = Path(info.get('main', '')), Path(info.get('sdk', '')), Path(info.get('romfs', ''))
    if m.is_file() and s.is_file() and r.is_dir() and any(r.iterdir()):
        return {'main': m, 'sdk': s, 'romfs': r, 'hashes': info.get('hashes', {})}
    return None


# ------------------------------------------------------------------------------------------------ remembered choice

def settings_path(base=None):
    if not base:
        base = os.environ.get('SNAP_SETUP_DIR') or str(Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'SnapRecomp' / 'setup')
    return Path(base) / 'settings.json'


def load_settings(path=None):
    """The settings file (default: %LOCALAPPDATA%\\SnapRecomp\\setup\\settings.json); {} when missing or unreadable."""
    try:
        return json.loads((settings_path() if path is None else Path(path)).read_text(encoding='utf-8-sig'))   # utf-8-sig: a hand-edited file may carry a BOM
    except (OSError, ValueError):
        return {}


def save_settings(data, path=None):
    p = settings_path() if path is None else Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + '.tmp')
    tmp.write_text(json.dumps(data, indent=1), encoding='utf-8')
    os.replace(tmp, p)


def remember(settings, sc, plan=None, hashes=None, keys_path=None):
    """Put what the player chose and what was found into the settings dict (no key contents; only the path of the keys file)."""
    g = settings.setdefault('game', {})
    g['folder'] = str(sc.folder)
    g['kind'] = sc.kind
    g['saved_at'] = time.strftime('%Y-%m-%d %H:%M:%S')
    if sc.kind == 'extracted':
        files = {}
        for k, p in sc.extracted.items():
            p = Path(p)
            if p.is_file():
                st = p.stat()
                files[k] = {'path': str(p), 'size': st.st_size, 'mtime': int(st.st_mtime)}
            else:
                files[k] = {'path': str(p)}
        g['files'] = files
    elif plan:
        files = {}
        for role, (d, t) in (('base', plan.base), ('update', plan.patch)):
            files[role] = {'path': str(d.path), 'size': d.size, 'mtime': d.mtime, 'version': t['version']}
        g['files'] = files
        if hashes:
            g['hashes'] = hashes
    if keys_path:
        g['keys_path'] = str(keys_path)
    return settings


def still_there(settings):
    """Do the remembered files still exist unchanged (size and mtime)? Returns (ok, reason)."""
    g = settings.get('game') or {}
    if not g.get('folder') or not g.get('files'):
        return False, 'no game folder is remembered'
    for role, f in g['files'].items():
        p = Path(f['path'])
        if not p.exists():
            return False, f'{p.name} is not where it was'
        if p.is_file() and 'size' in f:
            st = p.stat()
            if st.st_size != f['size'] or int(st.st_mtime) != f['mtime']:
                return False, f'{p.name} has changed'
    keys = g.get('keys_path')
    if g.get('kind') == 'dump' and keys and not Path(keys).is_file():
        return False, 'the keys file is not where it was'
    return True, ''
