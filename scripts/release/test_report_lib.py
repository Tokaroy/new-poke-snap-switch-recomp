"""Helpers for scripts/release/test_report.ps1 (project code, ours; no game data).

  python -I test_report_lib.py perf    <frames.csv> <boot_stderr.log> <start_wait> <end_wait>   -> JSON on stdout
  python -I test_report_lib.py waits   <boot_stderr.log>                                         -> JSON on stdout
  python -I test_report_lib.py hle     <hle_tests output> <allowlist,comma>                      -> JSON on stdout
  python -I test_report_lib.py render  <data.json> <report.md> <report.json>                     -> writes both, scrubbed
  python -I test_report_lib.py scrub   <file>...                                                 -> scrubs files in place

The report stays local: it holds game frames. scrub() removes user names, user paths and the machine name from text.
"""
import csv
import json
import os
import re
import sys
from pathlib import Path

WAIT_RE = re.compile(r"input script: wait '([^']+)' met after ([\d.]+) s at present (\d+)")


def scrub_text(text, extra=()):
    """Replace user profile paths, the account name, the machine name and any `extra` strings (the checkout path)."""
    for e in sorted({x for x in extra if x and len(x) > 3}, key=len, reverse=True):
        text = re.sub(re.escape(e), '<repo>', text, flags=re.I)
        text = re.sub(re.escape(e.replace('\\', '/')), '<repo>', text, flags=re.I)
    text = re.sub(r'[A-Za-z]:[\\/]+Users[\\/]+[^\\/\s"\'`<>|:*?]+', r'<user-profile>', text, flags=re.I)
    for var in ('USERNAME', 'USERPROFILE', 'COMPUTERNAME', 'HOMEPATH'):
        v = os.environ.get(var, '')
        if len(v) > 2 and var != 'HOMEPATH':
            text = re.sub(re.escape(v), '<' + var.lower() + '>' if var != 'USERPROFILE' else '<user-profile>', text, flags=re.I)
    return text


def parse_waits(path):
    out = []
    timeout = None
    ended = False
    for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        m = WAIT_RE.search(line)
        if m:
            out.append({'cond': m.group(1), 'after_s': float(m.group(2)), 'present': int(m.group(3))})
            continue
        if 'input script: TIMEOUT' in line:
            timeout = re.sub(r'^\[[^\]]*\]\s*', '', line)[:200]
        if 'input script: end of route reached' in line:
            ended = True
    return {'waits': out, 'timeout': timeout, 'route_end': ended}


def pct_low(dts, frac):
    s = sorted(dts, reverse=True)
    k = max(1, int(len(s) * frac))
    mean = sum(s[:k]) / k
    return 1000.0 / mean if mean > 0 else 0.0


