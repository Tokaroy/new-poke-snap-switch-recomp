"""Incremental per-translation-unit build of the generated `main` C into a static library (project tooling, ours).

Usage: tools/venv-recomp/Scripts/python -I recomp/snap_build.py [--gen generated/v262144/main] [--out build/v262144]
                                                              [--jobs N] [--opt -O2] [--cflags ...] [--only GLOB] [--lib NAME]

A TU is rebuilt when its object is missing, older than the source, or the flag/runtime-header stamp changed. Every clang
child runs at BelowNormal priority with a 30 minute limit; its peak working set is sampled. Writes
<out>/obj/*.obj, <out>/lib/snap_main.lib, <out>/build_report.json (per-TU seconds, peak MB, sizes).
"""
import argparse
import concurrent.futures as cf
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from snap_common import MemSampler, lower_priority  # noqa: E402

# SNAP_LLVM_BIN (folder with clang.exe and llvm-ar.exe) and SNAP_RUNTIME_INCLUDE (folder with cpu.h, simd.h, simd_fp.h) let the builder
# (builder/) use its own toolchain and the shipped headers; unset = the dev tree.
LLVM_BIN = Path(os.environ.get('SNAP_LLVM_BIN') or ROOT / 'tools' / 'llvm' / 'bin')
CLANG = LLVM_BIN / 'clang.exe'
AR = LLVM_BIN / 'llvm-ar.exe'
RUNTIME = Path(os.environ.get('SNAP_RUNTIME_INCLUDE') or ROOT / 'runtime')
BELOW_NORMAL = 0x4000
TU_TIMEOUT = 30 * 60


class PMC(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD), ('PeakWorkingSetSize', ctypes.c_size_t),
                ('WorkingSetSize', ctypes.c_size_t), ('a', ctypes.c_size_t), ('b', ctypes.c_size_t),
                ('c', ctypes.c_size_t), ('d', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                ('PeakPagefileUsage', ctypes.c_size_t)]


def peak_of(pid):
    k, p = ctypes.windll.kernel32, ctypes.windll.psapi
    k.OpenProcess.restype = wintypes.HANDLE
    h = k.OpenProcess(0x0410, False, pid)   # QUERY_INFORMATION | VM_READ
    if not h:
        return 0
    pmc = PMC()
    pmc.cb = ctypes.sizeof(pmc)
    p.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
    ok = p.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
    k.CloseHandle(h)
    return pmc.PeakWorkingSetSize if ok else 0


