"""Private generation cache. Keeps the function lists and generated C under %LOCALAPPDATA%\\SnapRecomp\\cache so a rebuild after
a runtime update skips Ghidra and snap_gen. Never in a release zip, never next to the exe. Objects are always recompiled.

Safety: eviction only happens inside a folder that holds our marker file; reparse points (junctions, symlinks) are refused
when copying and when deleting; two snapshots per kind and a total size cap."""
import json
import os
import shutil
import stat
import time
from pathlib import Path
from . import safedel

MARKER = 'SNAPRECOMP_CACHE_ROOT'
KEEP = 2
CAP_BYTES = 1 << 30


class CacheError(Exception):
    pass


def default_root():
    from . import paths   # <install root>\cache for an install (the exe's folder), the per-user folder for a source checkout
    return paths.cache_dir()


def _is_reparse(p):
    try:
        st = os.lstat(p)
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode):
        return True
    return bool(getattr(st, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400))


def _tree_has_reparse(folder):
    for dp, dns, fns in os.walk(folder):
        for n in dns + fns:
            if _is_reparse(os.path.join(dp, n)):
                return True
    return False


def _size(folder):
    return sum(f.stat().st_size for f in Path(folder).rglob('*') if f.is_file())


def open_root(root=None):
    root = Path(root) if root else default_root()
    if _is_reparse(root):
        raise CacheError(f'cache root {root} is a link; refusing to use it')
    if root.exists() and any(root.iterdir()) and not (root / MARKER).exists():
        raise CacheError(f'{root} is not empty and has no {MARKER} marker; refusing to use it')
    root.mkdir(parents=True, exist_ok=True)
    (root / MARKER).write_text('Snap Recomp builder cache. Safe to delete.\n', encoding='utf-8')
    return root


def _entry(root, kind, key):
    return Path(root) / kind / key


def has(root, kind, key):
    return (_entry(root, kind, key) / '.complete').exists()


def store(root, kind, key, src):
    """Copy the folder src into the cache as kind/key, then evict old snapshots. Returns the entry path."""
    root = open_root(root)
    src = Path(src)
    if _tree_has_reparse(src):
        raise CacheError(f'{src} contains a link; not caching it')
    e = _entry(root, kind, key)
    if e.exists():
        _rmtree(root, e)
    tmp = e.with_name(key + '.tmp')
    if tmp.exists():
        _rmtree(root, tmp)
    tmp.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, tmp)
    (tmp / '.complete').write_text(json.dumps({'key': key, 'time': time.time()}), encoding='utf-8')
    tmp.rename(e)
    evict(root, kind, keep_key=key)
    return e


def restore(root, kind, key, dst):
    """Copy a cached entry to dst (replacing it). Returns False when there is no complete entry."""
    root = Path(root)
    e = _entry(root, kind, key)
    if not (root / MARKER).exists() or not has(root, kind, key) or _tree_has_reparse(e):
        return False
    dst = Path(dst)
    if dst.exists():
        safedel.rmtree(dst)
    shutil.copytree(e, dst)
    (dst / '.complete').unlink(missing_ok=True)
    os.utime(e / '.complete', None)  # most recently used
    return True


def _rmtree(root, folder):
    root = Path(root).resolve()
    folder = Path(folder)
    if not (root / MARKER).exists():
        raise CacheError('refusing to delete outside a marked cache root')
    if _is_reparse(folder) or root not in folder.resolve().parents:
        raise CacheError(f'refusing to delete {folder}')
    safedel.rmtree(folder)


def evict(root, kind, keep_key=None, keep=KEEP, cap=CAP_BYTES):
    """Keep the newest `keep` complete snapshots of this kind and stay under `cap` bytes in total."""
    root = Path(root)
    kd = root / kind
    if not kd.exists():
        return []
    removed = []
    ents = sorted((d for d in kd.iterdir() if d.is_dir() and (d / '.complete').exists()),
                  key=lambda d: (d / '.complete').stat().st_mtime, reverse=True)
    for d in ents[keep:]:
        if d.name != keep_key:
            _rmtree(root, d)
            removed.append(d.name)
    while _size(root) > cap:
        olds = [d for k in root.iterdir() if k.is_dir() for d in k.iterdir()
                if d.is_dir() and (d / '.complete').exists() and d.name != keep_key]
        if not olds:
            break
        olds.sort(key=lambda d: (d / '.complete').stat().st_mtime)
        _rmtree(root, olds[0])
        removed.append(olds[0].name)
    return removed