def perf_stats(frames_csv, start_present, end_present):
    rows = list(csv.DictReader(open(frames_csv, encoding='utf-8')))
    f = [(int(r['frame']), float(r['dt_ms'])) for r in rows]
    # the first present carries the whole start-up cost; the window is the ride itself
    win = [dt for (n, dt) in f if start_present <= n <= end_present]
    if not win:
        return None
    tot = sum(win)
    srt = sorted(win)
    return {
        'frames': len(win), 'seconds': round(tot / 1000.0, 1), 'avg_fps': round(1000.0 * len(win) / tot, 1),
        'low1_fps': round(pct_low(win, 0.01), 1), 'low01_fps': round(pct_low(win, 0.001), 1),
        'p50_ms': round(srt[len(srt) // 2], 2), 'p99_ms': round(srt[min(len(srt) - 1, int(len(srt) * 0.99))], 2),
        'max_ms': round(srt[-1], 1), 'over_33ms': sum(1 for d in win if d > 33.4),
    }


def cmd_perf(argv):
    frames, log, a, b = argv
    w = {x['cond']: x for x in parse_waits(log)['waits']}
    if a not in w or b not in w:
        print(json.dumps({'error': 'ride window not found in the log (%s / %s)' % (a, b)}))
        return
    s = perf_stats(frames, w[a]['present'], w[b]['present'])
    print(json.dumps(s if s else {'error': 'no frames in the ride window'}))


def cmd_hle(argv):
    text = Path(argv[0]).read_text(encoding='utf-8', errors='replace')
    allow = {x for x in argv[1].split(',') if x}
    names = re.findall(r'^(\S+)\s+(PASS|FAIL) \((\d+) checks\)', text, re.M)
    failed = [n for (n, r, c) in names if r == 'FAIL']
    summ = re.search(r'^(\d+) checks, (\d+) failed', text, re.M)
    print(json.dumps({'tests': len(names), 'failed': failed, 'allowed_failures': [n for n in failed if n in allow],
                      'unexpected_failures': [n for n in failed if n not in allow],
                      'checks': int(summ.group(1)) if summ else None, 'failed_checks': int(summ.group(2)) if summ else None}))


def md_cell(x):
    return str(x).replace('|', '/')


def render(data, md_path, json_path):
    steps = data['steps']
    fails = [s for s in steps if s['status'] == 'FAIL']
    skips = [s for s in steps if s['status'] == 'SKIP']
    overall = 'FAIL' if fails else 'PASS'
    L = []
    L.append('# Release test report')
    L.append('')
    L.append('**Overall: %s** (%d passed, %d failed, %d skipped, %d known-failure allowances used)' % (
        overall, sum(1 for s in steps if s['status'] == 'PASS'), len(fails), len(skips), len(data.get('allowed', []))))
    L.append('')
    L.append('Local only: this folder holds game frames. Do not commit, upload or attach it anywhere.')
    L.append('')
    L.append('Fps and load times are only comparable between reports taken on an idle PC; other jobs on the machine lower them (one benchmark ride on a shared PC fell to half its idle fps).')
    L.append('')
    L.append('| Step | Result | Time | Detail |')
    L.append('|---|---|---|---|')
    for s in steps:
        L.append('| %s | %s | %ss | %s |' % (md_cell(s['name']), s['status'], s.get('seconds', 0), md_cell(s.get('detail', ''))))
    L.append('')
    L.append('## Build and machine')
    L.append('')
    for k, v in data['info'].items():
        L.append('- %s: %s' % (k, v))
    L.append('')
    for note in data.get('notes', []):
        L.append('- Note: ' + note)
    if data.get('notes'):
        L.append('')
    for r in data.get('rides', []):
        L.append('## Ride: %s' % r['route'])
        L.append('')
        L.append('- Result: %s; route end reached: %s; exit code %s%s' % (r['status'], r.get('route_end'), r.get('exit'), ('; ' + r['timeout']) if r.get('timeout') else ''))
        if r.get('perf'):
            p = r['perf']
            L.append('- Fps over the ride (%s to %s): average %s, 1%% low %s, 0.1%% low %s (%d frames, %s s, p99 %s ms, worst %s ms, %d frames over 33 ms)' % (
                r['window'][0], r['window'][1], p['avg_fps'], p['low1_fps'], p['low01_fps'], p['frames'], p['seconds'], p['p99_ms'], p['max_ms'], p['over_33ms']))
        elif r.get('perf_error'):
            L.append('- Fps: not available (%s)' % r['perf_error'])
        if r.get('score'):
            L.append('- Score screen: %s' % r['score'])
        L.append('')
        if r.get('waits'):
            L.append('Transitions (game time at each screen; includes the script\'s own key presses, so these are upper bounds on the load itself):')
            L.append('')
            L.append('| Transition | Seconds |')
            L.append('|---|---|')
            for w in r['waits']:
                L.append('| %s | %s |' % (md_cell(w['label']), w['after_s']))
            L.append('')
        for im in r.get('images', []):
            L.append('![%s](%s)' % (im['alt'], im['path']))
        if r.get('images'):
            L.append('')
    if data.get('ab'):
        L.append('## A/B captures (default settings vs Enhanced lighting)')
        L.append('')
        L.append('| Point | Result | Detail |')
        L.append('|---|---|---|')
        for a in data['ab']:
            L.append('| %s | %s | %s |' % (md_cell(a['name']), a['status'], md_cell(a.get('detail', ''))))
        L.append('')
        for a in data['ab']:
            for im in a.get('images', []):
                L.append('![%s](%s)' % (im['alt'], im['path']))
        L.append('')
    if data.get('launcher'):
        L.append('## Launcher screenshots')
        L.append('')
        for im in data['launcher']:
            L.append('![%s](%s)' % (im['alt'], im['path']))
        L.append('')
    if data.get('not_run'):
        L.append('## Not covered by this report')
        L.append('')
        for n in data['not_run']:
            L.append('- ' + n)
        L.append('')
    extra = data.get('scrub', [])
    Path(md_path).write_text(scrub_text('\n'.join(L), extra), encoding='utf-8', newline='\n')
    small = {'overall': overall, 'commit': data['info'].get('Commit'), 'quick': data.get('quick'), 'steps': [
        {'name': s['name'], 'status': s['status'], 'seconds': s.get('seconds'), 'detail': s.get('detail', '')} for s in steps],
        'rides': [{'route': r['route'], 'status': r['status'], 'perf': r.get('perf'), 'transitions': r.get('waits'), 'score': r.get('score')} for r in data.get('rides', [])],
        'ab': [{'name': a['name'], 'status': a['status']} for a in data.get('ab', [])], 'info': data['info']}
    Path(json_path).write_text(scrub_text(json.dumps(small, indent=2), extra), encoding='utf-8', newline='\n')
    return overall


def main():
    c, a = sys.argv[1], sys.argv[2:]
    if c == 'perf':
        cmd_perf(a)
    elif c == 'waits':
        print(json.dumps(parse_waits(a[0])))
    elif c == 'hle':
        cmd_hle(a)
    elif c == 'render':
        data = json.load(open(a[0], encoding='utf-8-sig'))
        print(render(data, a[1], a[2]))
    elif c == 'scrub':
        for f in a:
            p = Path(f)
            p.write_text(scrub_text(p.read_text(encoding='utf-8', errors='replace')), encoding='utf-8', newline='\n')
    else:
        sys.exit(2)


if __name__ == '__main__':
    main()
