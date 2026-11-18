"""Your saves and settings: the copy taken before every update, and portable mode.

Before an update replaces the program, `backup` copies what cannot be rebuilt (the current generation of every save, the settings files)
into <data root>\\update-backups\\<date>-<from>-to-<to>\\ and keeps the newest KEEP_BACKUPS of them. It never moves or deletes anything
in the live data. A backup that cannot be completed stops the update before it changes anything.

What is copied (everything else is rebuilt or only a cache: mods, shader caches, photo exports, screenshots, benchmark runs):
  * <data>\\save\\<user>\\CURRENT and the generation it names, and the same for <data>\\profiles\\<id>\\save
  * <data>\\profiles\\profiles.json and each profile's settings.cfg
  * <data>\\*.json, *.cfg, *.ini, *.txt at the top (audio.json, input.json, window.json ...), <data>\\config\\ and <data>\\mods\\mods.json
Each backup folder holds backup.json (what, from, to, when, sizes). A save backup is a valid save root: the game's own "Import a save" or
`SnapRecomp.exe --import-save <backup>\\save` puts it back as a new generation and keeps the current one.

Portable mode: a file `portable.txt` next to SnapRecomp.exe makes the game keep its data in `portable_data` next to the exe instead of
%APPDATA% and %LOCALAPPDATA% (host/portable.cpp does it at start). The update and the rollback carry both across the switch of out\\.
"""
import json
import os
import re
import shutil
import time
from pathlib import Path

from . import safedel
from .util import StepError

KEEP_BACKUPS = 3
BACKUP_DIR = 'update-backups'
PORTABLE_FLAG = 'portable.txt'
PORTABLE_DATA = 'portable_data'
SETTINGS_EXT = ('.json', '.cfg', '.ini', '.txt')
MAX_SETTINGS_FILE = 8 << 20          # a settings file larger than this is not a settings file
NAME_RE = re.compile(r'^\d{8}-\d{6}-[0-9.]+-to-[0-9.]+$')


class BackupError(StepError):
    """The copy could not be completed."""


# ----------------------------------------------------------------------------------------------------------- portable mode

def is_portable(exe_dir):
    return (Path(exe_dir) / PORTABLE_FLAG).is_file()


def portable_roaming(exe_dir):
    """%APPDATA% while portable. The game data root is <this>\\SnapRecomp."""
    return Path(exe_dir) / PORTABLE_DATA / 'Roaming'


def portable_local(exe_dir):
    return Path(exe_dir) / PORTABLE_DATA / 'Local'


def data_root(work=None, env=None):
    """Where the player's data is: <out>\\portable_data\\Roaming\\SnapRecomp when out\\ holds portable.txt, else %APPDATA%\\SnapRecomp."""
    env = os.environ if env is None else env
    if env.get('SNAP_DATA_DIR'):   # the game was started with --data-dir (host/portable.cpp): that folder wins, with or without portable.txt
        return Path(env['SNAP_DATA_DIR']) / 'Roaming' / 'SnapRecomp'
    if work is not None and is_portable(Path(work) / 'out'):
        return portable_roaming(Path(work) / 'out') / 'SnapRecomp'
    base = env.get('SNAP_REAL_APPDATA') or env.get('APPDATA') or str(Path.home() / 'AppData' / 'Roaming')
    return Path(base) / 'SnapRecomp'


def update_state_dir(work=None, env=None):
    """The updater's own folder. Portable: <work>\\updates, because out\\ is swapped during an update and the state must not travel with it."""
    env = os.environ if env is None else env
    if env.get('SNAP_DATA_DIR'):   # --data-dir: the launcher (host/launcher.cpp updates_dir) uses <work>\updates, the same as portable mode
        return Path(work) / 'updates' if work is not None else Path(env['SNAP_DATA_DIR']) / 'Local' / 'SnapRecomp' / 'updates'
    if work is not None and is_portable(Path(work) / 'out'):
        return Path(work) / 'updates'
    base = env.get('SNAP_REAL_LOCALAPPDATA') or env.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
    return Path(base) / 'SnapRecomp' / 'updates'


def carry_portable(src_out, dst_out):
    """Move portable.txt and portable_data from one build folder to the one that replaces it (renames on one volume, so a multi-GB save
    costs nothing). Returns the list of names moved, for carry_back. Nothing to do for a folder that is not portable."""
    src_out, dst_out = Path(src_out), Path(dst_out)
    moved = []
    if not is_portable(src_out):
        return moved
    try:
        for name in (PORTABLE_DATA, PORTABLE_FLAG):
            s, d = src_out / name, dst_out / name
            if s.exists():
                if d.exists():                       # a stale copy in the new folder must not hide the real data
                    safedel.rmtree(d, ignore_errors=True) if d.is_dir() else d.unlink()
                os.replace(s, d)
                moved.append(name)
    except OSError:
        carry_back(src_out, dst_out, moved)
        raise
    return moved


