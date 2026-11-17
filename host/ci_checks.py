"""Fast checks behind host/ci_check.ps1 (project tooling): identity/leak scan of the files a commit or release would carry, banned-phrase scan
of the added text of changed .md files, and LICENSE/README agreement.

  python -I host/ci_checks.py identity [--full] [--commits N]
  python -I host/ci_checks.py docs
  python -I host/ci_checks.py license

Each prints findings and ends with one line `RESULT PASS|FAIL <name>: <reason>`; exit 0 on pass, 1 on fail, 3 when a check cannot run.
Nothing here writes to the repository or contacts a network.
"""
import fnmatch
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'builder'))
import leak_scan  # noqa: E402

ALLOW = ROOT / 'host' / 'ci_allow.txt'
ROMFS = ROOT / 'extracted' / 'romfs-v262144'
TERMS = ROOT / 'build' / 'release' / 'leak_terms.txt'
UNSLOP = Path.home() / '.agents' / 'skills' / 'unslop' / 'scripts' / 'banned_phrase_scan.py'
# release-package rules that make no sense for a source tree: DLL manifest, license entries, the product-name rule, and game file names as text
# (the runtime opens the game's archives by name, so code and docs name them; a game file committed by name is still caught by game-file-name)
SOURCE_SKIP_RULES = {'dll', 'license', 'name', 'game-file-name-in-content'}


def git(*a, z=False):
    p = subprocess.run(['git', '-C', str(ROOT)] + list(a), capture_output=True)
    out = p.stdout.decode('utf-8', 'replace')
    return [x for x in out.split('\0') if x] if z else out


def result(ok, name, reason, rc_fail=1):
    print(f'RESULT {"PASS" if ok else "FAIL"} {name}: {reason}')
    return 0 if ok else rc_fail


def changed_files():
    """Modified, staged and new (not ignored) files; deleted ones are skipped."""
    names = set(git('diff', '--name-only', '-z', 'HEAD', z=True)) | set(git('ls-files', '-o', '--exclude-standard', '-z', z=True))
    return sorted(n for n in names if (ROOT / n).is_file())


def all_files():
    names = set(git('ls-files', '-z', z=True)) | set(git('ls-files', '-o', '--exclude-standard', '-z', z=True))
    return sorted(n for n in names if (ROOT / n).is_file())


def load_allow():
    out = []
    if ALLOW.exists():
        for ln in ALLOW.read_text(encoding='utf-8').splitlines():
            ln = ln.split('#')[0].strip()
            if '|' in ln:
                g, r = ln.split('|', 1)
                out.append((g.strip(), r.strip()))
    cf = ROOT / 'builder' / 'leak_allow_content.txt'      # the release scan's pinned third-party false positives apply to the same files at the repo root
    if cf.exists():
        for ln in cf.read_text(encoding='utf-8').splitlines():
            ln = ln.split('#')[0].strip()
            if '|' in ln:
                g, r = ln.split('|', 1)
                out.append((g.strip(), r.strip()))
    return out


def identity(full, commits):
    if not TERMS.exists():
        return result(False, 'identity', f'no forbidden-terms file at {TERMS.relative_to(ROOT)} (a scan that cannot look for the owner\'s names must not pass)', 3)
    terms = [l.strip() for l in TERMS.read_text(encoding='utf-8').splitlines() if l.strip() and not l.startswith('#')]
    files = all_files() if full else changed_files()
    allow = load_allow()

    def entries(_target):
        for rel in files:
            p = ROOT / rel
            yield rel, p.stat().st_size, (lambda p=p: p.read_bytes())

    leak_scan.entries = entries
    af = ROOT / 'builder' / 'leak_allow_names.txt'
    allow_names = [x for x in (l.split('#')[0].strip() for l in af.read_text(encoding='utf-8').splitlines()) if x] if af.exists() else []
    findings, n, _ = leak_scan.scan('.', str(ROMFS) if ROMFS.exists() else None, terms, 80, None, allow_names, [])
    findings = [f for f in findings if f[1] not in SOURCE_SKIP_RULES
                and not (f[1] == 'home-path' and re.search(r'users[\\/]+(<|\.\.\.)', f[2], re.I))      # a documented placeholder, C:\Users\<name>
                and not any(fnmatch.fnmatch(f[0], g) and r in ('*', f[1]) for g, r in allow)]
    # commit metadata of the commits that have not been pushed (or the last N without an upstream)
    rng = '@{u}..HEAD' if subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--verify', '-q', '@{u}'], capture_output=True).returncode == 0 else f'-{commits}'
    meta = git('log', rng, '--format=%H%x1f%an%x1f%ae%x1f%cn%x1f%ce%x1f%B%x1e') if not rng.startswith('-') else git('log', rng, '--format=%H%x1f%an%x1f%ae%x1f%cn%x1f%ce%x1f%B%x1e')
    ncommits = 0
    for rec in [r for r in meta.split('\x1e') if r.strip()]:
        ncommits += 1
        h, *rest = rec.strip().split('\x1f')
        blob = '\n'.join(rest)
        for t in terms:
            if re.search(re.escape(t), blob, re.I):
                findings.append((f'commit {h[:9]}', 'personal', f'author, committer or message contains a forbidden term ({t[:2]}...)'))
        for m in re.finditer(r'[A-Za-z0-9._%+-]{2,64}@[A-Za-z0-9-]{2,63}(?:\.[A-Za-z0-9-]{1,63})*\.[a-z]{2,6}', blob):
            if 'noreply' not in m.group(0).lower():
                findings.append((f'commit {h[:9]}', 'email', f'contains a non-noreply e-mail address ({m.group(0)[:3]}...)'))
    print(f'scanned {n} files ({"all tracked and new" if full else "changed and new"}), {ncommits} commits, {len(terms)} forbidden terms, '
          f'game file names {"checked" if ROMFS.exists() else "NOT checked (no romfs folder)"}')
    for rel, rule, msg in findings[:30]:
        print(f'  FOUND [{rule}] {rel}: {msg}')
    if findings:
        rel, rule, msg = findings[0]
        return result(False, 'identity', f'{len(findings)} finding(s), first [{rule}] {rel}: {msg}')
    return result(True, 'identity', f'{n} files and {ncommits} commits clean')


