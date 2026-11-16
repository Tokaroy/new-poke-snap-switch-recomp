"""Built-in updater: check GitHub Releases for a newer builder, download it, verify the signature, rebuild locally, switch only when the
new build passes, keep the previous build for a one-click rollback.

What an update is: our own code only (the builder, the lifter scripts and the prebuilt runtime objects), as one zip. It never carries
game files; the new version is built again on this PC from your own game files. The zip is signed with an Ed25519 key; the public half
is in builder/update_pubkey.txt. A zip whose signature does not check is deleted and nothing changes.

Safe by construction:
  * The only things this module writes are <state>/versions, <state>/downloads, <state>/state.json and the work folder's builds/.
    Saves, settings, input.json, the shader cache, texcache and the mods folder are never touched.
  * The work folder's out/ is replaced by pipeline.promote only after the new build passes its self-test and the smoke run. A failed
    rebuild leaves out/ exactly as it was. The previous build stays in builds/previous; `rollback` swaps the two.
  * Only a newer version is accepted (the version comes from the signed message). Re-sending an old signed zip is a downgrade and is refused.
Nothing leaves this PC except two plain GET requests to GitHub (the release list and the zip), with a generic User-Agent and no identifiers."""
import ctypes
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

from . import ed25519
from .util import BELOW_NORMAL, Log, StepError

RELEASES_URL = 'https://api.github.com/repos/finalpantasy/new-poke-snap-switch-recomp/releases/latest'
USER_AGENT = 'SnapRecompUpdater'
SIGN_PREFIX = b'snaprecomp-release-v1\n'
ZIP_RE = re.compile(r'^SnapRecomp-Builder-v?(\d+(?:\.\d+){1,3})-win-x64\.zip$')
VERSION_RE = re.compile(r'^\d+(?:\.\d+){1,3}$')
HEX64 = re.compile(r'^[0-9a-f]{64}$')
HEX128 = re.compile(r'^[0-9a-f]{128}$')
MAX_ZIP = 2 << 30          # 2 GiB download
MAX_UNPACKED = 6 << 30     # 6 GiB unpacked
MAX_FILES = 20000
MAX_NOTES = 2000
KEEP_VERSIONS = 2          # managed version folders kept after an update: the new one and the one before it
ALLOWED_HOSTS = ('api.github.com', 'github.com')
ALLOWED_SUFFIXES = ('.githubusercontent.com',)


class UpdateError(StepError):
    """Something stopped the update. The message says what, and that the current build was left as it was."""


# ----------------------------------------------------------------------------------------------------------- versions

def parse_version(s):
    s = str(s).strip()
    if s[:1] in ('v', 'V'):
        s = s[1:]
    if not VERSION_RE.match(s):
        raise UpdateError(f'"{s}" is not a version number (expected digits and dots, for example 1.0.1)')
    return tuple(int(x) for x in s.split('.'))


def norm_version(s):
    return '.'.join(str(x) for x in parse_version(s))


def newer(a, b):
    """True when version a is newer than b (missing parts count as 0)."""
    pa, pb = list(parse_version(a)), list(parse_version(b))
    n = max(len(pa), len(pb))
    return pa + [0] * (n - len(pa)) > pb + [0] * (n - len(pb))


def current_version(builder_dir):
    f = Path(builder_dir) / 'VERSION'
    try:
        return norm_version(f.read_text(encoding='utf-8').strip())
    except OSError:
        raise UpdateError(f'{f} is missing: this builder cannot tell which version it is')


def load_pubkey(builder_dir):
    f = Path(builder_dir) / 'update_pubkey.txt'
    try:
        t = f.read_text(encoding='utf-8').strip().lower()
    except OSError:
        raise UpdateError(f'{f} is missing: updates cannot be verified')
    if not HEX64.match(t):
        raise UpdateError(f'{f} does not hold a 32-byte public key in hex')
    return bytes.fromhex(t)


