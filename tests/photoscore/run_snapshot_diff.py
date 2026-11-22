"""Builds and runs the snapshot differential test (tests/photoscore/snapshot_diff.cpp; project code, ours).

Compares lifted guest functions with a second implementation of them (a readable rewrite) on real inputs recorded in game by
host/mods/pscap.cpp (SNAP_PSCAP). The lifted functions and everything they call are copied out of the generated main into
<out>/guest_fns.c (game-derived, so only under build/, which is git-ignored). The rewrite is passed with --readable and must define
`extern "C" GuestFn readable_fn(uint64_t main_off)`; a rewrite of game code is game-derived too and stays outside the repository.

Run: python -I tests/photoscore/run_snapshot_diff.py --capture <file.psc> --fns 422190,421480 --readable <a.cpp> [<b.cpp> ...]
     [--ret 40e740:s0,421480:w0] [--csv <file>] [--data-sym <name>=<float>] [--root <checkout with generated/, analysis/, tools/>]
     [--out <dir>]
Allocations the lifted code makes (the guest allocator, main+0x990e70) come from a scratch arena; frees are ignored.
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
BELOW_NORMAL = 0x4000
PLT_LO, PLT_HI = 0x7100a365b0, 0x7100a38e40
# guest functions replaced by the harness instead of copied: the allocator pair
REPLACED = {'f_7100990e70', 'f_7100994840'}

HARNESS_C = r'''
// Harness glue for the copied guest functions (project code, ours).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) { (void)a; (void)lr; (void)x0; }
void rt_dispatch(Cpu *c) { fprintf(stderr, "rt_dispatch to %#llx: not supported in the test\n", (unsigned long long)c->pc); *(volatile int *)0 = 0; }
void rt_import_call(Cpu *c, uint32_t idx) { fprintf(stderr, "import #%u has no test stub\n", idx); (void)c; *(volatile int *)0 = 0; }
void rt_unresolved(Cpu *c, uint64_t addr, const char *what) { fprintf(stderr, "unresolved %#llx %s\n", (unsigned long long)addr, what); (void)c; *(volatile int *)0 = 0; }
uint64_t snapshot_diff_alloc(uint64_t n);
void f_7100990e70(Cpu *c) { c->x[0] = snapshot_diff_alloc(c->x[0]); }   // guest operator new
void f_7100994840(Cpu *c) { (void)c; }                                    // guest operator delete
// libc++ __shared_weak_count::lock: owners (count - 1) at +8; -1 = expired
void imp__ZNSt3__119__shared_weak_count4lockEv(Cpu *c) {
    int64_t *owners = (int64_t *)(uintptr_t)(c->x[0] + 8);
    if (*owners == -1) c->x[0] = 0; else (*owners)++;
}
void imp__ZNSt3__119__shared_weak_count14__release_weakEv(Cpu *c) { (void)c; }
void imp_memcpy(Cpu *c) { memcpy((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], c->x[2]); }
void imp_memmove(Cpu *c) { memmove((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], c->x[2]); }
void imp_memset(Cpu *c) { memset((void *)(uintptr_t)c->x[0], (int)c->x[1], c->x[2]); }
void imp_sqrtf(Cpu *c) { float f; memcpy(&f, &c->v[0], 4); f = sqrtf(f); c->v[0] = 0; memcpy(&c->v[0], &f, 4); }
void imp_acosf(Cpu *c) { float f; memcpy(&f, &c->v[0], 4); f = acosf(f); c->v[0] = 0; memcpy(&c->v[0], &f, 4); }
'''


def vc_env():
    env = dict(os.environ)
    out = subprocess.run(f'cmd /c ""{VCVARS}" >nul && set"', capture_output=True, text=True, shell=True).stdout
    for line in out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k] = v
    return env


def index_generated(gen: Path):
    """name -> source text of every lifted function in the generated main (multi-line bodies end at a line '}')."""
    bodies = {}
    for f in sorted(gen.glob('main_*.c')):
        text = f.read_text(encoding='utf-8', errors='replace')
        for m in re.finditer(r'^void (f_[0-9a-f]{10})\(Cpu \*c\) \{$', text, re.M):
            end = text.index('\n}\n', m.start()) + 3
            bodies[m.group(1)] = text[m.start():end]
    return bodies


def closure(roots, bodies):
    want, seen, order = list(roots), set(), []
    while want:
        n = want.pop()
        if n in seen or n in REPLACED:
            continue
        seen.add(n)
        addr = int(n[2:], 16)
        if PLT_LO <= addr < PLT_HI:
            continue   # PLT stubs come from plt_stubs.c
        if n not in bodies:
            sys.exit(f'{n} not found in the generated main')
        order.append(n)
        want += re.findall(r'\b(f_[0-9a-f]{10})\(c\)', bodies[n])
    return order, seen


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--capture', required=True)
    ap.add_argument('--fns', required=True, help='main offsets (hex) of the functions under test')
    ap.add_argument('--readable', nargs='+', required=True)
    ap.add_argument('--ret', default='')
    ap.add_argument('--csv', default='')
    ap.add_argument('--verbose', default='5')
    ap.add_argument('--data-sym', action='append', default=[], help='<imported data symbol>=<float> (SDK constants the code reads)')
    ap.add_argument('--root', type=Path, default=SRC)
    ap.add_argument('--out', type=Path, default=SRC / 'build' / 'photoscore')
    a = ap.parse_args()
    gen = a.root / 'generated' / 'v262144' / 'main'
    elf = a.root / 'analysis' / 'import' / 'v262144-main.elf'
    llvm = a.root / 'tools' / 'llvm' / 'bin'
    a.out.mkdir(parents=True, exist_ok=True)
    fns = [int(x, 16) for x in a.fns.split(',')]
    roots = [f'f_{0x7100000000 + f:010x}' for f in fns]
    bodies = index_generated(gen)
    order, seen = closure(roots, bodies)
    decls = ''.join(f'void {n}(Cpu *c);\n' for n in sorted(seen | REPLACED))
    table = 'GuestFnPtr lifted_fn(uint64_t off) {\n' + ''.join(
        f'    if (off == {f:#x}) return {r};\n' for f, r in zip(fns, roots)) + '    return 0;\n}\n'
    (a.out / 'guest_fns.c').write_text(
        '// GENERATED-GAME-CODE copied from the generated main for tests/photoscore. Do not commit.\n'
        '#include "cpu.h"\n#include "simd.h"\ntypedef void (*GuestFnPtr)(Cpu *);\n' + decls + HARNESS_C + '\n'.join(bodies[n] for n in order) + table,
        encoding='utf-8')
    print(f'{len(order)} lifted functions copied for {len(fns)} roots')
    env = vc_env()
    env['PATH'] = str(llvm) + os.pathsep + env['PATH']
    common = ['-O2', '-march=x86-64-v3', '-ffp-contract=off', '-fno-strict-aliasing', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-I', str(SRC / 'runtime')]
    run = lambda cmd: subprocess.run(cmd, env=env, check=True, creationflags=BELOW_NORMAL)
    objs = []
    for src, name in ((a.out / 'guest_fns.c', 'guest_fns.o'), (gen / 'plt_stubs.c', 'plt_stubs.o'), (SRC / 'runtime' / 'fma.c', 'fma.o')):
        run([str(llvm / 'clang.exe')] + common + ['-c', str(src), '-o', str(a.out / name)])
        objs.append(str(a.out / name))
    exe = a.out / 'snapshot_diff.exe'
    run([str(llvm / 'clang++.exe'), '-std=c++20'] + common + [str(HERE / 'snapshot_diff.cpp')] + a.readable + objs + ['-o', str(exe)])
    args = [str(exe), str(elf), a.capture, '--only', a.fns, '--verbose', a.verbose]
    if a.ret:
        args += ['--ret', a.ret]
    if a.csv:
        args += ['--csv', a.csv]
    for d in a.data_sym:
        args += ['--data-sym', d]
    sys.exit(subprocess.run(args, creationflags=BELOW_NORMAL).returncode)


if __name__ == '__main__':
    main()