def carry_back(src_out, dst_out, moved):
    """Undo carry_portable after a failed switch."""
    for name in moved:
        try:
            os.replace(Path(dst_out) / name, Path(src_out) / name)
        except OSError:
            pass


# ----------------------------------------------------------------------------------------------------------- what to copy

def _read_current(uid_dir):
    """(generation number, CURRENT text) of a save user folder, or None. CURRENT is 'SNAPSAVE1', then the generation number."""
    try:
        text = (Path(uid_dir) / 'CURRENT').read_text(encoding='utf-8', errors='replace')
        gen = int(text.splitlines()[1].strip())
        return gen, text
    except (OSError, ValueError, IndexError):
        return None


def _save_roots(root):
    """(label, folder) of every save root: <data>\\save and <data>\\profiles\\<id>\\save."""
    out = []
    if (root / 'save').is_dir():
        out.append(('save', root / 'save'))
    prof = root / 'profiles'
    if prof.is_dir():
        for d in sorted(prof.iterdir()):
            if d.is_dir() and not d.name.startswith('_') and (d / 'save').is_dir():
                out.append((f'profiles/{d.name}/save', d / 'save'))
    return out


def _copy_file(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    return dst.stat().st_size


def _copy_tree(src, dst):
    n = 0
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames[:] = [d for d in dirnames if not safedel.is_link(os.path.join(dirpath, d))]   # never follow a junction
        for f in filenames:
            p = Path(dirpath) / f
            if f == '.snap.lock' or safedel.is_link(p):
                continue
            n += _copy_file(p, Path(dst) / p.relative_to(src))
    return n


def plan(root):
    """What would be copied: list of (relative name, source path, kind). Cheap: no data is read."""
    root = Path(root)
    items = []
    for label, sroot in _save_roots(root):
        try:
            uids = sorted(d for d in sroot.iterdir() if d.is_dir())
        except OSError:
            continue
        for u in uids:
            cur = _read_current(u)
            if not cur:
                continue
            items.append((f'{label}/{u.name}/CURRENT', u / 'CURRENT', 'file'))
            items.append((f'{label}/{u.name}/gens/{cur[0]}', u / 'gens' / str(cur[0]), 'tree'))
            ok = u / 'gens' / f'{cur[0]}.ok'
            if ok.is_file():
                items.append((f'{label}/{u.name}/gens/{cur[0]}.ok', ok, 'file'))
    pj = root / 'profiles' / 'profiles.json'
    if pj.is_file():
        items.append(('profiles/profiles.json', pj, 'file'))
    prof = root / 'profiles'
    if prof.is_dir():
        for d in sorted(prof.iterdir()):
            for n in ('settings.cfg',):
                if d.is_dir() and not d.name.startswith('_') and (d / n).is_file():
                    items.append((f'profiles/{d.name}/{n}', d / n, 'file'))
    try:
        top = sorted(root.iterdir())
    except OSError:
        top = []
    for f in top:
        if f.is_file() and f.suffix.lower() in SETTINGS_EXT and f.stat().st_size <= MAX_SETTINGS_FILE:
            items.append((f.name, f, 'file'))
    if (root / 'config').is_dir() and not safedel.is_link(root / 'config'):
        items.append(('config', root / 'config', 'tree'))
    if (root / 'mods' / 'mods.json').is_file():
        items.append(('mods/mods.json', root / 'mods' / 'mods.json', 'file'))
    return items


# ----------------------------------------------------------------------------------------------------------- backup

def plan_bytes(items):
    """Total size of what plan() lists (a walk of the folders, links skipped, nothing read)."""
    n = 0
    for _rel, src, kind in items:
        try:
            if kind == 'file':
                n += Path(src).stat().st_size
                continue
            for dirpath, dirnames, filenames in os.walk(src):
                dirnames[:] = [d for d in dirnames if not safedel.is_link(os.path.join(dirpath, d))]
                for f in filenames:
                    try:
                        n += (Path(dirpath) / f).stat().st_size
                    except OSError:
                        pass
        except OSError:
            pass
    return n


def backup_name(frm, to, now=None):
    t = time.localtime(time.time() if now is None else now)
    return f'{time.strftime("%Y%m%d-%H%M%S", t)}-{frm}-to-{to}'


def backup(root, frm, to, now=None, log=None):
    """Copy saves and settings into <root>\\update-backups\\<name>. Returns {name, path, files, bytes}, or None when there is nothing to
    copy. Raises BackupError (and leaves no half-made folder) when a copy fails; the caller stops the update."""
    root = Path(root)
    items = plan(root)
    if not items:
        if log:
            log.write('backup: no saves or settings found, nothing to copy')
        return None
    base = root / BACKUP_DIR
    need = plan_bytes(items)
    try:
        base.mkdir(parents=True, exist_ok=True)
        free = shutil.disk_usage(base).free
    except OSError:
        free = None
    if free is not None and free < need + (256 << 20):
        raise BackupError(f'there is not enough free disk space to copy your saves and settings first (needs {need / 2**30:.1f} GB, '
                          f'{free / 2**30:.1f} GB free on the drive of {root}). Nothing was changed; free some space and try again.')
    name = backup_name(frm, to, now)
    dest = base / name
    n = 1
    while dest.exists():                     # two updates in the same second (a test): never reuse a folder
        n += 1
        dest = base / f'{name}-{n}'
    tmp = base / (dest.name + '.partial')
    safedel.rmtree(tmp, ignore_errors=True)
    files, size = 0, 0
    try:
        tmp.mkdir(parents=True)
        for rel, src, kind in items:
            d = tmp / rel
            if kind == 'file':
                size += _copy_file(src, d)
                files += 1
            else:
                size += _copy_tree(src, d)
                files += 1
        meta = {'format': 1, 'from': str(frm), 'to': str(to), 'made': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(time.time() if now is None else now)),
                'files': files, 'bytes': size, 'items': [i[0] for i in items]}
        (tmp / 'backup.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
        os.replace(tmp, dest)
    except (OSError, shutil.Error) as e:
        safedel.rmtree(tmp, ignore_errors=True)
        raise BackupError(f'could not copy your saves and settings first ({e}). Nothing was changed; free some disk space or close the game and try again.')
    pruned = prune(root, KEEP_BACKUPS, keep_names=(dest.name,))
    if log:
        log.write(f'backup: {dest.name} ({files} items, {size / 2**20:.1f} MB), kept the newest {KEEP_BACKUPS}' + (f', removed {len(pruned)} older' if pruned else ''))
    return {'name': dest.name, 'path': str(dest), 'files': files, 'bytes': size}


def list_backups(root):
    """Newest first: dicts with name, path, from, to, made, bytes."""
    base = Path(root) / BACKUP_DIR
    out = []
    try:
        dirs = [d for d in base.iterdir() if d.is_dir() and not d.name.endswith('.partial')]
    except OSError:
        return out
    for d in dirs:
        try:
            m = json.loads((d / 'backup.json').read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue                         # not one of ours (or half made): never listed, never pruned
        out.append({'name': d.name, 'path': str(d), 'from': m.get('from'), 'to': m.get('to'), 'made': m.get('made'), 'bytes': m.get('bytes', 0)})
    out.sort(key=lambda e: e['name'], reverse=True)
    return out


def prune(root, keep=KEEP_BACKUPS, keep_names=()):
    """Delete all but the newest `keep` backups (and any listed in keep_names). Returns the names removed."""
    removed = []
    for i, e in enumerate(list_backups(root)):
        if i >= keep and e['name'] not in keep_names:
            safedel.rmtree(e['path'], ignore_errors=True)
            removed.append(e['name'])
    base = Path(root) / BACKUP_DIR
    if base.is_dir():
        for d in base.iterdir():
            if d.name.endswith('.partial'):
                safedel.rmtree(d, ignore_errors=True)   # left by a stopped copy
    return removed


# ----------------------------------------------------------------------------------------------------------- restore settings

def restore_settings(root, name, log=None):
    """Put the settings files of a backup back (not the saves: those go through the game's own import, which keeps the current
    generation). The files replaced are first kept as <file>.before-restore. Returns the list of files restored."""
    root = Path(root)
    src = root / BACKUP_DIR / name
    if not (src / 'backup.json').is_file():
        raise BackupError(f'there is no backup called {name}')
    done = []
    for f in sorted(src.rglob('*')):
        if not f.is_file() or f.name == 'backup.json':
            continue
        rel = f.relative_to(src).as_posix()
        if rel.startswith('save/') or re.match(r'^profiles/[^/]+/save/', rel):
            continue                         # save data
        dst = root / rel
        if dst.exists():
            shutil.copy2(dst, str(dst) + '.before-restore')
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, dst)
        done.append(rel)
    if log:
        log.write(f'restored {len(done)} settings files from {name}')
    return done


def save_roots_in_backup(root, name):
    """The save folders inside a backup, as (profile id or None, path), for `SnapRecomp.exe --import-save <path> [--profile <id>]`."""
    src = Path(root) / BACKUP_DIR / name
    out = []
    if (src / 'save').is_dir():
        out.append((None, src / 'save'))
    p = src / 'profiles'
    if p.is_dir():
        for d in sorted(p.iterdir()):
            if d.is_dir() and (d / 'save').is_dir():
                out.append((d.name, d / 'save'))
    return out