def signed_message(version, sha256_hex, size):
    return SIGN_PREFIX + f'{version}\n{sha256_hex}\n{int(size)}\n'.encode('ascii')


# ----------------------------------------------------------------------------------------------------------- network

class _HttpsOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # A rename of the repository answers 301 to api.github.com/repositories/<id>/..., and downloads redirect to githubusercontent.com:
        # both are inside the allowlist. Anything else (another host, plain http) is refused, so a redirect cannot leave GitHub.
        _check_url(newurl, False)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _check_url(url, local_ok):
    sp = urllib.parse.urlsplit(url)
    if sp.scheme == 'file' and local_ok:
        return
    if sp.scheme != 'https':
        raise UpdateError(f'refusing {url}: only https links are used')
    host = (sp.hostname or '').lower()
    if host in ALLOWED_HOSTS or host.endswith(ALLOWED_SUFFIXES):
        return
    raise UpdateError(f'refusing {url}: updates only come from GitHub')


def open_url(url, local_ok=False, timeout=30):
    """A plain GET: generic User-Agent, no cookies, no identifiers. local_ok allows file:// (a release folder on this PC, for tests)."""
    _check_url(url, local_ok)
    req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT, 'Accept': 'application/vnd.github+json, application/octet-stream'})
    opener = urllib.request.build_opener(_HttpsOnly)
    try:
        return opener.open(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        raise UpdateError(f'GitHub answered {e.code} for {url}')
    except (urllib.error.URLError, OSError) as e:
        raise UpdateError(f'could not reach {url}: {getattr(e, "reason", e)}')


# ----------------------------------------------------------------------------------------------------------- release info

class Release:
    def __init__(self, version, tag, notes, zip_name, zip_url, zip_size, sig_url, page):
        self.version, self.tag, self.notes = version, tag, notes
        self.zip_name, self.zip_url, self.zip_size, self.sig_url, self.page = zip_name, zip_url, zip_size, sig_url, page


def clean_text(s, limit=MAX_NOTES):
    s = re.sub(r'[\x00-\x08\x0b-\x1f\x7f]', '', str(s or ''))
    return s if len(s) <= limit else s[:limit].rstrip() + ' [...]'


def parse_release(doc):
    """Turn the GitHub release JSON into a Release. Drafts and prereleases are ignored by the "latest" endpoint; refuse them anyway."""
    if not isinstance(doc, dict):
        raise UpdateError('the release list was not what was expected')
    if doc.get('draft') or doc.get('prerelease'):
        raise UpdateError('the newest release is a draft or a prerelease; ignored')
    tag = str(doc.get('tag_name') or '')
    version = norm_version(tag)
    assets = {a.get('name'): a for a in doc.get('assets') or [] if isinstance(a, dict)}
    zips = [n for n in assets if isinstance(n, str) and ZIP_RE.match(n)]
    if len(zips) != 1:
        raise UpdateError('the release does not have exactly one builder zip (SnapRecomp-Builder-<version>-win-x64.zip)')
    zname = zips[0]
    if norm_version(ZIP_RE.match(zname).group(1)) != version:
        raise UpdateError(f'the zip name {zname} does not match the release tag {tag}')
    sig = assets.get(zname + '.sig')
    if not sig:
        raise UpdateError(f'the release has no {zname}.sig signature file; it is not trusted')
    z = assets[zname]
    size = z.get('size')
    if not isinstance(size, int) or size <= 0 or size > MAX_ZIP:
        raise UpdateError('the zip has no usable size or is larger than 2 GiB')
    return Release(version, tag, clean_text(doc.get('body')), zname, str(z.get('browser_download_url') or ''), size,
                   str(sig.get('browser_download_url') or ''), str(doc.get('html_url') or ''))


def fetch_release(url=None, local_ok=None):
    local_ok = bool(url) if local_ok is None else local_ok
    with open_url(url or RELEASES_URL, local_ok) as r:
        data = r.read(4 << 20)
    try:
        return parse_release(json.loads(data.decode('utf-8')))
    except ValueError:
        raise UpdateError('the release list is not valid JSON')


# ----------------------------------------------------------------------------------------------------------- state

def default_state_dir():
    base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
    return Path(base) / 'SnapRecomp' / 'updates'


def load_state(state_dir):
    f = Path(state_dir) / 'state.json'
    try:
        d = json.loads(f.read_text(encoding='utf-8'))
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def save_state(state_dir, d):
    state_dir = Path(state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    tmp = state_dir / 'state.json.tmp'
    tmp.write_text(json.dumps(d, indent=1), encoding='utf-8')
    os.replace(tmp, state_dir / 'state.json')


def pid_alive(pid):
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pid))   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    try:
        code = ctypes.c_ulong()
        ok = ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        return bool(ok) and code.value == 259   # STILL_ACTIVE
    finally:
        ctypes.windll.kernel32.CloseHandle(h)


