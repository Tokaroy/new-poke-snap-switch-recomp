"""The Python the lifter needs (capstone, pyelftools, numpy, lz4), built from the pinned wheels in versions.json.

Two ways, same result:
  - a venv of a Python 3.14 that is already installed (`python -m venv`), or
  - the pinned python.org embeddable Python, unpacked into the managed tools folder (no installer, no admin rights).
Either way the wheels are fetched and SHA-256 checked by snapbuild.fetch first, and pip installs them with --require-hashes,
--no-deps, --only-binary=:all: and --no-index, so pip never reaches the network and nothing outside the pinned list is installed.
"""
import re
import subprocess
import sys
from pathlib import Path

from . import fetch

IMPORT_CHECK = 'import capstone, elftools, numpy, lz4.block'


def wheels(table):
    return list(table.get('tools', {}).get('python_packages', {}).get('wheels', []))


def requirements_text(wheel_list):
    """requirements.txt lines in pip's hash-checking mode (the installer wheel, pip itself, is not part of it)."""
    lines = []
    for w in wheel_list:
        if w.get('installer'):
            continue
        lines.append(f'{w["name"]}=={w["version"]} --hash=sha256:{w["sha256"].lower()}')
    return '\n'.join(lines) + '\n'


def py_version(python):
    r = subprocess.run([str(python), '-I', '-c', 'import sys;print("%d.%d.%d" % sys.version_info[:3])'], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def wanted_python_minor(table):
    """'3.14' from the pinned embeddable Python (the wheels are built for that version)."""
    v = table.get('tools', {}).get('python_embed', {}).get('version', '')
    m = re.match(r'(\d+\.\d+)', v)
    return m.group(1) if m else None


def import_ok(python, check=IMPORT_CHECK):
    return subprocess.run([str(python), '-I', '-c', check], capture_output=True).returncode == 0


def run_pip(cmd, say):
    say('pip: ' + ' '.join(str(c) for c in cmd[1:6]) + ' ...')
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode != 0:
        raise fetch.FetchError('pip failed:\n' + (r.stdout + r.stderr)[-2000:])
    return r.stdout


def install_wheels(python, wheel_files, req_file, pip_cmd_prefix, say):
    """pip install from local wheels only, every hash required."""
    wheel_dir = str(Path(wheel_files[0]).parent)
    cmd = list(pip_cmd_prefix) + ['install', '--require-hashes', '--no-deps', '--only-binary=:all:', '--no-index', '--no-cache-dir',
                                 '--disable-pip-version-check', '--find-links', wheel_dir, '-r', str(req_file)]
    run_pip(cmd, say)


def fetch_wheels(table, wheel_dir, ask, offline_dirs=(), opener=None, say=print):
    out = []
    for w in wheels(table):
        out.append(fetch.fetch_pin('wheel:' + w['name'], w, wheel_dir, ask, offline_dirs, opener, say))
    return out


def build_with_venv(table, base_python, env_dir, wheel_dir, ask, offline_dirs=(), opener=None, say=print, check=IMPORT_CHECK):
    """venv of an installed Python 3.14 plus the pinned wheels. Returns the venv's python.exe."""
    want = wanted_python_minor(table)
    ver = py_version(base_python)
    if ver is None:
        raise fetch.FetchError(f'{base_python} does not run')
    if want and not ver.startswith(want + '.'):
        raise fetch.FetchError(f'the pinned wheels are built for Python {want}; {base_python} is {ver}. Use the managed Python instead (no --base-python).')
    files = fetch_wheels(table, wheel_dir, ask, offline_dirs, opener, say)
    env_dir = Path(env_dir)
    r = subprocess.run([str(base_python), '-I', '-m', 'venv', str(env_dir)], capture_output=True, text=True)
    if r.returncode != 0:
        raise fetch.FetchError('could not create the venv:\n' + (r.stdout + r.stderr)[-1500:])
    py = env_dir / 'Scripts' / 'python.exe'
    req = env_dir / 'requirements.pinned.txt'
    req.write_text(requirements_text(wheels(table)), encoding='ascii')
    install_wheels(py, files, req, [py, '-m', 'pip'], say)
    if not import_ok(py, check):
        raise fetch.FetchError(f'{py} installed the wheels but {check!r} fails')
    return py


def build_embedded(table, env_dir, wheel_dir, download_dir, ask, offline_dirs=(), opener=None, say=print):
    """The pinned embeddable Python unpacked into env_dir, with the pinned wheels installed. Returns its python.exe."""
    emb = table.get('tools', {}).get('python_embed')
    arc = fetch.fetch_pin('python_embed', emb, download_dir, ask, offline_dirs, opener, say)
    env_dir = Path(env_dir)
    fetch.extract(arc, env_dir)
    for pth in env_dir.glob('python*._pth'):   # the embeddable Python ignores site-packages until this is turned on
        text = pth.read_text(encoding='ascii')
        lines = [l for l in text.splitlines() if l.strip() and not l.strip().lstrip('#').strip() == 'import site']
        lines += ['Lib\\site-packages', 'import site']
        pth.write_text('\n'.join(lines) + '\n', encoding='ascii')
    (env_dir / 'Lib' / 'site-packages').mkdir(parents=True, exist_ok=True)
    files = fetch_wheels(table, wheel_dir, ask, offline_dirs, opener, say)
    installer = next((f for f, w in zip(files, wheels(table)) if w.get('installer')), None)
    if installer is None:
        raise fetch.FetchError('versions.json has no pip installer wheel (installer: true)')
    py = env_dir / 'python.exe'
    req = env_dir / 'requirements.pinned.txt'
    req.write_text(requirements_text(wheels(table)), encoding='ascii')
    install_wheels(py, files, req, [py, '-s', str(installer) + '/pip'], say)   # pip runs straight from its own wheel
    if not import_ok(py):
        raise fetch.FetchError(f'{py} installed the wheels but {IMPORT_CHECK!r} fails')
    return py


def main_python_for(table, base_python=None, managed_dir=None):
    """Not used by the CLI directly; kept for the pipeline: the managed Python when it is already built, else None."""
    if managed_dir:
        for cand in (Path(managed_dir) / 'venv' / 'Scripts' / 'python.exe', Path(managed_dir) / 'python' / 'python.exe'):
            if cand.exists() and import_ok(cand):
                return cand
    return None


if __name__ == '__main__':
    print(__doc__)
    sys.exit(0)
