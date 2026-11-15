"""Leak scan for a release staging folder or zip: nothing game-derived and nothing personal may be in it.

  python -I builder/leak_scan.py <folder-or-zip> [--romfs <folder>] [--terms <file>] [--size-cap-mb 80] [--manifest <manifest.json>]

Rules (every finding names the file; exit 1 when there is any):
  1. extensions: no .nso .nca .xci .nsp .nsz .xcz .keys .spv .profdata .tsv .nro .nrr .elf .drp .wem .lmb .nutexb .bfstm .bwav
  2. game file names: no file in the package has the name of a file in the romfs listing, and no file contains one as text
     (checked against --romfs, a folder; the listing is only read, never copied)
  2b. names listed in builder/leak_allow_names.txt (archives the runtime opens by name) are not reported inside file contents
  2c. no file name contains "pokemon" or "pokémon" (release names stay generic)
  3. size cap: no file over the cap unless it is a DLL listed in manifest.json (the "dlls" list: path and SHA-256)
  4. DLLs: every .dll and .exe in the package is listed in manifest.json with a matching SHA-256
  5. personal strings: every term in the terms file (one per line, # comments; kept outside the repository, default
     build/release/leak_terms.txt), the pattern "Users\\<name>" home paths, and e-mail addresses other than noreply ones,
     in file names and contents (ASCII and UTF-16)
Without a terms file the scan refuses to pass (exit 3): a scan that cannot look for the owner's name must not look clean.
"""
import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path

BAD_EXT = {'.nso', '.nca', '.xci', '.nsp', '.nsz', '.xcz', '.keys', '.spv', '.profdata', '.tsv', '.nro', '.nrr', '.elf', '.drp',
           '.wem', '.lmb', '.nutexb', '.bfstm', '.bwav'}
BAD_NAMES = {'pipeline_keys.bin', 'pipeline_keys_seed.bin', 'pipeline_cache.bin'}   # shader-pipeline key lists and driver cache: game-derived, harvested on the user's own PC
HOME_RE = re.compile(rb'(?i)[A-Za-z]:[\\/]{1,2}users[\\/]{1,2}[^\\/\x00\s"\']{1,40}|(?<![A-Za-z])users[\\/]{1,2}[A-Za-z0-9_.-]{2,40}[\\/]')
HOME_RE16 = re.compile(rb'(?i)u\x00s\x00e\x00r\x00s\x00[\\/]\x00[A-Za-z0-9_.-]\x00[A-Za-z0-9_.-]\x00')
EMAIL_RE = re.compile(rb'[A-Za-z0-9._%+-]{2,64}@[A-Za-z0-9-]{2,63}(?:\.[A-Za-z0-9-]{1,63})*\.(?:com|org|net|io|dev|edu|gov|co|me|info|us|xyz|app|ai)(?![A-Za-z])')
ALLOWED_HOME_HITS = (b'users\\public', b'users/public', b'users\\default', b'users/default', b'users\\<', b'users/<')


def romfs_names(romfs):
    names, exts = set(), set()
    for p in Path(romfs).rglob('*'):
        if p.is_file():
            names.add(p.name.lower())
            exts.add(p.suffix.lower().lstrip('.'))
    return names, exts


def entries(target):
    """Yield (relative name, reader) for every file of a folder or zip; reader() returns the bytes."""
    t = Path(target)
    if t.is_file() and t.suffix.lower() == '.zip':
        z = zipfile.ZipFile(t)
        for i in z.infolist():
            if not i.is_dir():
                yield i.filename, i.file_size, (lambda i=i: z.read(i))
    else:
        for p in sorted(t.rglob('*')):
            if p.is_file():
                yield p.relative_to(t).as_posix(), p.stat().st_size, (lambda p=p: p.read_bytes())


