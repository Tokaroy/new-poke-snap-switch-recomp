"""Fetch and verify the pinned tools (versions.json "tools"): ask, download, check the SHA-256, unpack.

Rules:
  - Nothing is downloaded without asking first (`ask` callback; --yes answers yes for every tool). A download is never started
    without a pinned https URL and a pinned SHA-256.
  - The hash is computed while the file streams in. A file that does not match is deleted and the run stops.
  - A file that is already in the download folder, or in one of the offline folders (--offline-dir), is used when its hash matches,
    without asking and without network.
  - Archives are unpacked only after the hash matched, and never outside the target folder.
"""
import hashlib
import os
import re
import shutil
import tarfile
import urllib.request
import zipfile
from pathlib import Path

HEX64 = re.compile(r'^[0-9a-f]{64}$')
CHUNK = 1 << 20


class FetchError(Exception):
    pass


def sha256_file(path, progress=None):
    h = hashlib.sha256()
    total = 0
    with open(path, 'rb') as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
            total += len(b)
            if progress:
                progress(total)
    return h.hexdigest()


def is_pinned(pin):
    """True when the pin can be downloaded: an https URL and a 64-hex SHA-256 (not a TODO)."""
    return bool(pin and str(pin.get('url', '')).startswith('https://') and HEX64.match(str(pin.get('sha256', '')).lower()))


def default_tools_dir():
    base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
    return Path(base) / 'SnapRecomp' / 'tools'


def default_download_dir():
    return default_tools_dir() / 'downloads'


def list_pins(table):
    """Every downloadable pin in the version table as (key, pin). Wheels come out as 'wheel:<name>'."""
    out = []
    tools = table.get('tools', {})
    for key, pin in tools.items():
        if key == 'python_packages':
            for w in pin.get('wheels', []):
                out.append(('wheel:' + w['name'], w))
        elif isinstance(pin, dict) and 'url' in pin:
            out.append((key, pin))
    return out


def unpinned(table):
    """Pins that cannot be downloaded yet (missing or TODO url or hash): the list for the person who maintains versions.json."""
    return [k for k, p in list_pins(table) if not is_pinned(p)]


def find_local(pin, folders):
    """A file named like the pin, in one of the folders, whose hash matches. Returns its path or None."""
    want = pin['sha256'].lower()
    for d in folders:
        if not d:
            continue
        cand = Path(d) / pin['filename']
        if cand.is_file() and sha256_file(cand) == want:
            return cand
    return None


def download(url, dest, want_sha, opener=None, progress=None, size_hint=None):
    """Stream url to dest (via dest.part), hashing as it goes. Deletes the partial file and raises FetchError on any mismatch."""
    if not url.startswith('https://'):
        raise FetchError(f'refusing a non-https download URL: {url}')
    opener = opener or urllib.request.urlopen
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + '.part')
    h = hashlib.sha256()
    n = 0
    try:
        with opener(url) as r, open(part, 'wb') as f:
            while True:
                b = r.read(CHUNK)
                if not b:
                    break
                f.write(b)
                h.update(b)
                n += len(b)
                if progress:
                    progress(n)
    except FetchError:
        raise
    except Exception as e:
        part.unlink(missing_ok=True)
        raise FetchError(f'download failed: {url}: {e}') from e
    if size_hint and n != size_hint:
        part.unlink(missing_ok=True)
        raise FetchError(f'{dest.name}: got {n} bytes, expected {size_hint}. The partial file was deleted.')
    if h.hexdigest() != want_sha.lower():
        part.unlink(missing_ok=True)
        raise FetchError(f'{dest.name}: SHA-256 mismatch. Expected {want_sha.lower()}, got {h.hexdigest()}. The file was deleted; nothing was installed.')
    os.replace(part, dest)
    return dest


