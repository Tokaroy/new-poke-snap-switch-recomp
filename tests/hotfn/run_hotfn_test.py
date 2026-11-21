"""Builds and runs the hot-function differential test (tests/hotfn/hotfn_test.cpp; project code, ours).

Extracts the generated guest functions the test compares against from the generated main (game-derived, so they are
written only to build/hotfn/guest_fns.c, which is git-ignored), compiles them with the same flags as the main library
(-O2 -march=x86-64-v3 -fno-strict-aliasing; no PGO, which does not change FP results), compiles the test, and runs it.

Run: python -I tests/hotfn/run_hotfn_test.py [--cases N] [--capture FILE] [--gen DIR] [--elf FILE] [--root DIR]
--root is the checkout that holds generated/, analysis/ and tools/ (default: this checkout).
--hooked tests the hooked generated functions instead (build/hotfn/hooked_fns.c, written by
tests/recomp/hotfn_hook_test.py --emit build/hotfn) with host/mods/hotfn.cpp, once per mode: SNAP_HOT_*=1, check, 0.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1]
VCVARS = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
FUNCS = ['f_710096d390', 'f_710096d5f0', 'f_7100155600', 'f_71008b5f10']
BELOW_NORMAL = 0x4000


def vc_env():
    env = dict(os.environ)
    out = subprocess.run(f'cmd /c ""{VCVARS}" >nul && set"', capture_output=True, text=True, shell=True).stdout
    for line in out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k] = v
    return env


def extract(gen: Path, names):
    """C source of each `void <name>(Cpu *c) {` ... `}` body found in the generated chunks."""
    want, got = set(names), {}
    for f in sorted(gen.glob('main_*.c')):
        text = f.read_text(encoding='utf-8', errors='replace')
        for n in list(want):
            m = re.search(r'^void ' + n + r'\(Cpu \*c\) \{$', text, re.M)
            if not m:
                continue
            end = text.index('\n}\n', m.start()) + 3
            got[n] = text[m.start():end]
            want.discard(n)
        if not want:
            break
    if want:
        sys.exit(f'not found in {gen}: {sorted(want)}')
    return [got[n] for n in names]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', type=Path, default=SRC)
    ap.add_argument('--gen', type=Path)
    ap.add_argument('--elf', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--cases', type=int, default=20000)
    ap.add_argument('--capture', default='')
    ap.add_argument('--hooked', action='store_true')
    a = ap.parse_args()
    gen = a.gen or a.root / 'generated' / 'v262144' / 'main'
    elf = a.elf or a.root / 'analysis' / 'import' / 'v262144-main.elf'
    out = a.out or SRC / 'build' / 'hotfn'
    llvm = a.root / 'tools' / 'llvm' / 'bin'
    out.mkdir(parents=True, exist_ok=True)
    body = '\n'.join(extract(gen, FUNCS))
    decls = ''.join(sorted(set(f'void {n}(Cpu *c);\n' for n in re.findall(r'\b(f_[0-9a-f]{10})\(c\)', body))))
    (out / 'guest_fns.c').write_text('// GENERATED-GAME-CODE copied from the generated main for tests/hotfn. Do not commit.\n'
                                     '#include "cpu.h"\n#include "simd.h"\n'
                                     'volatile int g_fnprof;\nvoid fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) {}\n'
                                     'void rt_dispatch(Cpu *c) {}\n'   # the bone-loop regions end by dispatching: stop there
                                     + decls + body, encoding='utf-8')
    env = vc_env()
    env['PATH'] = str(llvm) + os.pathsep + env['PATH']
    common = ['-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-I', str(SRC / 'runtime')]
    run = lambda cmd: subprocess.run(cmd, env=env, check=True, creationflags=BELOW_NORMAL)
    run([str(llvm / 'clang.exe')] + common + ['-c', str(out / 'guest_fns.c'), '-o', str(out / 'guest_fns.o')])
    # the bone-loop regions and the hooked copies come from the generator (tests/recomp/hotfn_hook_test.py --emit)
    gen_py = SRC / 'recomp' / 'snap_gen.py'
    if not (out / 'regions.c').exists() or (out / 'regions.c').stat().st_mtime < gen_py.stat().st_mtime:
        py = a.root / 'tools' / 'venv-recomp' / 'Scripts' / 'python.exe'
        subprocess.run([str(py), '-I', str(SRC / 'tests' / 'recomp' / 'hotfn_hook_test.py'), '--emit', str(out)], check=True,
                       stdout=subprocess.DEVNULL, creationflags=BELOW_NORMAL)
    run([str(llvm / 'clang.exe')] + common + ['-c', str(out / 'regions.c'), '-o', str(out / 'regions.o')])
    exe = out / 'hotfn_test.exe'
    if not a.hooked:
        run([str(llvm / 'clang++.exe'), '-std=c++20'] + common + [str(HERE / 'hotfn_test.cpp'), str(out / 'guest_fns.o'),
                                                                 str(out / 'regions.o'), '-o', str(exe)])
        args = [str(exe), str(elf), str(a.cases)] + ([a.capture] if a.capture else [])
        sys.exit(subprocess.run(args, creationflags=BELOW_NORMAL).returncode)
    hooked = out / 'hooked_fns.c'
    if not hooked.exists():
        sys.exit(f'{hooked} is missing: run tests/recomp/hotfn_hook_test.py --emit {out}')
    exe = out / 'hotfn_test_hooked.exe'
    run([str(llvm / 'clang.exe')] + common + ['-c', str(hooked), '-o', str(out / 'hooked_fns.o')])
    run([str(llvm / 'clang++.exe'), '-std=c++20', '-DHOTFN_HOOKED'] + common + [str(HERE / 'hotfn_test.cpp'), str(SRC / 'host' / 'mods' / 'hotfn.cpp'),
         str(out / 'guest_fns.o'), str(out / 'regions.o'), str(out / 'hooked_fns.o'), '-o', str(exe)])
    rc = 0
    for mode in ('1', 'check', '0'):
        print(f'--- SNAP_HOT_*={mode}')
        env1 = dict(os.environ, SNAP_HOT_MATDECOMP=mode, SNAP_HOT_MATDECOMP_SCALED=mode, SNAP_HOT_REVERB=mode,
                    SNAP_HOT_HIER=mode, SNAP_HOT_BONE=mode)
        args = [str(exe), str(elf), str(a.cases)] + ([a.capture] if a.capture else [])
        rc |= subprocess.run(args, env=env1, creationflags=BELOW_NORMAL).returncode
    sys.exit(rc)


if __name__ == '__main__':
    main()