def scan(target, romfs=None, terms=(), size_cap_mb=80, manifest=None, allow_names=(), allow_content=()):
    findings = []
    names, exts = romfs_names(romfs) if romfs else (set(), set())
    allow = {n.lower() for n in allow_names}
    allow_c = {(a.split('|')[0].strip().lower(), a.split('|')[1].strip()) for a in allow_content if '|' in a}
    name_re = None
    if exts:
        name_re = re.compile(rb'[A-Za-z0-9_\-\. ()\[\]]{1,200}\.(?:' + '|'.join(re.escape(e) for e in sorted(exts)).encode() + rb')(?![A-Za-z0-9])', re.I)
    term_pats = []
    for t in terms:
        term_pats.append((t, re.compile(re.escape(t.encode('utf-8')), re.I)))
        term_pats.append((t, re.compile(re.escape(t.encode('utf-16-le')), re.I)))
    listed = {(d.get('path') or 'bin/' + d['name']).lower(): d['sha256'] for d in (manifest or {}).get('dlls', [])}
    n_files = 0
    dlls = []
    all_rel = set()
    for rel, size, read in entries(target):
        n_files += 1
        low = rel.lower()
        all_rel.add(low)
        base = low.rsplit('/', 1)[-1]
        ext = Path(low).suffix
        if base in BAD_NAMES:
            findings.append((rel, 'extension', f'{base} is game-derived and must not be released'))
        if ext in BAD_EXT:
            findings.append((rel, 'extension', f'{ext} files must not be released'))
        if re.search(r'(^|/)(amd_fidelityfx|nvngx|_nvngx|libxess|sl\.(dlss|common|interposer)|xess)', low):
            findings.append((rel, 'vendor-binary', 'AMD, NVIDIA and Intel upscaler files are fetched on the user PC, never shipped'))
        if 'pokemon' in low.replace('é', 'e') or 'pokémon' in low:
            findings.append((rel, 'name', 'the product name must not contain the game title in a file name'))
        if base in names:
            findings.append((rel, 'game-file-name', 'the file name matches a file of the game romfs'))
        for t, pat in term_pats[::2]:
            if pat.search(rel.encode('utf-8')):
                findings.append((rel, 'personal', f'file name contains a forbidden term ({t[:2]}...)'))
        data = read()
        if ext in ('.dll', '.exe'):
            dlls.append(rel)
            digest = hashlib.sha256(data).hexdigest()
            if listed.get(low) != digest:
                findings.append((rel, 'dll', 'not listed in manifest.json with this SHA-256' if low not in listed else 'SHA-256 differs from manifest.json'))
        elif size > size_cap_mb * 2**20:
            findings.append((rel, 'size', f'{size / 2**20:.0f} MB is over the {size_cap_mb} MB cap'))
        if ext in ('.dll', '.exe') and size > size_cap_mb * 2**20 and low not in listed:
            findings.append((rel, 'size', f'{size / 2**20:.0f} MB is over the cap'))
        if name_re:
            seen = set()
            for m in name_re.finditer(data):
                cand = m.group(0).decode('latin-1').lower().strip()
                # a name may follow a space or path text: test the tail pieces too
                for piece in {cand, cand.rsplit('/', 1)[-1], cand.rsplit('\\', 1)[-1], cand.rsplit(' ', 1)[-1]}:
                    if piece in names and piece not in seen and piece not in allow:
                        seen.add(piece)
                        findings.append((rel, 'game-file-name-in-content', f'contains the game file name "{piece}"'))
        for t, pat in term_pats:
            if pat.search(data):
                findings.append((rel, 'personal', f'contains a forbidden term ({t[:2]}...)'))
        for m in HOME_RE.finditer(data):
            hit = m.group(0)
            if not hit.lower().startswith(ALLOWED_HOME_HITS):
                findings.append((rel, 'home-path', f'contains a user home path: {hit[:60].decode("latin-1")}'))
                break
        if HOME_RE16.search(data):
            findings.append((rel, 'home-path', 'contains a user home path (UTF-16)'))
        for m in ([] if (ext == '.dll' or low.startswith('licenses/')) else EMAIL_RE.finditer(data)):
            e = m.group(0).lower()
            if b'noreply' not in e and not e.endswith((b'.png', b'.dll', b'.so', b'.h')) and b'@' in e:
                findings.append((rel, 'email', f'contains an e-mail address: {e[:3].decode("latin-1")}...'))
                break
    findings = [f for f in findings if (f[0].lower(), f[1]) not in allow_c]
    for d in (manifest or {}).get('dlls', []):   # a DLL that names a license file must ship it (redistribution notices travel with the binary)
        m = re.search(r'\((licenses/[^)\s]+)\)', d.get('license', ''))
        if m and m.group(1).lower() not in all_rel:
            findings.append((d.get('path') or d['name'], 'license', f'manifest names {m.group(1)} but it is not in the package'))
        if not d.get('license'):
            findings.append((d.get('path') or d['name'], 'license', 'DLL has no license entry in manifest.json'))
    unlisted = sorted(set(listed) - {d.lower() for d in dlls})
    for u in unlisted:
        findings.append((u, 'dll', 'listed in manifest.json but not present in the package'))
    return findings, n_files, len(dlls)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('target')
    ap.add_argument('--romfs')
    ap.add_argument('--terms', help='file with forbidden terms, one per line (default: build/release/leak_terms.txt under the repo)')
    ap.add_argument('--size-cap-mb', type=int, default=80)
    ap.add_argument('--manifest', help='manifest.json listing the DLLs (default: manifest.json at the top of a folder target)')
    ap.add_argument('--allow-names', help='file with game file names the runtime must reference by name (default: builder/leak_allow_names.txt)')
    a = ap.parse_args(argv)
    repo = Path(__file__).resolve().parents[1]
    terms_file = Path(a.terms) if a.terms else repo / 'build' / 'release' / 'leak_terms.txt'
    if not terms_file.exists():
        print(f'refusing to pass: no terms file at {terms_file} (one forbidden term per line: the owner\'s names and addresses)')
        return 3
    terms = [l.strip() for l in terms_file.read_text(encoding='utf-8').splitlines() if l.strip() and not l.startswith('#')]
    mp = Path(a.manifest) if a.manifest else Path(a.target) / 'manifest.json'
    manifest = json.loads(mp.read_text(encoding='utf-8')) if mp.exists() else None
    if manifest is None:
        print(f'note: no manifest.json found ({mp}); every DLL will be reported as unlisted')
    af = Path(a.allow_names) if a.allow_names else Path(__file__).resolve().parent / 'leak_allow_names.txt'
    allow = [l.split('#')[0].strip() for l in af.read_text(encoding='utf-8').splitlines()] if af.exists() else []
    allow = [x for x in allow if x]
    cf = Path(__file__).resolve().parent / 'leak_allow_content.txt'
    allow_c = [l.split('#')[0].strip() for l in cf.read_text(encoding='utf-8').splitlines()] if cf.exists() else []
    findings, n, nd = scan(a.target, a.romfs, terms, a.size_cap_mb, manifest, allow, [x for x in allow_c if x])
    if not a.romfs:
        print('note: no --romfs given, so the game file name rule was not checked')
    print(f'scanned {n} files ({nd} DLLs, {len(terms)} forbidden terms)')
    for rel, rule, msg in findings[:200]:
        print(f'  FOUND [{rule}] {rel}: {msg}')
    if len(findings) > 200:
        print(f'  ... and {len(findings) - 200} more')
    print('LEAK SCAN FAILED' if findings else 'LEAK SCAN PASSED')
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
