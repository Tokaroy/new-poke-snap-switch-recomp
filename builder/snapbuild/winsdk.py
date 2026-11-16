"""The Microsoft C runtime and Windows SDK files the final link (and the C compile) need, without Visual Studio.

`xwin` (MIT/Apache-2.0, pinned in versions.json) downloads them from Microsoft's own servers. That only happens on the user PC, after
the user accepted Microsoft's license (the setup wizard shows it; the command line asks). Nothing from Microsoft is ever shipped by us.

The result is a folder in the managed tools folder:
    winsdk/crt/include, crt/lib/x86_64, sdk/include/{ucrt,um,shared}, sdk/lib/{ucrt,um}/x86_64   and   winsdk.json (the receipt).
`env_for` turns it into INCLUDE and LIB, which clang and lld-link read without any Visual Studio install.
"""
import json
import os
import subprocess
import time
from pathlib import Path

from .util import StepError
from . import safedel

LICENSE_URL = 'https://go.microsoft.com/fwlink/?LinkId=2086102'   # the license xwin asks about (Visual Studio / Windows SDK terms)
RECEIPT = 'winsdk.json'

_INCLUDE = ('crt/include', 'sdk/include/ucrt', 'sdk/include/um', 'sdk/include/shared')
_LIB = ('crt/lib/x86_64', 'sdk/lib/ucrt/x86_64', 'sdk/lib/um/x86_64')


def sdk_root(tools_dir):
    return Path(tools_dir) / 'winsdk'


def find(tools_dir=None):
    """The xwin-made SDK folder, or None."""
    from .fetch import default_tools_dir
    r = sdk_root(tools_dir or default_tools_dir())
    ok = (r / RECEIPT).exists() and all((r / d).is_dir() for d in _INCLUDE + _LIB) and (r / 'sdk/lib/um/x86_64/kernel32.lib').exists()
    return r if ok else None


def env_for(root):
    """INCLUDE and LIB for clang and lld-link (the existing PATH and the rest of the environment stay as they are)."""
    root = Path(root)
    env = dict(os.environ)
    env['INCLUDE'] = os.pathsep.join(str(root / d) for d in _INCLUDE)
    env['LIB'] = os.pathsep.join(str(root / d) for d in _LIB)
    return env


def receipt(root):
    try:
        return json.loads((Path(root) / RECEIPT).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def install(xwin_exe, tools_dir, accepted, say=print, cache_dir=None, timeout=3600, runner=None):
    """Download and lay out the SDK. `accepted` must be True: it is the user's answer to Microsoft's license, never assumed.

    Returns the folder. Raises StepError with a plain message on any failure (offline, blocked, disk full)."""
    if not accepted:
        raise StepError("Microsoft's license for the C runtime and Windows SDK files was not accepted, so they were not downloaded. "
                        f'The license is at {LICENSE_URL}. Accept it to continue, or install Visual Studio Build Tools instead.')
    tools_dir = Path(tools_dir)
    root = sdk_root(tools_dir)
    cache = Path(cache_dir) if cache_dir else tools_dir / 'winsdk_download'
    tmp = tools_dir / 'winsdk.partial'
    if tmp.exists():
        _rmtree(tmp)
    tmp.mkdir(parents=True)
    cache.mkdir(parents=True, exist_ok=True)
    cmd = [str(xwin_exe), '--accept-license', '--cache-dir', str(cache), '--arch', 'x86_64', '--variant', 'desktop',
           'splat', '--output', str(tmp)]
    say('Downloading the Microsoft C runtime and Windows SDK files (about 600 MB, from Microsoft)...')
    t0 = time.time()
    try:
        r = (runner or subprocess.run)(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired:
        raise StepError('the Microsoft SDK download took too long and was stopped. Check the internet connection and try again.')
    except OSError as e:
        raise StepError(f'could not start the SDK downloader ({e}).')
    if r.returncode != 0:
        tail = ' '.join((r.stdout + r.stderr).strip().splitlines()[-4:])
        raise StepError('the Microsoft SDK download failed. Check the internet connection (the files come from Microsoft) and try again. '
                        f'Details: {tail[:400]}')
    missing = [d for d in _INCLUDE + _LIB if not (tmp / d).is_dir()]
    if missing or not (tmp / 'sdk/lib/um/x86_64/kernel32.lib').exists():
        raise StepError('the Microsoft SDK download finished but the folder is incomplete (' + ', '.join(missing[:3]) + '). Try again.')
    (tmp / RECEIPT).write_text(json.dumps({'tool': 'xwin', 'accepted_license': LICENSE_URL, 'accepted_at': time.strftime('%Y-%m-%d %H:%M:%S'),
                                           'seconds': round(time.time() - t0, 1)}, indent=1), encoding='utf-8')
    if root.exists():
        _rmtree(root)
    os.replace(tmp, root)
    # the downloaded packages are only needed to make the folder; keep nothing (the files are checked by xwin against Microsoft's manifest)
    _rmtree(cache)
    say(f'Microsoft SDK files ready in {root}')
    return root


def _rmtree(p):
    import shutil
    safedel.rmtree(p, ignore_errors=True)


def vs_available():
    """True when Visual Studio Build Tools with the C++ tools are installed (the older way; still used when present)."""
    from .toolchain import find_vcvars
    try:
        find_vcvars()
        return True
    except StepError:
        return False
