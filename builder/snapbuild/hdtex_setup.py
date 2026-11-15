# SPDX-License-Identifier: MIT
"""Everything the HD AI textures feature downloads, on the user's PC, after the user accepts each license.

  - the Python packages (ONNX Runtime with DirectML, plus its three small dependencies) on top of the builder's pinned
    Python, in a separate folder (tools/hdtex-python), installed by pip from hash-pinned wheels with no network access;
  - texconv.exe (DirectXTex, MIT): BC decode and encode;
  - realesrgan-ncnn-vulkan and the Real-ESRGAN model files (MIT / BSD-3), only when a chosen model needs them;
  - each chosen ONNX model, from its author's release.
Pins live in builder/hdtex_pins.json. A file that fails its SHA-256 is deleted and nothing is installed.
Models are accepted one by one (--accept-license <id>, or yes at the prompt); the builder never accepts for the user.
"""
import copy
import hashlib
import json
import shutil
import time
import zipfile
from pathlib import Path

from . import fetch, pyenv


def tools_dir(base=None):
    return Path(base) if base else fetch.default_tools_dir()


def receipt_path(tdir):
    return Path(tdir) / 'hdtex' / 'licenses.json'


def accepted_before(tdir):
    p = receipt_path(tdir)
    return json.loads(p.read_text(encoding='utf-8')) if p.exists() else {}


def record_accept(tdir, key, lic):
    p = receipt_path(tdir)
    p.parent.mkdir(parents=True, exist_ok=True)
    d = accepted_before(tdir)
    d[key] = {'license': lic.get('name', ''), 'url': lic.get('url', ''), 'date': time.strftime('%Y-%m-%d')}
    p.write_text(json.dumps(d, indent=1) + '\n', encoding='utf-8')


def accept(key, title, lic, tdir, accepted, ask, say=print):
    """True when the user accepts (now or in an earlier run, recorded in tools/hdtex/licenses.json)."""
    if key in accepted_before(tdir):
        return True
    note = ' It is for personal, non-commercial use only.' if lic.get('personal_use_only') else ''
    who = f' by {lic["author"]}' if lic.get('author') else ''
    say(f'{title}{who}: license {lic.get("name", "?")}, {lic.get("url", "")}.{note}')
    # --yes only allows downloads: an auto-yes `ask` never answers a license question.
    if key in accepted or (not getattr(ask, 'auto_yes', False) and
                           ask(f'Do you accept the {lic.get("name", "")} license for {title}? It is downloaded from its author.')):
        record_accept(tdir, key, lic)
        return True
    return False


# ---- Python with ONNX Runtime --------------------------------------------------------------------------------------------

def merged_table(table, pins):
    t = copy.deepcopy(table)
    pk = t.setdefault('tools', {}).setdefault('python_packages', {})
    have = {w['name'] for w in pk.get('wheels', [])}
    pk['wheels'] = list(pk.get('wheels', [])) + [w for w in pins['python_wheels'] if w['name'] not in have]
    return t


def ensure_python(table, pins, tdir, accepted, ask, offline=(), say=print, opener=None):
    """Python of tools/hdtex-python (made on first use) that imports onnxruntime. The DirectML license is asked first."""
    tdir = Path(tdir)
    py = tdir / 'hdtex-python' / 'python.exe'
    if py.exists() and pyenv.import_ok(py, 'import onnxruntime, numpy'):
        return py
    lic = pins['directml_license']
    if not accept('directml', 'ONNX Runtime with DirectML (DirectML.dll is Microsoft\'s)', {'name': lic['title'], 'url': lic['url']}, tdir, accepted, ask, say):
        raise fetch.FetchError('the DirectML license was not accepted; the AI engine cannot be installed (texconv-only levels still need it)')
    dl = tdir / 'downloads'
    py = pyenv.build_embedded(merged_table(table, pins), tdir / 'hdtex-python', dl, dl, ask, offline, opener, say)
    if not pyenv.import_ok(py, 'import onnxruntime, numpy'):
        raise fetch.FetchError(f'{py}: onnxruntime does not import after the install')
    return py


# ---- single files and archives ---------------------------------------------------------------------------------------------

def _sha(p):
    return fetch.sha256_file(p)


