"""Emits the game's Kraken decoder (main+0x7240 OodleLZ_Decompress and everything it calls directly) for tests/oodle
(project code, ours; the output is generated game code, so it goes under build/ and is never committed).

  tools/venv-recomp/Scripts/python -I tests/oodle/emit_oodle.py [--out build/oodle]

Writes two copies with renamed functions: oodle_ref.c (ref_<addr>, no hooks) and oodle_hooked.c (hk_<addr>, with the
SNAP_GEN_HOTFN hooks of recomp/snap_gen.py, so the Oodle routines call host/mods/hotfn.cpp). The libc imports the decoder
calls through the PLT (memcpy, memmove, memset) keep their f_ names; tests/oodle/oodle_test.cpp defines them.
Needs analysis/maps and the dumps like the generator does.
"""
import argparse
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'recomp'))
os.environ['SNAP_GEN_HOTFN'] = '1'
from snap_common import BASE, functions, load_module  # noqa: E402
import ehlsda  # noqa: E402
import snap_gen  # noqa: E402
from gen import Emitter  # noqa: E402

ENTRY = 0x7240
PLT = {0xa365c0: 'memset', 0xa36600: 'memmove', 0xa36610: 'memcpy'}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'oodle')
    a = ap.parse_args()
    mod = load_module(name='main')
    fl = functions()
    starts = {BASE + s for s, e, k, t in fl}
    ends = {BASE + s: BASE + e for s, e, k, t in fl}
    kw = dict(lsda=ehlsda.module_pads(mod), resume=True, setjmp_targets=set())
    hooks = set(snap_gen.HOTFN_INLINE)
    inline = {k: v for k, v in snap_gen.MAIN_INLINE_HOOKS.items() if k in hooks}
    branch = {k: v for k, v in snap_gen.MAIN_BRANCH_HOOKS.items() if k in snap_gen.HOTFN_BRANCH}
    todo, seen, bodies = [BASE + ENTRY], set(), {}
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        if f - BASE in PLT:
            continue
        out = []
        for name, ih, bh in (('ref', {}, {}), ('hk', inline, branch)):
            e = Emitter(mod, starts, inline_hooks=ih, branch_hooks=bh, **kw)
            lines, _ = e.function(f, ends[f])
            out.append('\n'.join(lines))
        bodies[f] = out
        for m in re.findall(r'\bf_([0-9a-f]{10})\(c\)', out[0] + out[1]):
            t = int(m, 16)
            if t not in seen:
                todo.append(t)
    own = {f'{f:x}' for f in bodies}
    a.out.mkdir(parents=True, exist_ok=True)
    head = '// GENERATED-GAME-CODE (tests/oodle copy of the Kraken decoder). Do not commit.\n#include "cpu.h"\n#include "simd.h"\n'
    for i, (name, fname) in enumerate((('ref', 'oodle_ref.c'), ('hk', 'oodle_hooked.c'))):
        ren = lambda t: re.sub(r'\bf_([0-9a-f]{10})\b', lambda m: f'{name}_{m.group(1)}' if m.group(1) in own else m.group(0), t)
        body = '\n'.join(ren(bodies[f][i]) for f in sorted(bodies))
        decls = ''.join(f'void {n}(Cpu *c);\n' for n in sorted(set(re.findall(r'\b((?:f|ref|hk)_[0-9a-f]{10})\(c\)', body))))
        (a.out / fname).write_text(head + decls + body + '\n', encoding='utf-8')
    hooked = sum(len(re.findall(r'rt_hot_oodle_\w+', bodies[f][1])) for f in bodies)
    print(f'{len(bodies)} functions -> {a.out} (oodle_ref.c, oodle_hooked.c); {hooked} hook references')


if __name__ == '__main__':
    main()
