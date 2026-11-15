"""Upscaler runtime files (AMD FSR, NVIDIA DLSS, Intel XeSS), fetched on the user's PC and installed next to SnapRecomp.exe.

Our release carries no AMD, NVIDIA or Intel binary. For each upscaler the user picks:
  1. the vendor's license text is downloaded (hash checked) and shown;
  2. the user accepts it (CLI: --accept-license <name>, or typing yes at the prompt; we never accept for them);
  3. the files are downloaded from the official URL, their SHA-256 checked against versions.json ("upscalers"), then installed;
     a zip is downloaded once, the pinned members are taken out and the zip is deleted.
A file that fails its hash is deleted and nothing is installed. A missing upscaler is not an error for the game: the runtime hides it.
"""
import hashlib
import json
import os
import time
import zipfile
from pathlib import Path

from . import fetch

RECEIPT = 'upscalers.json'


def pins(table):
    return table.get('upscalers', {})


def default_names(table):
    return [k for k, v in pins(table).items() if v.get('default')]


def _sha(path):
    return fetch.sha256_file(path)


def installed(entry, install_dir):
    """True when every file of the upscaler is present with its pinned hash."""
    lic = entry['license']
    required = [(f['dest'], f) for f in entry['files']] + [('licenses/upscalers/' + lic['filename'], lic)]   # the vendor license is part of the install
    for dest, f in required:
        p = Path(install_dir) / dest
        if not p.is_file() or (f.get('size') is not None and p.stat().st_size != f['size']) or _sha(p) != f['sha256']:
            return False
    return True


def status(table, install_dir):
    return {k: installed(v, install_dir) for k, v in pins(table).items()}


def get_license(name, entry, download_dir, opener=None, say=print):
    """Download (or reuse) the vendor license text, hash checked. Returns its path. Needs no acceptance: it is only the text."""
    lic = entry['license']
    dest = Path(download_dir) / lic['filename']
    if dest.is_file() and _sha(dest) == lic['sha256']:
        return dest
    fetch.download(lic['url'], dest, lic['sha256'], opener=opener, size_hint=lic.get('size'))
    return dest


def _get_archive(name, entry, download_dir, opener, say):
    a = entry['archive']
    dest = Path(download_dir) / a['filename']
    if dest.is_file() and _sha(dest) == a['sha256']:
        return dest
    say(f'{name}: downloading {a["filename"]} ({a["size"] / 2**20:.0f} MB) from {a["url"]}')
    return fetch.download(a['url'], dest, a['sha256'], opener=opener, size_hint=a['size'])


def _install_file(src_bytes_or_path, dest, expect_sha):
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + '.tmp')
    if isinstance(src_bytes_or_path, (bytes, bytearray)):
        tmp.write_bytes(src_bytes_or_path)
    else:
        tmp.write_bytes(Path(src_bytes_or_path).read_bytes())
    if _sha(tmp) != expect_sha:
        tmp.unlink(missing_ok=True)
        raise fetch.FetchError(f'{dest.name}: hash differs after copy')
    os.replace(tmp, dest)


def install(name, entry, install_dir, download_dir, opener=None, say=print):
    """Download, verify and install every file of one upscaler (the license is assumed accepted by the caller). Returns the list of dests."""
    install_dir, download_dir = Path(install_dir), Path(download_dir)
    done = []
    # Fetch and verify everything first; nothing in install_dir changes until every file has passed, so a bad file never leaves a mixed install.
    lic = get_license(name, entry, download_dir, opener, say)   # the license text travels with the files
    staged = []                                                  # (bytes or temp path, dest, sha) for every file, license last
    tmps = []
    try:
        if 'archive' in entry:
            arc = _get_archive(name, entry, download_dir, opener, say)
            with zipfile.ZipFile(arc) as z:
                for f in entry['files']:
                    data = z.read(f['member'])
                    if len(data) != f['size'] or hashlib.sha256(data).hexdigest() != f['sha256']:
                        raise fetch.FetchError(f'{name}: {f["member"]} does not match its pinned hash; nothing installed')
                    staged.append((data, f['dest'], f['sha256']))
        else:
            for f in entry['files']:
                tmp = download_dir / (Path(f['dest']).name + '.dl')
                tmps.append(tmp)
                say(f'{name}: downloading {Path(f["dest"]).name} ({f["size"] / 2**20:.1f} MB) from {f["url"]}')
                fetch.download(f['url'], tmp, f['sha256'], opener=opener, size_hint=f['size'])
                staged.append((tmp, f['dest'], f['sha256']))
        staged.append((lic, 'licenses/upscalers/' + entry['license']['filename'], entry['license']['sha256']))
        for src, dest, sha in staged:
            _install_file(src, install_dir / dest, sha)
            done.append(dest)
        if 'archive' in entry:
            arc.unlink(missing_ok=True)   # keep only the files we use
    finally:
        for t in tmps:
            t.unlink(missing_ok=True)
    return done


def write_receipt(install_dir, name, files, accepted_flag):
    p = Path(install_dir) / RECEIPT
    data = json.loads(p.read_text(encoding='utf-8')) if p.exists() else {}
    data[name] = {'files': files, 'license_accepted_by_user': bool(accepted_flag), 'date': time.strftime('%Y-%m-%d')}
    p.write_text(json.dumps(data, indent=1) + '\n', encoding='utf-8')


def run(table, names, install_dir, download_dir, accepted, ask, opener=None, say=print, show_license=None):
    """Fetch the named upscalers. `accepted` is the set of names the user already accepted (--accept-license); otherwise the user is
    asked after the license text was shown (show_license(name, path)). Returns {name: 'installed'|'already installed'|'declined'}."""
    out = {}
    entries = pins(table)
    for n in names:
        if n not in entries:
            raise fetch.FetchError(f'unknown upscaler "{n}". Known: {", ".join(entries)}')
        e = entries[n]
        if installed(e, install_dir):
            out[n] = 'already installed'
            continue
        lic = get_license(n, e, download_dir, opener, say)
        (show_license or (lambda nm, p: say(f'{nm}: the {e["vendor"]} license is saved at {p}; read it before accepting')))(n, lic)
        if n not in accepted and not ask(f'Do you accept the {e["vendor"]} license for {e["title"]} (text above)? Files will be downloaded from the official source.'):
            out[n] = 'declined'
            say(f'{n}: not installed (license not accepted). The game works without it; the option stays hidden.')
            continue
        files = install(n, e, install_dir, download_dir, opener, say)
        write_receipt(install_dir, n, files, True)
        out[n] = 'installed'
        say(f'{n}: installed ({len(files)} files)')
    return out