def added_text(rel):
    """The lines a change adds to a .md file (the whole file when it is new)."""
    if rel in set(git('ls-files', '-z', z=True)):
        diff = git('diff', '-U0', 'HEAD', '--', rel)
        return '\n'.join(l[1:] for l in diff.splitlines() if l.startswith('+') and not l.startswith('+++'))
    return (ROOT / rel).read_text(encoding='utf-8', errors='replace')


def docs(strict):
    mds = [f for f in changed_files() if f.lower().endswith('.md')]
    if not mds:
        return result(True, 'docs', 'no changed .md files')
    if not UNSLOP.exists():
        return result(False, 'docs', f'unslop scanner not found at {UNSLOP}', 3)
    bad = []
    for rel in mds:
        text = added_text(rel)
        if not text.strip():
            continue
        p = subprocess.run([sys.executable, '-I', str(UNSLOP)], input=text.encode('utf-8'), capture_output=True)
        try:
            rep = json.loads(p.stdout.decode('utf-8'))
        except ValueError:
            return result(False, 'docs', f'scanner gave no JSON for {rel}', 3)
        hard, soft = rep['by_severity'].get('hard', 0), rep['by_severity'].get('soft', 0)
        if hard or (strict and soft):
            first = rep['violations'][0]
            bad.append((rel, hard, soft, first['phrase'], first['context'][:70]))
        elif soft:
            print(f'  note: {rel}: {soft} soft flag(s) in the added text')
    for rel, hard, soft, ph, ctx in bad:
        print(f'  FOUND {rel}: {hard} hard, {soft} soft; first "{ph}": {ctx}')
    if bad:
        rel, hard, soft, ph, _ = bad[0]
        return result(False, 'docs', f'{len(bad)} changed .md file(s) with banned phrases, first {rel} ("{ph}")')
    return result(True, 'docs', f'{len(mds)} changed .md file(s), added text clean')


def license_check():
    lic = (ROOT / 'LICENSE').read_text(encoding='utf-8')
    readme = (ROOT / 'README.md').read_text(encoding='utf-8')
    first = next((l.strip() for l in lic.splitlines() if l.strip()), '')
    m = re.match(r'(MIT|Apache|BSD|GNU|ISC|MPL)\b', first)
    if not m:
        return result(False, 'license', f'LICENSE starts with "{first[:40]}", not a known license title')
    spdx = m.group(1)
    names = ('GNU', 'GPL', 'AGPL', 'LGPL') if spdx == 'GNU' else (spdx,)   # the GNU licenses are named GPL-3.0 and so on in badges and prose
    badge = re.search(r'img\.shields\.io/badge/license-([A-Za-z0-9.\-]+?)-', readme)
    sec = re.search(r'(?m)^##\s+License[^\n]*\n+(.*?)(?=^##\s|\Z)', readme, re.S)
    problems = []
    if not badge or not badge.group(1).startswith(names):
        problems.append(f'README badge says "{badge.group(1) if badge else "nothing"}", LICENSE is {spdx}')
    if not sec or not sec.group(1).lstrip().startswith(names):
        problems.append(f'README License section does not start with {spdx}')
    if sec and '(LICENSE)' not in sec.group(1):
        problems.append('README License section does not link LICENSE')
    holder = re.search(r'Copyright \(c\) (\d{4}) (.+)', lic)
    if not holder:
        problems.append('LICENSE has no "Copyright (c) <year> <holder>" line')
    else:
        others = {h.strip() for h in re.findall(r'Copyright \(c\) \d{4} ([^\n.]+)', readme)} - {holder.group(2).strip()}
        if others:
            problems.append(f'README names another copyright holder ({sorted(others)[0][:30]})')
    if not (ROOT / 'THIRD_PARTY_NOTICES.md').exists():
        problems.append('THIRD_PARTY_NOTICES.md is missing')
    for p in problems:
        print(f'  FOUND {p}')
    if problems:
        return result(False, 'license', problems[0])
    return result(True, 'license', f'LICENSE and README agree ({spdx}, holder "{holder.group(2).strip()}")')


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ''
    if cmd == 'identity':
        n = 30
        if '--commits' in argv:
            n = int(argv[argv.index('--commits') + 1])
        return identity('--full' in argv, n)
    if cmd == 'docs':
        return docs('--strict' in argv)
    if cmd == 'license':
        return license_check()
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
