"""Cross-check every player option on the four surfaces: launcher pages, F1 menu, native tabs (the game's own Settings screen) and `--set`.

  python -I scripts/ui/option_audit.py            print the table and every problem; exit 1 when there is one
  python -I scripts/ui/option_audit.py --md docs/option-map.md   write the table as Markdown

Reads our own sources only (no game data): scripts/ui/make_native_options.py (TABS), runtime/ui/ui_options.cpp (the F1 option table),
host/launcher_ui.cpp (build_pages), runtime/hle/settings.cpp (kEnvKeys, key reads) and host/cli_flags.h (the --set aliases).
Problems it reports: a native row without a help line or a --set key, a native help line that says "Applies after restart." for a row the F1
table applies live (or the reverse), an option that is on the launcher or in the F1 table but on no native tab (listed as "no native row",
not an error: the F1 menu stays as the fallback until every row is native).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)


def read(rel):
    return open(os.path.join(ROOT, rel), encoding='utf-8').read()


def native_rows():
    argv, sys.argv = sys.argv, ['x']
    try:
        import make_native_options as M
    finally:
        sys.argv = argv
    out = {}
    for tab, _, rows in M.TABS:
        for r in rows:
            out[r['key']] = dict(label=r['label'], tab=tab, store=r['store'], help=r['help'])
    return out


def f1_rows():
    """key -> dict(label, tab, section, when). The F1 tab is the option vector's name (graphics_opts -> Graphics)."""
    src = read('runtime/ui/ui_options.cpp')
    out, tab, sec = {}, '', ''
    for line in src.splitlines():
        m = re.match(r'std::vector<Opt>& (\w+)_opts\(\)', line)
        if m:
            tab, sec = m.group(1).capitalize(), ''
            continue
        m = re.match(r'\s*\{"",\s*"([^"]+)",\s*kSection', line)
        if m:
            sec = m.group(1)
            continue
        m = re.match(r'\s*\{"(\w+)",\s*"([^"]+)",\s*(k\w+),\s*(kLive|kRestart|kSoon)\b', line)
        if m and tab:
            out[m.group(1)] = dict(label=m.group(2), tab=tab, section=sec, when=m.group(4)[1:].lower(), kind=m.group(3))
    return out


# launcher keys stored outside settings.cfg use their own names; map them to the key the F1 menu and the native tabs use
LAUNCHER_ALIAS = {'mode': 'win_mode', 'monitor': 'win_monitor', 'pause_on_focus_loss': 'win_pause', 'mute_in_background': 'win_mute',
                  'output': 'audio_output', 'master': 'master', 'bgm': 'music_volume', 'se': 'effects_volume', 'voice': 'voice_volume'}


def launcher_rows():
    src = read('host/launcher_ui.cpp')
    out, page = {}, ''
    for line in src.splitlines():
        m = re.search(r'Page p; p\.name = "([^"]+)"', line)
        if m:
            page = m.group(1)
        for m in re.finditer(r'choice\("([^"]+)",\s*"(\w*)",\s*(S_\w+)', line):
            key = m.group(2)
            if not key:
                continue
            out[LAUNCHER_ALIAS.get(key, key)] = dict(label=m.group(1), page=page)
    return out


def cli_keys():
    """Keys --set takes: settings.cfg keys that reach a variable (kEnvKeys, kPassKeys), keys the runtime reads straight from settings
    (settings.cpp get calls and the like), and the aliases host/cli_flags.h turns into variables."""
    s = read('runtime/hle/settings.cpp')
    keys = set(re.findall(r'\{"(\w+)",\s*"SNAP_\w+"\}', s))
    keys |= set(re.findall(r'\{"(gfx_pass_\w+)"', s))
    keys |= set(re.findall(r'\bget(?:_bool|_float|_int)?\("([a-z_0-9]+)"', s))
    for top in ('runtime', 'host'):
        for dirpath, _, files in os.walk(os.path.join(ROOT, top)):
            for fn in files:
                if fn.endswith(('.cpp', '.h')):
                    text = open(os.path.join(dirpath, fn), encoding='utf-8', errors='replace').read()
                    keys |= set(re.findall(r'\b(?:settings|st)::get(?:_bool|_float|_int)?\("([a-z_0-9]+)"', text))
    cf = read('host/cli_flags.h')
    keys |= set(re.findall(r'\{"(\w+)",\s*"SNAP_\w+"\}', cf))
    keys |= set(re.findall(r'\{"(\w+)",\s*"SNAP_CTL_\w+"\}', cf))
    if 'kSetPrefixCtl' in cf:
        keys |= set(re.findall(r'"(face_layout|swap_sticks|left_invert_y|right_invert_y|toggle_zl|toggle_zr|mouse_look|mouse_sensitivity|mouse_invert_x|mouse_invert_y|mouse_touch|lock_cursor)"', cf))
    if 'gfx_profile' in cf:
        keys.add('gfx_profile')
    return keys


def main():
    md = None
    if '--md' in sys.argv:
        md = sys.argv[sys.argv.index('--md') + 1]
    nat, f1, lau, cli = native_rows(), f1_rows(), launcher_rows(), cli_keys()
    keys = list(nat)
    for src in (lau, f1):
        keys += [k for k in src if k not in nat and k not in keys]
    problems, rows = [], []
    for k in keys:
        n, f, l = nat.get(k), f1.get(k), lau.get(k)
        label = (n or f or l)['label']
        has_cli = k in cli
        applies = (f or {}).get('when', '')
        rows.append((label, k, l['page'] if l else '', f['tab'] + (' / ' + f['section'] if f and f['section'] else '') if f else '',
                     n['tab'] if n else '', ('--set ' + k) if has_cli else '', applies))
        if n:
            if not n['help'].strip():
                problems.append('native row %s (%s) has no help line' % (n['label'], k))
            has_restart = 'restart' in n['help']
            if f and f['when'] == 'live' and has_restart:
                problems.append('native row %s (%s) says restart but the F1 table applies it live' % (n['label'], k))
            if f and f['when'] == 'restart' and not has_restart and k not in ('gfx_profile',):
                problems.append('native row %s (%s) does not say restart but the F1 table needs one' % (n['label'], k))
            if not f and n['store'] == 'opt':
                problems.append('native row %s (%s) is not in the F1 option table' % (n['label'], k))
        if not has_cli:
            problems.append('no --set key for %s (%s)' % (label, k))
    print('%d options: %d native, %d F1, %d launcher' % (len(keys), len(nat), len(f1), len(lau)))
    missing = [r for r in rows if not r[4]]
    print('no native row (%d): %s' % (len(missing), ', '.join(r[1] for r in missing)))
    for p in problems:
        print('PROBLEM:', p)
    if md:
        lines = ['# Option map', '',
                 'Every player option and where it lives: the launcher page, the F1 menu tab and section, the tab of the game\'s own Settings screen, '
                 'and the `--set` key ([command-line.md](command-line.md)). Generated by `python -I scripts/ui/option_audit.py --md docs/option-map.md`; '
                 'the script also fails when a native row has no help line, no `--set` key or the wrong restart note.', '',
                 '| Option | Key | Launcher | F1 menu | Game Settings tab | Command line | Applies |', '| --- | --- | --- | --- | --- | --- | --- |']
        for r in rows:
            lines.append('| %s | `%s` | %s | %s | %s | %s | %s |' % (r[0], r[1], r[2] or '-', r[3] or '-', r[4] or '-', ('`%s`' % r[5]) if r[5] else '-', r[6] or '-'))
        open(os.path.join(ROOT, md), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
        print('wrote', md)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