def compile_one(src, obj, flags):
    t0 = time.perf_counter()
    p = subprocess.Popen([str(CLANG)] + flags + ['-c', str(src), '-o', str(obj)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, creationflags=BELOW_NORMAL)
    peak = 0
    import threading
    out = []
    th = threading.Thread(target=lambda: out.append(p.stdout.read()))
    th.start()
    try:
        while p.poll() is None:
            peak = max(peak, peak_of(p.pid))
            if time.perf_counter() - t0 > TU_TIMEOUT:
                subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
                break
            time.sleep(0.5)
    finally:
        th.join()
    peak = max(peak, peak_of(p.pid))
    return {'src': src.name, 'seconds': time.perf_counter() - t0, 'peak_mb': peak / 2**20, 'rc': p.returncode,
            'log': (out[0].decode(errors='replace')[-1500:] if out else ''), 'obj_bytes': obj.stat().st_size if obj.exists() else 0,
            'src_bytes': src.stat().st_size}


CACHE_LIMIT_GB = 3.0


def compile_cached(src, obj, flags, ident, cache_dir, use_cache):
    """compile_one behind the object cache (preprocessed-text key); the result carries 'cache': hit | miss | off."""
    if not use_cache:
        return dict(compile_one(src, obj, flags), cache='off')
    t0 = time.perf_counter()
    key = preprocess_key(src, flags, ident)
    if key is not None:
        hit = cache_dir / f'{key}.obj'
        if hit.exists():
            shutil.copyfile(hit, obj)
            os.utime(hit)                      # recently used: pruned last
            os.utime(obj)                      # newer than the source, so the next build sees it as current
            return {'src': src.name, 'seconds': time.perf_counter() - t0, 'peak_mb': 0, 'rc': 0, 'log': '', 'cache': 'hit',
                    'obj_bytes': obj.stat().st_size, 'src_bytes': src.stat().st_size}
    r = compile_one(src, obj, flags)
    r['cache'] = 'miss'
    if r['rc'] == 0 and key is not None and obj.exists():
        tmp = cache_dir / f'{key}.tmp{os.getpid()}'
        shutil.copyfile(obj, tmp)
        os.replace(tmp, cache_dir / f'{key}.obj')
    return r


def preprocess_key(src, flags, ident):
    """Content key of a TU: the preprocessed text (comments and unused whitespace gone) plus every flag, the compiler identity and the
    profile data (already folded into `ident`). Equal key, equal object code."""
    p = subprocess.run([str(CLANG)] + flags + ['-E', '-P', str(src)], capture_output=True, creationflags=BELOW_NORMAL)
    if p.returncode != 0:
        return None
    return hashlib.sha256(ident.encode() + b'\0' + p.stdout).hexdigest()


def prune_cache(cache_dir, limit_gb):
    files = sorted(cache_dir.glob('*.obj'), key=lambda f: f.stat().st_mtime)
    total = sum(f.stat().st_size for f in files)
    for f in files:
        if total <= limit_gb * 2**30:
            break
        total -= f.stat().st_size
        f.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--gen', type=Path, default=ROOT / 'generated' / 'v262144' / 'main')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'v262144')
    ap.add_argument('--jobs', type=int, default=16)
    ap.add_argument('--opt', default='-O2')
    ap.add_argument('--cflags', default='-march=x86-64-v3')
    ap.add_argument('--only', default='*.c')
    ap.add_argument('--lib', default='snap_main')
    ap.add_argument('--no-lib', action='store_true')
    # PGO (2026-11-13, host/README.md "PGO and ThinLTO"): 'auto' optimizes with build/pgo/snap.profdata when it exists (local,
    # trained on a Florio course run); 'off' = the plain -O2 build. Skipped when --cflags already has a -fprofile flag.
    ap.add_argument('--pgo', choices=('auto', 'off'), default='auto')
    # Object cache (2026-11-15): a stale TU is first preprocessed (0.2 s) and looked up by content in build/objcache; a hit copies the object
    # (comment-only header edit, a reverted experiment, a flag set built before). A miss compiles and stores. --no-cache turns it off.
    ap.add_argument('--no-cache', action='store_true')
    ap.add_argument('--cache-dir', type=Path, default=ROOT / 'build' / 'objcache')
    args = ap.parse_args()
    lower_priority()
    obj_dir, lib_dir = args.out / 'obj', args.out / 'lib'
    obj_dir.mkdir(parents=True, exist_ok=True)
    lib_dir.mkdir(parents=True, exist_ok=True)
    flags = [args.opt, '-fno-strict-aliasing', '-w', '-I', str(RUNTIME), '-D_CRT_SECURE_NO_WARNINGS'] + args.cflags.split()
    prof = ROOT / 'build' / 'pgo' / 'snap.profdata'
    prof_hash = ''
    if args.pgo == 'auto' and prof.exists() and '-fprofile' not in args.cflags:
        flags.append('-fprofile-use=' + str(prof))
        prof_hash = hashlib.sha256(prof.read_bytes()).hexdigest()   # a new profile rebuilds everything
    stamp = hashlib.sha256((' '.join(flags) + prof_hash + ''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(RUNTIME.glob('*.h')))).encode()).hexdigest()
    stamp_f = args.out / 'flags.stamp'
    stale_all = not stamp_f.exists() or stamp_f.read_text() != stamp
    srcs = sorted(args.gen.glob(args.only))
    todo = []
    for s in srcs:
        o = obj_dir / (s.stem + '.obj')
        if stale_all or not o.exists() or o.stat().st_mtime < s.stat().st_mtime:
            todo.append((s, o))
    print(f'{len(srcs)} TUs, {len(todo)} to build ({"flags/runtime header changed" if stale_all else "incremental"}), '
          f'{args.jobs} jobs, {" ".join(flags)}', flush=True)
    sampler = MemSampler().__enter__()
    t0 = time.perf_counter()
    results, failed = [], []
    # Longest first, by the seconds each TU took the last time it was compiled (tu_cost.json), else by size: a 1.2 MB TU can take 88 s and a
    # 1.5 MB one 20 s, so size alone leaves the slowest TUs for the tail of the build.
    cost_f = args.out / 'tu_cost.json'
    cost = json.loads(cost_f.read_text()) if cost_f.exists() else {}
    sec_per_byte = (sum(cost.values()) / max(1, sum((args.gen / n).stat().st_size for n in cost if (args.gen / n).exists()))) if cost else 0
    todo.sort(key=lambda t: -cost.get(t[0].name, sec_per_byte * t[0].stat().st_size or t[0].stat().st_size))
    ident = ''
    if not args.no_cache:
        args.cache_dir.mkdir(parents=True, exist_ok=True)
        st = CLANG.stat()
        ident = f'{CLANG.name} {st.st_size} {int(st.st_mtime)} {prof_hash} ' + ' '.join(flags)
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        futs = [ex.submit(compile_cached, s, o, flags, ident, args.cache_dir, not args.no_cache) for s, o in todo]
        for i, f in enumerate(cf.as_completed(futs)):
            r = f.result()
            results.append(r)
            if r['rc'] != 0:
                failed.append(r)
                print(f'FAILED {r["src"]} rc={r["rc"]}\n{r["log"]}', flush=True)
            if (i + 1) % 20 == 0 or i + 1 == len(futs):
                print(f'compiled {i + 1}/{len(futs)} TUs, {time.perf_counter() - t0:.0f}s', flush=True)
    wall = time.perf_counter() - t0
    sampler.__exit__()
    for r in results:
        if r['rc'] == 0 and r.get('cache') != 'hit':
            cost[r['src']] = round(r['seconds'], 1)
    cost_f.write_text(json.dumps(cost, indent=0))
    if not args.no_cache:
        prune_cache(args.cache_dir, CACHE_LIMIT_GB)
    if failed:
        return 1
    stamp_f.write_text(stamp)
    objs = sorted(obj_dir.glob('*.obj'))
    lib = lib_dir / f'{args.lib}.lib'
    t1 = time.perf_counter()
    if todo and not args.no_lib or not lib.exists():
        lib.unlink(missing_ok=True)
        subprocess.check_call([str(AR), 'rcs', str(lib)] + [str(o) for o in objs], creationflags=BELOW_NORMAL)
    t_ar = time.perf_counter() - t1
    hits = sum(1 for r in results if r.get('cache') == 'hit')
    rep = {'tus_total': len(srcs), 'tus_built': len(todo) - hits, 'cache_hits': hits, 'wall_s': round(wall, 1), 'cpu_s': round(sum(r['seconds'] for r in results), 1),
           'ar_s': round(t_ar, 1), 'jobs': args.jobs, 'system_peak_used_mb': sampler.peak_used_mb, 'flags': flags,
           'src_mb': round(sum(s.stat().st_size for s in srcs) / 1e6, 1),
           'obj_mb': round(sum(o.stat().st_size for o in objs) / 1e6, 1),
           'lib_mb': round(lib.stat().st_size / 1e6, 1) if lib.exists() else 0,
           'max_tu_peak_mb': round(max((r['peak_mb'] for r in results), default=0)),
           'slowest10': [{k: (round(v, 1) if isinstance(v, float) else v) for k, v in r.items() if k != 'log'}
                         for r in sorted(results, key=lambda r: -r['seconds'])[:10]],
           'per_tu': [{k: (round(v, 1) if isinstance(v, float) else v) for k, v in r.items() if k != 'log'} for r in sorted(results, key=lambda r: r['src'])]}
    (args.out / 'build_report.json').write_text(json.dumps(rep, indent=1))
    print(json.dumps({k: v for k, v in rep.items() if k != 'per_tu'}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