def _ours(f, tdir):
    """True for a file in the builder's own download folder. A file found in an --offline-dir belongs to the user and is
    only read: never moved or deleted."""
    return Path(f).resolve().parent == (Path(tdir) / 'downloads').resolve()


def _done_with(f, tdir):
    if _ours(f, tdir):
        Path(f).unlink(missing_ok=True)


def ensure_texconv(pins, tdir, accepted, ask, offline=(), say=print, opener=None):
    pin = pins['texconv']
    dest = Path(tdir) / 'hdtex' / 'texconv.exe'
    if dest.is_file() and _sha(dest) == pin['sha256']:
        return dest
    if not accept('texconv', pin['title'], pin['license'], tdir, accepted, ask, say):
        raise fetch.FetchError('texconv is needed to read and write textures; its license was not accepted')
    f = fetch.fetch_pin('texconv', pin, Path(tdir) / 'downloads', ask, offline, opener, say)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(Path(f).read_bytes())
    return dest


def ensure_ncnn(pins, tdir, accepted, ask, offline=(), say=print, opener=None):
    """realesrgan-ncnn-vulkan folder with the pinned exe, its OpenMP runtime and the Real-ESRGAN models (only those files)."""
    n = pins['ncnn']
    root = Path(tdir) / 'hdtex' / 'ncnn'
    if all((root / f['dest']).is_file() and _sha(root / f['dest']) == f['sha256'] for f in n['files']):
        return root
    if not accept('ncnn', n['title'], n['license'], tdir, accepted, ask, say):
        raise fetch.FetchError('Real-ESRGAN (ncnn) license not accepted')
    arc = fetch.fetch_pin('ncnn', n['archive'], Path(tdir) / 'downloads', ask, offline, opener, say)
    with zipfile.ZipFile(arc) as z:
        for f in n['files']:
            data = z.read(f['member'])
            if len(data) != f['size'] or hashlib.sha256(data).hexdigest() != f['sha256']:
                raise fetch.FetchError(f'{f["member"]} does not match its pin; nothing installed')
            p = root / f['dest']
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
    _done_with(arc, tdir)
    return root


def model_path(pins, mid, tdir):
    m = pins['models'][mid]
    if m['engine'] == 'ncnn':
        return Path(tdir) / 'hdtex' / 'ncnn' / 'models' / (m['ncnn_name'] + '.bin')
    return Path(tdir) / 'hdtex' / 'models' / m['filename']


def model_sha(pins, mid):
    m = pins['models'][mid]
    if m['engine'] == 'ncnn':
        return next(f['sha256'] for f in pins['ncnn']['files'] if f['dest'].endswith(m['ncnn_name'] + '.bin'))
    return m.get('member_sha256') or m['sha256']


def ensure_model(pins, mid, tdir, accepted, ask, offline=(), say=print, opener=None):
    if mid not in pins['models']:
        raise fetch.FetchError(f'unknown model "{mid}". Known: {", ".join(pins["models"])}')
    m = pins['models'][mid]
    if m['engine'] == 'ncnn':
        ensure_ncnn(pins, tdir, accepted, ask, offline, say, opener)
        if not accept('model:' + mid, m['title'], m['license'], tdir, accepted, ask, say):
            raise fetch.FetchError(f'license for {m["title"]} not accepted')
        return model_path(pins, mid, tdir)
    dest = model_path(pins, mid, tdir)
    if dest.is_file() and _sha(dest) == model_sha(pins, mid):
        return dest
    if not accept('model:' + mid, m['title'], m['license'], tdir, accepted, ask, say):
        raise fetch.FetchError(f'license for {m["title"]} not accepted')
    dl = Path(tdir) / 'downloads'
    if 'archive' in m:
        arc = fetch.fetch_pin('model:' + mid, m['archive'], dl, ask, offline, opener, say)
        with zipfile.ZipFile(arc) as z:
            data = z.read(m['member'])
        if hashlib.sha256(data).hexdigest() != m['member_sha256']:
            raise fetch.FetchError(f'{m["member"]} does not match its pin; nothing installed')
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        _done_with(arc, tdir)
        return dest
    f = fetch.fetch_pin('model:' + mid, m, dl, ask, offline, opener, say)
    dest.parent.mkdir(parents=True, exist_ok=True)
    if _ours(f, tdir):
        Path(f).replace(dest)
    else:
        shutil.copyfile(f, dest)
    return dest