def wait_for_pid(pid, seconds=120):
    t0 = time.time()
    while pid_alive(pid):
        if time.time() - t0 > seconds:
            raise UpdateError(f'process {pid} (the launcher) is still running; close it and try again')
        time.sleep(0.5)


class Lock:
    """One update at a time. A lock left by a dead process is taken over."""

    def __init__(self, state_dir):
        self.path = Path(state_dir) / 'update.lock'

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        for _ in range(2):
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, str(os.getpid()).encode())
                os.close(fd)
                return self
            except FileExistsError:
                try:
                    pid = int(self.path.read_text().strip() or 0)
                except (OSError, ValueError):
                    pid = 0
                if pid and pid_alive(pid):
                    raise UpdateError(f'another update is running (process {pid})')
                self.path.unlink(missing_ok=True)
        raise UpdateError('could not take the update lock')

    def __exit__(self, *a):
        self.path.unlink(missing_ok=True)


# ----------------------------------------------------------------------------------------------------------- check

def read_last_build(work):
    try:
        d = json.loads((Path(work) / 'last_build.json').read_text(encoding='utf-8'))
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def estimate_minutes(work):
    """(low, high) minutes of rebuild, from the step times of the last build here. Low = compile, link and the start-up check;
    high adds the code generator, which runs again only when the generator or the runtime interface changed. Ghidra never reruns
    for an update (the function lists are cached by the hash of your game files). None when there is no earlier build to go by."""
    lb = read_last_build(work)
    st = lb.get('steps') or {}
    if not st:
        return None
    base = sum(float(st.get(k, 0)) for k in ('compile_main', 'compile_sdk', 'link')) + float(lb.get('smoke_seconds') or 400)
    extra = sum(float(st.get(k, 0)) for k in ('gen_main', 'gen_sdk'))
    # cap the generator figure: a cached run records its restore time, a cold one its real time
    lo = max(1, round(base / 60))
    hi = max(lo, round((base + max(extra, 60)) / 60))
    return lo, hi


def human_size(n):
    return f'{n / 2**20:.0f} MB' if n >= 2**20 else f'{n / 1024:.0f} KB'


