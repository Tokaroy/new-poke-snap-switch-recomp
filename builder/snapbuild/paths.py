# SPDX-License-Identifier: GPL-3.0-or-later
"""Where an install keeps its files (release standard 2: portable by default).

One folder holds everything: the folder of the exe the player downloaded. The wizard unpacks this builder to
<root>\\setup\\app\\<version>\\builder, so the root is found from this file's own place and no environment variable or registry key is
needed. Moving the folder moves the install.

    <root>\\setup\\      unpacked builder and Python, settings.json, logs
    <root>\\tools\\      pinned build tools
    <root>\\cache\\      generated-code cache
    <root>\\game\\       game data extracted from a raw dump
    <root>\\work\\       build files, out\\ (the built program and portable_data\\), builds\\previous

A builder that runs from a source checkout or an unpacked zip has no such root: it keeps the per-user folders under %LOCALAPPDATA%\\SnapRecomp
as before. SNAP_INSTALL_ROOT names a root by hand (tests, a caller that keeps the builder elsewhere).

config.json and settings.json store absolute paths plus "install_root", the root they were written for. When the folder has been moved,
the root found now differs and rebase() moves every stored path that starts with the old root (the exe does the same for config.json:
host/portable.cpp snap_config_rebase).
"""
import os
from pathlib import Path

BUILDER_DIR = Path(__file__).resolve().parents[1]
B = chr(92)


def install_root(env=None, builder_dir=None):
    """The install's one folder, or None for a builder that is not part of an install (source checkout, unpacked zip)."""
    env = os.environ if env is None else env
    o = env.get('SNAP_INSTALL_ROOT')
    if o:
        return Path(o)
    b = Path(builder_dir) if builder_dir else BUILDER_DIR
    # <root>\setup\app\<version>\builder
    if len(b.parts) >= 5 and b.parent.parent.name.lower() == 'app' and b.parent.parent.parent.name.lower() == 'setup':
        return b.parents[3]
    # after an update the builder runs from <root>\work\updates\versions\<version>\builder (snapbuild/update.py)
    if len(b.parts) >= 6 and [x.name.lower() for x in (b.parents[1], b.parents[2], b.parents[3])] == ['versions', 'updates', 'work']:
        return b.parents[4]
    return None


def _local_base(env):
    # a builder started by a portable game sees the redirected LOCALAPPDATA; the real one is kept beside it (host/portable.cpp)
    return Path(env.get('SNAP_REAL_LOCALAPPDATA') or env.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')) / 'SnapRecomp'


def base_dir(env=None, builder_dir=None):
    """The install root, or %LOCALAPPDATA%\\SnapRecomp when there is none."""
    env = os.environ if env is None else env
    return install_root(env, builder_dir) or _local_base(env)


def setup_dir(env=None, builder_dir=None):
    env = os.environ if env is None else env
    return Path(env['SNAP_SETUP_DIR']) if env.get('SNAP_SETUP_DIR') else base_dir(env, builder_dir) / 'setup'


def cache_dir(env=None, builder_dir=None):
    return base_dir(env, builder_dir) / 'cache'


def tools_dir(env=None, builder_dir=None):
    """The build tools: SNAP_TOOLS_DIR, else <root>\\tools. An install that has none yet reuses the tools an older version left in
    %LOCALAPPDATA%\\SnapRecomp\\tools (about 8 GB) instead of downloading them again."""
    env = os.environ if env is None else env
    if env.get('SNAP_TOOLS_DIR'):
        return Path(env['SNAP_TOOLS_DIR'])
    mine = base_dir(env, builder_dir) / 'tools'
    old = _local_base(env) / 'tools'
    if not mine.exists() and old != mine and old.is_dir() and any(old.iterdir()):
        return old
    return mine


def _swap(text, old, new):
    t = text.replace('/', B)
    if t.lower() == old.lower():
        return new
    if t.lower().startswith(old.lower() + B):
        return new + t[len(old):]
    return None



def rebase(obj, old_root, new_root):
    """Move every string in obj (dicts and lists, in place) that is old_root or a path below it to new_root. True when anything changed."""
    old = os.path.normpath(str(old_root)).rstrip(B)
    new = os.path.normpath(str(new_root)).rstrip(B)
    if not old or old.lower() == new.lower():
        return False
    changed = False
    items = obj.items() if isinstance(obj, dict) else enumerate(obj) if isinstance(obj, list) else ()
    for k, v in list(items):
        if isinstance(v, str):
            r = _swap(v, old, new)
            if r is not None:
                obj[k] = r
                changed = True
        elif isinstance(v, (dict, list)):
            changed = rebase(v, old, new) or changed
    return changed
