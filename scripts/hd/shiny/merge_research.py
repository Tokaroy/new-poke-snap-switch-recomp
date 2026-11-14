# SPDX-License-Identifier: MIT
r"""Merges the official-shiny-color research (research/batch_NN.json, text only) into shiny_colors.txt.

  python -E -P scripts\hd\shiny\merge_research.py [--dry-run]

Every table line whose description starts with `approx` (or that is `skip` and has a research entry) gets the research description and
rules. Hand-written lines and palettes.json overrides are left alone. Every merged rule is parsed with make_shiny_pack.parse_rules
and a failure stops the merge. Entry fields: n, name, normal, shiny, desc, rules, source, confidence, note.
"""
import glob, json, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import make_shiny_pack as m  # noqa: E402


def main():
    dry = '--dry-run' in sys.argv
    res = {}
    for f in sorted(glob.glob(str(HERE / 'research' / 'batch_*.json'))):
        for e in json.load(open(f, encoding='utf-8')):
            res[int(e['n'])] = e
    tab = HERE / 'shiny_colors.txt'
    out, merged, from_skip, kept, unmergeable = [], 0, [], 0, []
    for ln in tab.read_text(encoding='utf-8').splitlines():
        if not ln.strip() or ln.startswith('#'):
            out.append(ln); continue
        n, name, desc, rules = ln.split('|')
        n = int(n)
        e = res.get(n)
        if e and e['rules'].strip() == 'skip':
            unmergeable.append('%d %s' % (n, name)); out.append(ln); kept += 1
        elif e and (desc.strip().startswith('approx') or rules.strip() == 'skip'):
            if rules.strip() == 'skip':
                from_skip.append('%d %s' % (n, name))
            r = e['rules'].strip()
            assert m.parse_rules(r), 'rule does not parse: %d %s %r' % (n, name, r)
            d = e['desc'].replace('|', '/').strip()
            out.append('%d|%s|%s [%s]|%s' % (n, name, d, e.get('confidence', '?'), r))
            merged += 1
        else:
            out.append(ln); kept += 1
    print('merged %d researched lines, kept %d; replaced skip lines: %s; research said skip (kept as is): %s; research entries unused: %s' % (
        merged, kept, from_skip, unmergeable, sorted(set(res) - {int(l.split('|')[0]) for l in out if l.strip() and not l.startswith('#')})))
    if not dry:
        tab.write_text('\n'.join(out) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
