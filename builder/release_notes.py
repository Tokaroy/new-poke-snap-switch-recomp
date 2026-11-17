"""Release notes: the fixed template, a lint for it, and the banner for stale releases.

  python builder/release_notes.py new --version 1.2.3 [--testing]   print a notes skeleton
  python builder/release_notes.py lint notes.md                      exit 1 and list the problems
  python builder/release_notes.py stale --latest v1.3.0 --url URL    print the banner to put on top of an older release

The section order is fixed: Known issues, Saves and settings carry over, Driver notes, Changes, Install or update.
Known issues come first so nobody installs without seeing them. Every change bullet links its pull request or issue.
"""
import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEMPLATE = HERE / 'release' / 'NOTES_TEMPLATE.md'
REPO_URL = 'https://github.com/finalpantasy/new-pokemon-snap-recomp'

SECTIONS = ('Known issues', 'Saves and settings carry over', 'Driver notes', 'Changes', 'Install or update')
BANNER_MARK = 'A newer release is out'

# Nothing personal goes onto a public release page.
PERSONAL = [re.compile(r'[A-Za-z]:\\Users\\', re.I), re.compile(r'[\w.+-]+@[\w-]+\.[\w.-]+')]
# The owner's names live in the git-ignored leak list that builder/leak_scan.py uses, never in this file.
_TERMS = HERE.parent / 'build' / 'release' / 'leak_terms.txt'
if _TERMS.exists():
    PERSONAL += [re.compile(r'\b' + re.escape(t.strip()) + r'\b', re.I)
                 for t in _TERMS.read_text(encoding='utf-8', errors='replace').splitlines() if len(t.strip()) >= 3 and '@' not in t]
LINK = re.compile(r'\[[^\]]*#\d+[^\]]*\]\(' + re.escape(REPO_URL) + r'/(?:pull|issues)/\d+\)')
PLACEHOLDERS = ('Short plain description of the change', 'Tested on driver version X', '/pull/123)')


def skeleton(version, testing=False):
    text = TEMPLATE.read_text(encoding='utf-8')
    text = re.sub(r'<!--.*?-->\s*', '', text, flags=re.S)   # the authoring hints are for the writer, not the page
    head = f'# Snap Recomp {version}' + (' (Testing)' if testing else '')
    lines = [head, '']
    if testing:
        lines += ['This is an early build on the Testing channel. It may have bugs. Stable users are not offered it.', '']
    return '\n'.join(lines) + text.strip() + '\n'


def banner(latest_tag, url=None):
    url = url or f'{REPO_URL}/releases/latest'
    return f'> **{BANNER_MARK}: {latest_tag}.** This release is old; install the latest one instead: {url}\n'


def sections_of(text):
    """(heading, body lines) for every level-2 heading, in order."""
    out, cur = [], None
    for line in text.splitlines():
        m = re.match(r'##\s+(.*?)\s*$', line)
        if m:
            cur = (m.group(1), [])
            out.append(cur)
        elif cur is not None:
            cur[1].append(line)
    return out


def bullets(body):
    return [ln.strip()[2:].strip() for ln in body if ln.strip().startswith(('- ', '* '))]


def lint(text):
    """Returns a list of problems; empty means the notes follow the template."""
    problems = []
    body = re.sub(r'<!--.*?-->', '', text, flags=re.S)
    secs = sections_of(body)
    names = [n for n, _ in secs]
    if tuple(n for n in names if n in SECTIONS) != tuple(n for n in SECTIONS if n in names):
        problems.append('sections are out of order; the order is: ' + ', '.join(SECTIONS))
    for want in SECTIONS:
        if want not in names:
            problems.append(f'missing section "{want}"')
    by = dict(secs)
    ki = bullets(by.get('Known issues', []))
    if 'Known issues' in by and not ki:
        problems.append('"Known issues" has no bullets (write "None known." if there are none)')
    carry = ' '.join(by.get('Saves and settings carry over', [])).lower()
    if 'Saves and settings carry over' in by and not ('saves' in carry and 'settings' in carry and 'copies' in carry):
        problems.append('"Saves and settings carry over" must say the updater copies saves and settings first')
    dn = bullets(by.get('Driver notes', []))
    if 'Driver notes' in by and not dn:
        problems.append('"Driver notes" has no bullets (write "No driver notes." if there are none)')
    ch = bullets(by.get('Changes', []))
    if 'Changes' in by:
        if not ch:
            problems.append('"Changes" has no bullets')
        for b in ch:
            if not LINK.search(b):
                problems.append(f'change without a pull request or issue link: "{b[:60]}"')
    for ph in PLACEHOLDERS:
        if ph in body:
            problems.append(f'template placeholder left in the notes: "{ph}"')
    first_h1 = next((ln for ln in body.splitlines() if ln.startswith('# ')), None)
    if first_h1 is None:
        problems.append('no title line ("# Snap Recomp <version>")')
    for rx in PERSONAL:
        m = rx.search(body)
        if m:
            problems.append(f'personal detail in the notes: "{m.group(0)}"')
    return problems


def with_banner(text, latest_tag, url=None):
    """Put the stale-release banner on top; replaces an older banner instead of stacking them."""
    kept = [ln for ln in text.splitlines() if BANNER_MARK not in ln]
    while kept and not kept[0].strip():
        kept.pop(0)
    return banner(latest_tag, url) + '\n' + '\n'.join(kept) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    n = sub.add_parser('new')
    n.add_argument('--version', required=True)
    n.add_argument('--testing', action='store_true')
    l = sub.add_parser('lint')
    l.add_argument('file')
    s = sub.add_parser('stale')
    s.add_argument('--latest', required=True)
    s.add_argument('--url')
    s.add_argument('--file', help='put the banner on top of this notes file and print the result')
    a = ap.parse_args(argv)
    if a.cmd == 'new':
        sys.stdout.write(skeleton(a.version, a.testing))
        return 0
    if a.cmd == 'lint':
        probs = lint(Path(a.file).read_text(encoding='utf-8'))
        for p in probs:
            print('FAIL', p)
        print('release notes OK' if not probs else f'{len(probs)} problem(s)')
        return 1 if probs else 0
    if a.cmd == 'stale':
        if a.file:
            sys.stdout.write(with_banner(Path(a.file).read_text(encoding='utf-8'), a.latest, a.url))
        else:
            sys.stdout.write(banner(a.latest, a.url))
        return 0
    return 2


if __name__ == '__main__':
    sys.exit(main())
