"""Release gate (developer tool, not shipped): run host/ci_check.ps1 -Mode release before a release step.

The release scripts call `add_args(ap)` and then `gate(a, '<step>')` right after parsing their arguments:

  python -I builder/release_prebuilt.py                          # the gate runs first; a failure stops the step with one reason line
  python -I builder/release_prebuilt.py --skip-ci-check "why"    # override: a reason is required, logged to build/release/ci_override.log

A pass is remembered in build/release/ci_pass.json for the exact tree it checked (HEAD plus the content of every changed file), so the three
release steps of one release run the checks once. Any edit, new commit or an older-than-2-hours pass runs them again.
"""
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PASS_FILE = ROOT / 'build' / 'release' / 'ci_pass.json'
OVERRIDE_LOG = ROOT / 'build' / 'release' / 'ci_override.log'
MAX_AGE_S = 2 * 3600


def add_args(ap):
    ap.add_argument('--skip-ci-check', metavar='REASON', default=None,
                    help='skip the host/ci_check.ps1 release gate; the reason is required and is appended to build/release/ci_override.log')


def _git(*a):
    return subprocess.run(['git', '-C', str(ROOT)] + list(a), capture_output=True).stdout


def tree_fingerprint():
    """HEAD plus the content of every file that differs from it (changed, staged or new and not ignored)."""
    h = hashlib.sha256(_git('rev-parse', 'HEAD'))
    names = set(x for x in _git('diff', '--name-only', '-z', 'HEAD').decode('utf-8', 'replace').split('\0') if x)
    names |= set(x for x in _git('ls-files', '-o', '--exclude-standard', '-z').decode('utf-8', 'replace').split('\0') if x)
    for n in sorted(names):
        p = ROOT / n
        h.update(n.encode('utf-8') + b'\0')
        if p.is_file():
            h.update(hashlib.sha256(p.read_bytes()).digest())
    return h.hexdigest()


def _passed_recently(fp):
    try:
        d = json.loads(PASS_FILE.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if d.get('fingerprint') == fp and time.time() - d.get('time', 0) < MAX_AGE_S:
        return d
    return None


def gate(a, step):
    """Run the gate for release step `step`; exits the process (code 4) when a check fails or cannot run."""
    reason = getattr(a, 'skip_ci_check', None)
    if reason is not None:
        reason = reason.strip()
        if not reason:
            raise SystemExit('--skip-ci-check needs a reason (it is logged): --skip-ci-check "why"')
        OVERRIDE_LOG.parent.mkdir(parents=True, exist_ok=True)
        head = _git('rev-parse', '--short', 'HEAD').decode().strip()
        with OVERRIDE_LOG.open('a', encoding='utf-8') as f:
            f.write(f'{time.strftime("%Y-%m-%d %H:%M:%S")}\t{step}\tHEAD {head}\t{reason}\n')
        print(f'!!! CI GATE OVERRIDDEN for {step}: "{reason}" (logged to {OVERRIDE_LOG}) !!!', flush=True)
        return
    fp = tree_fingerprint()
    prev = _passed_recently(fp)
    if prev:
        print(f'[ci gate] {step}: host/ci_check.ps1 -Mode release already passed for this exact tree '
              f'({int(time.time() - prev["time"])} s ago), not repeated', flush=True)
        return
    print(f'[ci gate] {step}: running host/ci_check.ps1 -Mode release (several minutes; --skip-ci-check "reason" to override)', flush=True)
    t0 = time.time()
    p = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(ROOT / 'host' / 'ci_check.ps1'), '-Mode', 'release'],
                       cwd=str(ROOT), capture_output=True)
    out = p.stdout.decode('utf-8', 'replace').splitlines()
    for ln in out:
        if ln.startswith('[ci_check]'):
            print(ln, flush=True)
    last = next((ln for ln in reversed(out) if ln.startswith('CI CHECK ')), f'CI CHECK FAILED: no result line (exit {p.returncode})')
    if p.returncode == 0 and last.startswith('CI CHECK PASSED'):
        PASS_FILE.parent.mkdir(parents=True, exist_ok=True)
        PASS_FILE.write_text(json.dumps({'fingerprint': tree_fingerprint(), 'time': time.time(), 'step': step, 'seconds': round(time.time() - t0)}),
                             encoding='utf-8')
        print(f'[ci gate] {last}', flush=True)
        return
    print(f'{step}: {last}', file=sys.stderr, flush=True)
    print('fix it, or override with --skip-ci-check "reason" (logged)', file=sys.stderr, flush=True)
    raise SystemExit(4)
