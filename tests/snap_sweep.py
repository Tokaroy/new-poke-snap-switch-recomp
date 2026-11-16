"""Whole-set differential sweep of the lifter (dev tool, local only; see docs/lifter-diff.md).

Runs tests/snap_func.py over every function the test can run in isolation, in address-ordered slices, one slice process at a time at
BelowNormal. Each slice lifts and compiles its functions, runs the same random states in Unicorn (AArch64 emulator, GPL-2.0, dev venv only) and
in the compiled C, and compares registers and memory. Finished slices are skipped on a re-run, so the sweep resumes. The result is
analysis/recomp/diff_sweep/: summary.json, results.tsv (every function), mismatches.tsv/.md/.json and repro/<entry>.{json,s,c}.

  tools\\venv-recomp\\Scripts\\python -I tests\\snap_sweep.py [--size 400] [--slices 0:10] [--cases 8] [--budget-min 240] [--aggregate-only]

Nothing here is linked into the game or shipped; analysis/, generated/ and build/ are gitignored.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUTROOT = ROOT / 'analysis' / 'recomp'
SWEEP = OUTROOT / 'diff_sweep'
LOGS = ROOT / 'build' / 'w'


def slice_dir(i):
    return OUTROOT / f'diff_sw{i}'


def total_functions(py, extra_env):
    env = dict(os.environ, SNAP_DIFF_TAG='swcount', **extra_env)
    out = subprocess.run([py, '-I', str(HERE / 'snap_func.py'), '--sweep-count'], cwd=ROOT, env=env, capture_output=True, text=True).stdout
    for ln in out.splitlines():
        if ln.startswith('SWEEP_TOTAL'):
            return int(ln.split()[1])
    raise SystemExit('could not read SWEEP_TOTAL:\n' + out[-800:])


def run_slice(py, i, size, a):
    LOGS.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SNAP_DIFF_TAG=f'sw{i}')
    cmd = [py, '-I', str(HERE / 'snap_func.py'), '--sweep', f'{i}:{size}', '--cases', str(a.cases), '--retry-untested', str(a.retry),
           '--steps', str(a.steps), '--seed', str(a.seed), '--jobs', str(a.jobs), '--cflags', a.cflags]
    if a.no_minimize:
        cmd.append('--no-minimize')
    t0 = time.time()
    with open(LOGS / f'sweep_{i}.log', 'w', encoding='utf-8') as log:
        rc = subprocess.run(cmd, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    for d in (ROOT / 'generated' / f'subset_sw{i}', ROOT / 'build' / f'subset_sw{i}'):
        shutil.rmtree(d, ignore_errors=True)       # the emitted C and objects are not needed after the slice
    return rc, time.time() - t0


def read_tsv(p):
    lines = p.read_text(encoding='utf-8').splitlines()
    cols = lines[0].split('\t')
    return [dict(zip(cols, ln.split('\t'))) for ln in lines[1:] if ln]


def aggregate(nslices):
    SWEEP.mkdir(parents=True, exist_ok=True)
    (SWEEP / 'repro').mkdir(exist_ok=True)
    rows, mism, done, timing = [], [], [], {'unicorn_s': 0.0, 'host_s': 0.0, 'total_s': 0.0}
    for i in range(nslices):
        d = slice_dir(i)
        if not (d / 'diff_results.json').exists():
            continue
        done.append(i)
        rows += read_tsv(d / 'diff_results.tsv')
        t = json.loads((d / 'diff_results.json').read_text()).get('summary', {}).get('timing', {})
        for k in timing:
            timing[k] += t.get(k, 0)
        if (d / 'mismatches.json').exists():
            mism += json.loads((d / 'mismatches.json').read_text())
        if (d / 'repro').exists():
            for f in (d / 'repro').iterdir():
                shutil.copy2(f, SWEEP / 'repro' / f.name)
    status = {}
    cat = {}
    for r in rows:
        status[r['status']] = status.get(r['status'], 0) + 1
        c = cat.setdefault(r['category'], {})
        c[r['status']] = c.get(r['status'], 0) + 1
    why = {}
    for r in rows:
        if r['status'] == 'untested':
            k = (r['detail'].split(',')[0].split('x ', 1)[-1] if r['detail'] else 'no reason')[:70]
            why[k] = why.get(k, 0) + 1
    summary = {'slices_done': len(done), 'slices_total': nslices, 'functions': len(rows), 'status': status, 'by_category': cat,
               'cases_compared': sum(int(r['cases_run']) for r in rows), 'timing_s': {k: round(v, 1) for k, v in timing.items()},
               'untested_reasons': dict(sorted(why.items(), key=lambda kv: -kv[1])[:12])}
    (SWEEP / 'summary.json').write_text(json.dumps(summary, indent=1))
    if rows:
        cols = list(rows[0])
        with open(SWEEP / 'results.tsv', 'w', encoding='utf-8') as f:
            f.write('\t'.join(cols) + '\n' + '\n'.join('\t'.join(r[c] for c in cols) for r in rows) + '\n')
    mism.sort(key=lambda m: int(m['entry'], 16))
    (SWEEP / 'mismatches.json').write_text(json.dumps(mism, indent=1))
    with open(SWEEP / 'mismatches.tsv', 'w', encoding='utf-8') as f:
        f.write('entry\tend\tinsns\tcategory\tstatus\tminimized\tnonzero_registers\tfirst_difference\n')
        for m in mism:
            f.write('\t'.join([m['entry'], m['end'], str(m['insns']), m['category'], m['status'], str(m.get('minimized', False)),
                               str(m.get('nonzero_registers', '')), '; '.join(m.get('diff', [])[:2])]) + '\n')
    md = [f'# Lifter differential sweep: mismatches', '',
          f'{len(done)} of {nslices} slices done, {len(rows)} functions, status {status}, {summary["cases_compared"]} random states compared.', '']
    if not mism:
        md.append('No mismatching function so far.')
    for m in mism:
        md += [f'## {m["entry"]} ({m["insns"]} instructions, {m["category"]}) {m["status"]}', '']
        md += [f'- first differences: {"; ".join(m.get("diff", [])[:4])}']
        if m.get('case'):
            c = m['case']
            regs = ', '.join(f'x{k}={v}' for k, v in c['x'].items()) or 'all registers zero'
            md += [f'- minimal input: sp={c["sp"]}, {regs}' + (f', v{",".join(c["v"])} set' if c['v'] else '') + (f', {len(c["patches"])} memory patches' if c['patches'] else '')]
            md += [f'- files: `repro/{m["entry"]}.json` (input), `.s` (AArch64), `.c` (lifted C)']
        md += ['']
    (SWEEP / 'mismatches.md').write_text('\n'.join(md), encoding='utf-8')
    return summary, mism


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--size', type=int, default=400, help='functions per slice process')
    ap.add_argument('--slices', default='', metavar='A:B', help='only slices A..B-1')
    ap.add_argument('--cases', type=int, default=8)
    ap.add_argument('--retry', type=int, default=16)
    ap.add_argument('--steps', type=int, default=100000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--jobs', type=int, default=2, help='clang jobs inside a slice')
    ap.add_argument('--cflags', default='-march=x86-64-v3 -O2')
    ap.add_argument('--no-minimize', action='store_true')
    ap.add_argument('--budget-min', type=float, default=0, help='stop starting slices after this many minutes (0 = no limit)')
    ap.add_argument('--redo', action='store_true', help='re-run slices that already have results')
    ap.add_argument('--aggregate-only', action='store_true')
    a = ap.parse_args()
    py = sys.executable
    total = total_functions(py, {})
    nsl = (total + a.size - 1) // a.size
    lo, hi = (0, nsl) if not a.slices else tuple(int(t) for t in a.slices.split(':'))
    hi = min(hi, nsl)
    print(f'{total} functions, {nsl} slices of {a.size}; running {lo}..{hi - 1}', flush=True)
    t0 = time.time()
    if not a.aggregate_only:
        for i in range(lo, hi):
            if (slice_dir(i) / 'diff_results.json').exists() and not a.redo:
                continue
            if a.budget_min and (time.time() - t0) / 60 > a.budget_min:
                print(f'budget of {a.budget_min} min used; stopping before slice {i}', flush=True)
                break
            rc, dt = run_slice(py, i, a.size, a)
            ok = (slice_dir(i) / 'diff_results.json').exists()
            print(f'slice {i}/{nsl - 1}: {"ok" if ok else "NO RESULT"} rc={rc} {dt:.0f}s (log build/w/sweep_{i}.log)', flush=True)
            if i % 5 == 4:
                aggregate(nsl)
    summary, mism = aggregate(nsl)
    print(json.dumps(summary), flush=True)
    print(f'{len(mism)} mismatching functions; see {SWEEP}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