def check(builder_dir, state_dir, work=None, url=None, min_age_hours=None, now=None):
    """Ask GitHub for the newest release and say whether it is newer than this builder. Returns a dict:
    checked, current, latest, update_available, notes, size, size_text, estimate, page. Never raises for a network problem:
    returns {"checked": False, "error": ...} so a start-up check cannot get in the way."""
    now = time.time() if now is None else now
    cur = current_version(builder_dir)
    st = load_state(state_dir)
    out = {'checked': False, 'current': cur, 'update_available': False}
    if min_age_hours is not None and now - float(st.get('last_check', 0)) < min_age_hours * 3600:
        out['skipped'] = 'checked recently'
        if st.get('latest') and newer(st['latest'].get('version', '0'), cur):   # the last answer still stands
            out.update(update_available=True, latest=st['latest']['version'], notes=st['latest'].get('notes', ''),
                       size=st['latest'].get('size', 0), size_text=human_size(st['latest'].get('size', 0)))
            est = estimate_minutes(work) if work else None
            if est:
                out['estimate'] = {'low_min': est[0], 'high_min': est[1]}
        return out
    try:
        rel = fetch_release(url)
    except UpdateError as e:
        out['error'] = str(e)
        return out
    out['checked'] = True
    st['last_check'] = now
    st['latest'] = {'version': rel.version, 'notes': rel.notes, 'size': rel.zip_size}
    try:
        save_state(state_dir, st)
    except OSError:
        pass
    out.update(latest=rel.version, notes=rel.notes, size=rel.zip_size, size_text=human_size(rel.zip_size), page=rel.page)
    if newer(rel.version, cur):
        out['update_available'] = True
        if work:
            est = estimate_minutes(work)
            if est:
                out['estimate'] = {'low_min': est[0], 'high_min': est[1]}
    elif newer(cur, rel.version):
        out['note'] = f'the newest release ({rel.version}) is older than this builder ({cur}); nothing to do'
    return out


CHECK_INTERVAL_HOURS = 24