def fetch_pin(key, pin, download_dir, ask, offline_dirs=(), opener=None, say=print):
    """Return a verified local copy of the pinned file, downloading it only after `ask` says yes.

    Raises FetchError when the pin is incomplete, the user declines, or the hash does not match."""
    if not is_pinned(pin):
        raise FetchError(f'{key}: no pinned URL and SHA-256 in versions.json yet')
    have = find_local(pin, [download_dir] + list(offline_dirs))
    if have:
        say(f'{key}: using {have} (SHA-256 checked)')
        return have
    mb = f' ({pin["size"] / 2**20:.0f} MB)' if pin.get('size') else ''
    if not ask(f'Download {key}{mb} from {pin["url"]} ?'):
        raise FetchError(f'{key}: download declined. Put {pin["filename"]} in {download_dir} (or use --offline-dir) and run again.')
    say(f'{key}: downloading {pin["filename"]}{mb}')
    last = [0]

    def prog(n):
        if n - last[0] >= 64 * 2**20:
            last[0] = n
            say(f'{key}: {n / 2**20:.0f} MB')
    dest = download(pin['url'], Path(download_dir) / pin['filename'], pin['sha256'], opener=opener, progress=prog, size_hint=pin.get('size'))
    say(f'{key}: SHA-256 verified')
    return dest


def _inside(base, target):
    base = os.path.realpath(base)
    target = os.path.realpath(target)
    return target == base or target.startswith(base + os.sep)


def extract(archive, dest, strip=0):
    """Unpack a .zip or .tar.xz/.tar.gz into dest. `strip` drops that many leading path parts. Entries that would land outside dest
    (.. or absolute paths, links) are refused."""
    archive, dest = Path(archive), Path(dest)
    dest.mkdir(parents=True, exist_ok=True)

    def target(name):
        parts = [p for p in re.split(r'[\\/]+', name) if p not in ('', '.')]
        if any(p == '..' for p in parts) or (parts and re.match(r'^[A-Za-z]:$', parts[0])):
            raise FetchError(f'{archive.name}: unsafe path in archive: {name}')
        parts = parts[strip:]
        if not parts:
            return None
        t = dest.joinpath(*parts)
        if not _inside(dest, t):
            raise FetchError(f'{archive.name}: unsafe path in archive: {name}')
        return t
    n = 0
    if archive.suffix.lower() == '.zip' or archive.name.lower().endswith('.whl'):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                t = target(info.filename)
                if t is None:
                    continue
                if info.is_dir():
                    t.mkdir(parents=True, exist_ok=True)
                    continue
                t.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(t, 'wb') as out:
                    shutil.copyfileobj(src, out, CHUNK)
                n += 1
    else:
        with tarfile.open(archive) as tf:
            for m in tf:
                if m.issym() or m.islnk():
                    continue   # the Windows LLVM archive has none; never follow links out of the folder
                t = target(m.name)
                if t is None:
                    continue
                if m.isdir():
                    t.mkdir(parents=True, exist_ok=True)
                elif m.isfile():
                    t.parent.mkdir(parents=True, exist_ok=True)
                    with tf.extractfile(m) as src, open(t, 'wb') as out:
                        shutil.copyfileobj(src, out, CHUNK)
                    n += 1
    return n


def install_pin(key, pin, archive, tools_dir, say=print):
    """Unpack a verified archive into <tools_dir>/<install.dir>. Returns the folder."""
    inst = pin.get('install') or {'dir': key, 'strip': 0}
    target = Path(tools_dir) / inst['dir']
    n = extract(archive, target, strip=int(inst.get('strip', 0)))
    say(f'{key}: unpacked {n} files into {target}')
    return target


def ask_from_args(yes, stdin_is_tty=None):
    """The `ask` callback for the command line: --yes answers yes; otherwise prompt on a terminal; no terminal means no."""
    if yes:
        return lambda q: (print(q + ' yes (--yes)'), True)[1]
    import sys
    tty = sys.stdin.isatty() if stdin_is_tty is None else stdin_is_tty

    def ask(q):
        if not tty:
            print(q + ' no (no terminal; pass --yes to allow downloads)')
            return False
        try:
            return input(q + ' [y/N] ').strip().lower() in ('y', 'yes')
        except EOFError:
            return False
    return ask


def fetch_tools(table, keys, tools_dir, download_dir, ask, offline_dirs=(), opener=None, say=print):
    """Fetch, verify and unpack the named tools ('all' = every pinned download except wheels). Returns {key: folder or file}."""
    pins = dict(list_pins(table))
    if keys == ['all'] or not keys:
        keys = [k for k in pins if not k.startswith('wheel:')]
    out = {}
    for k in keys:
        if k not in pins:
            raise FetchError(f'unknown tool "{k}". Known: {", ".join(pins)}')
        f = fetch_pin(k, pins[k], download_dir, ask, offline_dirs, opener, say)
        out[k] = f if k.startswith('wheel:') else install_pin(k, pins[k], f, tools_dir, say)
    return out
