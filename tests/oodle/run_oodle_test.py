"""Builds and runs the Kraken decode test for the Oodle hot routines (tests/oodle/oodle_test.cpp; project code, ours).

  python -I tests/oodle/run_oodle_test.py [--root CHECKOUT] [--modes 1,check,0] [--repeat N]

--root is the checkout that holds analysis/, extracted/ and tools/ (default: this checkout). Steps: the corpus
(build/oodle/corpus.bin, tests/oodle/make_corpus.py, once), the decoder copies (tests/oodle/emit_oodle.py, when missing or
older than recomp/snap_gen.py), then one compile (-O2 -march=x86-64-v3 like the main library) and one run per mode.
Everything game-derived stays under build/oodle.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1]
VCVARS = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
BELOW_NORMAL = 0x4000


def vc_env():
    env = dict(os.environ)
    out = subprocess.run(f'cmd /c ""{VCVARS}" >nul && set"', capture_output=True, text=True, shell=True).stdout
    for line in out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k] = v
    return env


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', type=Path, default=SRC)
    ap.add_argument('--modes', default='1,check,0')
    ap.add_argument('--repeat', type=int, default=1)
    a = ap.parse_args()
    out = SRC / 'build' / 'oodle'
    out.mkdir(parents=True, exist_ok=True)
    llvm = a.root / 'tools' / 'llvm' / 'bin'
    corpus = out / 'corpus.bin'
    if not corpus.exists():
        py = a.root / 'tools' / 'venv-oodle' / 'Scripts' / 'python.exe'
        subprocess.run([str(py), '-I', str(HERE / 'make_corpus.py'), '--root', str(a.root), '--out', str(corpus)], check=True,
                       creationflags=BELOW_NORMAL)
    gen_py = SRC / 'recomp' / 'snap_gen.py'
    ref = out / 'oodle_ref.c'
    if not ref.exists() or ref.stat().st_mtime < gen_py.stat().st_mtime:
        py = a.root / 'tools' / 'venv-recomp' / 'Scripts' / 'python.exe'
        subprocess.run([str(py), '-I', str(HERE / 'emit_oodle.py'), '--out', str(out)], check=True, creationflags=BELOW_NORMAL)
    env = vc_env()
    env['PATH'] = str(llvm) + os.pathsep + env['PATH']
    common = ['-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-I', str(SRC / 'runtime')]
    run = lambda cmd: subprocess.run(cmd, env=env, check=True, creationflags=BELOW_NORMAL)
    for name in ('oodle_ref', 'oodle_hooked'):
        c, o = out / f'{name}.c', out / f'{name}.o'
        if not o.exists() or o.stat().st_mtime < c.stat().st_mtime:
            run([str(llvm / 'clang.exe')] + common + ['-c', str(c), '-o', str(o)])
    exe = out / 'oodle_test.exe'
    run([str(llvm / 'clang++.exe'), '-std=c++20'] + common + [str(HERE / 'oodle_test.cpp'), str(SRC / 'host' / 'mods' / 'hotfn.cpp'),
         str(out / 'oodle_ref.o'), str(out / 'oodle_hooked.o'), '-o', str(exe)])
    elf = a.root / 'analysis' / 'import' / 'v262144-main.elf'
    rel = a.root / 'analysis' / 'maps' / 'relocations.tsv'
    rc = 0
    for mode in a.modes.split(','):
        env1 = dict(os.environ, SNAP_HOT_OODLE=mode)
        rc |= subprocess.run([str(exe), str(elf), str(rel), str(corpus), str(a.repeat)], env=env1, creationflags=BELOW_NORMAL).returncode
    sys.exit(rc)


if __name__ == '__main__':
    main()