def startup_check_enabled(config_path):
    """The start-up check is ON unless config.json says "update_check": false (the launcher's Updates menu writes that)."""
    try:
        d = json.loads(Path(config_path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return True
    return d.get('update_check', True) is not False


def startup_check(builder_dir, state_dir, config_path, work=None, url=None, now=None):
    """What the launcher runs in the background at start: at most one GitHub request per 24 hours, none when switched off,
    silent when offline or rate-limited (a failed try is not counted, so the next start tries again). Only looks and reports;
    installing is a separate click. Returns None when switched off, else the dict from check()."""
    if not startup_check_enabled(config_path):
        return None
    return check(builder_dir, state_dir, work=work, url=url, min_age_hours=CHECK_INTERVAL_HOURS, now=now)


# ----------------------------------------------------------------------------------------------------------- download and verify

def parse_sig(text):
    try:
        d = json.loads(text)
    except ValueError:
        raise UpdateError('the signature file is not valid JSON')
    if not isinstance(d, dict) or d.get('format') != 1:
        raise UpdateError('the signature file has an unknown format')
    v, h, s, g = d.get('version'), d.get('sha256'), d.get('size'), d.get('signature')
    if not (isinstance(v, str) and VERSION_RE.match(v) and isinstance(h, str) and HEX64.match(h) and isinstance(s, int) and s > 0
            and isinstance(g, str) and HEX128.match(g)):
        raise UpdateError('the signature file is malformed')
    return d


class ShortDownload(UpdateError):
    """The zip arrived with fewer bytes than the release says, or the connection broke: worth trying again."""


DOWNLOAD_TRIES = 3
DOWNLOAD_WAITS = (2, 5)   # seconds before the 2nd and 3rd try


def download(rel, dest_dir, url_local_ok=False, progress=None, tries=None, sleep=time.sleep):
    """download_once with a retry: a short zip or a broken connection is tried again (3 tries, short waits) before it fails, in plain words.
    A zip larger than the release says, or a wrong signature later on, is never retried."""
    tries = tries or DOWNLOAD_TRIES
    for i in range(tries):
        try:
            return download_once(rel, dest_dir, url_local_ok, progress)
        except ShortDownload as e:
            if i == tries - 1:
                raise UpdateError(f'{e} It failed {tries} times in a row, so nothing was installed. Check your internet connection and try again.') from e
            sleep(DOWNLOAD_WAITS[min(i, len(DOWNLOAD_WAITS) - 1)])


def download_once(rel, dest_dir, url_local_ok=False, progress=None):
    """Download the zip and its signature into dest_dir. Returns (zip_path, sig_text). A failed or short download leaves nothing behind."""
    dest_dir = Path(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)
    zpath = dest_dir / rel.zip_name
    part = dest_dir / (rel.zip_name + '.part')
    for p in (zpath, part):
        p.unlink(missing_ok=True)
    try:
        with open_url(rel.sig_url, url_local_ok) as r:
            sig_text = r.read(8192).decode('utf-8', 'replace')
        done = 0
        with open_url(rel.zip_url, url_local_ok, timeout=60) as r, open(part, 'wb') as f:
            while True:
                try:
                    b = r.read(1 << 20)
                except OSError as e:   # the connection broke part-way (timeout, reset)
                    raise ShortDownload(f'the connection broke after {done} of {rel.zip_size} bytes ({e}).') from e
                if not b:
                    break
                done += len(b)
                if done > rel.zip_size:
                    raise UpdateError('the download is larger than the release says')
                f.write(b)
                if progress:
                    progress(done, rel.zip_size)
        if done != rel.zip_size:
            raise ShortDownload(f'the download stopped early ({done} of {rel.zip_size} bytes).')
        os.replace(part, zpath)
    except BaseException:
        part.unlink(missing_ok=True)
        zpath.unlink(missing_ok=True)
        raise
    return zpath, sig_text


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def verify_download(zpath, sig_text, rel, pubkey, installed):
    """Check the signature and the version. Deletes the zip and raises on any problem. Returns the signed version."""
    zpath = Path(zpath)
    try:
        sig = parse_sig(sig_text)
        digest, size = sha256_of(zpath), zpath.stat().st_size
        if digest != sig['sha256'] or size != sig['size']:
            raise UpdateError('the downloaded zip does not match its signature file (hash or size differs)')
        if not ed25519.verify(pubkey, signed_message(sig['version'], sig['sha256'], sig['size']), bytes.fromhex(sig['signature'])):
            raise UpdateError('the signature is not valid for this zip; it was NOT made by the release key')
        version = norm_version(sig['version'])
        if version != rel.version:
            raise UpdateError(f'the signed version {version} differs from the release tag {rel.version}')
        if not newer(version, installed):
            raise UpdateError(f'version {version} is not newer than the installed {installed}; downgrades are refused')
        return version
    except BaseException:
        zpath.unlink(missing_ok=True)
        raise


# ----------------------------------------------------------------------------------------------------------- unpack

def _safe_member(name):
    n = name.replace('\\', '/')
    if n.startswith('/') or re.match(r'^[A-Za-z]:', n) or ':' in n:
        return None
    parts = [p for p in n.split('/') if p not in ('', '.')]
    if not parts or any(p == '..' for p in parts):
        return None
    return parts


def unpack(zpath, dest, version):
    """Unpack into dest (new folder). Refuses path tricks, links and oversize archives. A single wrapping folder is removed.
    Checks that builder/VERSION says `version`. Returns dest."""
    dest = Path(dest)
    if dest.exists():
        raise UpdateError(f'{dest} already exists')
    part = dest.with_name(dest.name + '.part')
    shutil.rmtree(part, ignore_errors=True)
    try:
        with zipfile.ZipFile(zpath) as z:
            infos = z.infolist()
            if len(infos) > MAX_FILES or sum(i.file_size for i in infos) > MAX_UNPACKED:
                raise UpdateError('the zip is larger than an update should be')
            names = []
            for i in infos:
                parts = _safe_member(i.filename)
                if parts is None:
                    raise UpdateError(f'the zip holds an unsafe path ({i.filename!r}); refused')
                if stat.S_ISLNK((i.external_attr >> 16) & 0xFFFF):
                    raise UpdateError(f'the zip holds a link ({i.filename!r}); refused')
                names.append(parts)
            wrap = names[0][0] if names and all(len(p) > 1 for p in names) and len({p[0] for p in names}) == 1 else None
            part.mkdir(parents=True)
            root = part.resolve()
            for i, parts in zip(infos, names):
                rel = parts[1:] if wrap else parts
                if not rel:
                    continue
                target = (part.joinpath(*rel)).resolve()
                if root not in target.parents and target != root:
                    raise UpdateError(f'the zip holds an unsafe path ({i.filename!r}); refused')
                if i.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(i) as src, open(target, 'wb') as out:
                        shutil.copyfileobj(src, out, 1 << 20)
        for need in ('builder/snap_builder.py', 'builder/VERSION', 'builder/update_pubkey.txt', 'prebuilt/manifest.json'):
            if not (part / need).exists():
                raise UpdateError(f'the update is missing {need}')
        if current_version(part / 'builder') != norm_version(version):
            raise UpdateError(f'builder/VERSION in the zip does not say {version}')
        os.replace(part, dest)
    except BaseException:
        shutil.rmtree(part, ignore_errors=True)
        raise
    return dest


# ----------------------------------------------------------------------------------------------------------- rebuild

REPLAY_FLAGS = ('maps', 'llvm', 'python', 'vulkan_lib', 'ghidra', 'jdk', 'harness', 'ghost', 'cache_dir')


def build_command(new_builder_dir, last):
    """The build command line for the new version, from the arguments the last build here used (prebuilt is dropped: the new
    version brings its own runtime objects). Returns (argv, work)."""
    a = (last or {}).get('args') or {}
    for k in ('main', 'sdk', 'romfs', 'work'):
        if not a.get(k):
            raise UpdateError('this work folder has no record of the last build (last_build.json); run the builder once with the new version by hand')
    cmd = [sys.executable, '-I', str(Path(new_builder_dir) / 'snap_builder.py'), 'build', '--main', a['main'], '--sdk', a['sdk'],
           '--romfs', a['romfs'], '--work', a['work']]
    for k in REPLAY_FLAGS:
        if a.get(k):
            cmd += ['--' + k.replace('_', '-'), str(a[k])]
    if a.get('jobs'):
        cmd += ['--jobs', str(a['jobs'])]
    if a.get('smoke_timeout'):
        cmd += ['--smoke-timeout', str(a['smoke_timeout'])]
    if a.get('cache') is False:
        cmd.append('--no-cache')
    if a.get('skip_smoke'):
        cmd.append('--skip-smoke')
    if a.get('upscalers'):
        cmd += ['--upscalers', ','.join(a['upscalers'])]
    for n in a.get('accept_license') or []:
        cmd += ['--accept-license', n]
    return cmd, a['work']


def run_build(cmd, log):
    """Run the new builder, echo its output, return its exit code."""
    log.write('rebuilding with the new version: ' + Path(cmd[3]).parent.name)
    p = subprocess.Popen([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=BELOW_NORMAL)

    def pump():
        for line in iter(p.stdout.readline, b''):
            log.write('  | ' + line.decode('utf-8', 'replace').rstrip())
    th = threading.Thread(target=pump, daemon=True)
    th.start()
    code = p.wait()
    th.join(10)
    return code


def prune(state_dir, keep_dirs):
    """Delete managed version folders other than keep_dirs, and any leftover download."""
    sd = Path(state_dir)
    vd = sd / 'versions'
    keep = {str(Path(k).resolve()).lower() for k in keep_dirs if k}
    if vd.exists():
        for d in vd.iterdir():
            if d.is_dir() and str(d.resolve()).lower() not in keep:
                shutil.rmtree(d, ignore_errors=True)
    shutil.rmtree(sd / 'downloads', ignore_errors=True)


def apply(builder_dir, work, state_dir, url=None, log=None, run=None, progress=None, confirm=None, wait_pid=None):
    """Download, verify, unpack, rebuild here, and switch only when the new build passes. Returns a result dict.
    `run(cmd, log)` runs the new builder (tests pass a fake). `confirm(info)` may return False to stop before downloading."""
    log = log or Log(Path(work) / 'update.log')
    run = run or run_build
    builder_dir, work, state_dir = Path(builder_dir), Path(work), Path(state_dir)
    local_ok = bool(url)
    if wait_pid:
        wait_for_pid(wait_pid)
    with Lock(state_dir):
        installed = current_version(builder_dir)
        pub = load_pubkey(builder_dir)
        rel = fetch_release(url)
        if not newer(rel.version, installed):
            log.write(f'already up to date (installed {installed}, newest release {rel.version})')
            return {'updated': False, 'current': installed, 'latest': rel.version}
        info = {'version': rel.version, 'notes': rel.notes, 'size': rel.zip_size, 'estimate': estimate_minutes(work)}
        if confirm and not confirm(info):
            return {'updated': False, 'current': installed, 'latest': rel.version, 'declined': True}
        last = read_last_build(work)
        build_command(builder_dir, last)   # fails early, before any download, when this work folder has no build record
        log.write(f'downloading {rel.zip_name} ({human_size(rel.zip_size)})')
        zpath, sig_text = download(rel, state_dir / 'downloads', local_ok, progress)
        version = verify_download(zpath, sig_text, rel, pub, installed)
        log.write(f'signature ok for version {version}')
        vdir = state_dir / 'versions' / version
        if vdir.exists():
            shutil.rmtree(vdir)
        unpack(zpath, vdir, version)
        zpath.unlink(missing_ok=True)
        cmd, _ = build_command(vdir / 'builder', last)
        code = run(cmd, log)
        if code != 0:
            shutil.rmtree(vdir, ignore_errors=True)
            prune(state_dir, [builder_dir])
            raise UpdateError(f'the rebuild with version {version} failed (exit code {code}). Your current game build was left exactly as it was. '
                              f'Details: {work / "builder.log"}')
        st = load_state(state_dir)
        st.update(previous=installed, previous_dir=str(builder_dir), current=version, current_dir=str(vdir))
        save_state(state_dir, st)
        prune(state_dir, [vdir, builder_dir])
        log.write(f'updated {installed} -> {version}. The earlier build is in {work / "builds" / "previous"} (rollback restores it).')
        return {'updated': True, 'from': installed, 'to': version, 'builder_dir': str(vdir)}


def read_build_version(build_dir):
    try:
        d = json.loads((Path(build_dir) / 'config.json').read_text(encoding='utf-8'))
        return str(d.get('builder_version') or 'unknown')
    except (OSError, ValueError):
        return 'unknown'


def rollback(work, state_dir, log=None, wait_pid=None):
    """Swap out/ and builds/previous. Run twice to go forward again. Needs the game closed (Windows will not rename a folder in use)."""
    from .pipeline import _replace_retry
    log = log or Log(Path(work) / 'update.log')
    work, state_dir = Path(work), Path(state_dir)
    out, prev, tmp = work / 'out', work / 'builds' / 'previous', work / 'builds' / '_swap'
    if wait_pid:
        wait_for_pid(wait_pid)
    if not (prev / 'SnapRecomp.exe').exists():
        raise UpdateError('there is no earlier build to go back to')
    if not (out / 'SnapRecomp.exe').exists():
        raise UpdateError(f'{out} has no build to swap with')
    with Lock(state_dir):
        shutil.rmtree(tmp, ignore_errors=True)
        try:
            _replace_retry(out, tmp)
            try:
                _replace_retry(prev, out)
            except OSError:
                _replace_retry(tmp, out)   # put it back
                raise
            _replace_retry(tmp, prev)
        except OSError as e:
            raise UpdateError(f'could not swap the builds ({e}). Close the game and the folders under {work} and try again; nothing was changed.')
        was, now = read_build_version(prev), read_build_version(out)
        st = load_state(state_dir)
        st.update(current=now, previous=was)
        save_state(state_dir, st)
        try:
            (work / 'current.json').write_text(json.dumps({'build': out.name, 'out': str(out), 'previous': str(prev), 'rolled_back_from': was,
                                                           'switched': time.strftime('%Y-%m-%d %H:%M:%S')}, indent=1), encoding='utf-8')
        except OSError:
            pass
        log.write(f'rolled back: {out} is build version {now} again; version {was} is in {prev}')
        return {'now': now, 'previous': was}
