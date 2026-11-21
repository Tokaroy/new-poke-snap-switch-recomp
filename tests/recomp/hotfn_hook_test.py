"""Generator test for the hot-function hooks (recomp/snap_gen.py HOTFN_*, host/mods/hotfn.cpp; project code, ours).

  tools/venv-recomp/Scripts/python -I tests/recomp/hotfn_hook_test.py [--emit DIR]

Emits main+0x96d390, 0x96d5f0, 0x155600, 0x8b5f10 and 0x8b6750 from the real module with and without the HOTFN tables and checks: the entry,
check and `ret` sites hold the instruction words the hooks were written for; with the tables the entry hook is followed by
the branch to the `ret` label and the check hook sits before the final store; without them the C has no hook. --emit DIR
also writes the hooked functions (renamed hk_<addr>) to DIR/hooked_fns.c for tests/hotfn (generated game code: keep it
under build/). Needs analysis/maps and the dumps like the generator does.
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

FAILS = []
RET = 0xd65f03c0
# (function, hook site, entry hook, check site, check hook, branch target, check site word)
SITES = [(0x96d390, 0x96d390, 'rt_hot_matdecomp', 0x96d5b0, 'rt_hot_matdecomp_check', 0x96d5b4, 0x6d000420),  # stp d0, d1, [x1]
         (0x96d5f0, 0x96d5f0, 'rt_hot_matdecomp_s', 0x96d8d8, 'rt_hot_matdecomp_s_check', 0x96d8dc, 0x6d000420),
         (0x155600, 0x155600, 'rt_hot_reverb', 0x155a1c, 'rt_hot_reverb_check', 0x155a20, 0x4d009100),      # st1 {v0.s}[3], [x8]
         (0x8b5f10, 0x8b5f10, 'rt_hot_hier', 0x8b66b0, 'rt_hot_hier_check', 0x8b66cc, 0x6d4623e9),          # ldp d9, d8, [sp, #0x60]
         (0x8b6750, 0x8b7400, 'rt_hot_bone1', 0x8b7960, 'rt_hot_bone1_check', 0x8b7960, 0xa943bff1),        # ldp x17, x15, [sp, #0x38]
         (0x8b6750, 0x8b7ac0, 'rt_hot_bone2', 0x8b8028, 'rt_hot_bone2_check', 0x8b8028, 0x1280000d)]       # mov w13, #-1
# guest regions the bone hooks replace, emitted as functions for tests/hotfn (they end by dispatching to the target)
REGIONS = [(0x8b7400, 0x8b7960), (0x8b7ac0, 0x8b8028)]


def check(cond, msg):
    print(('ok   ' if cond else 'FAIL ') + msg)
    if not cond:
        FAILS.append(msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--emit', type=Path)
    a = ap.parse_args()
    mod = load_module(name='main')
    fl = functions()
    starts = {BASE + s for s, e, k, t in fl}
    ends = {BASE + s: BASE + e for s, e, k, t in fl}
    lsda = ehlsda.module_pads(mod)
    word = lambda off: int(mod.words[(off - mod.nso.text[0]) >> 2])
    check(set(snap_gen.HOTFN_INLINE) <= set(snap_gen.MAIN_INLINE_HOOKS) and set(snap_gen.HOTFN_BRANCH.items()) <= set(snap_gen.MAIN_BRANCH_HOOKS.items()),
          'SNAP_GEN_HOTFN=1 turns the tables on')
    kw = dict(lsda=lsda, resume=True, setjmp_targets=set())

    def emit(entry, inline, branch):
        e = Emitter(mod, starts, inline_hooks=inline, branch_hooks=branch, **kw)
        lines, _ = e.function(BASE + entry, ends[BASE + entry])
        return '\n'.join(lines), e.stats

    base_inline = {k: v for k, v in snap_gen.MAIN_INLINE_HOOKS.items() if k not in snap_gen.HOTFN_INLINE}
    base_branch = {k: v for k, v in snap_gen.MAIN_BRANCH_HOOKS.items() if k not in snap_gen.HOTFN_BRANCH}
    hooked, emitted = [], {}
    nsites = {}
    for fn, *_ in SITES:
        nsites[fn] = nsites.get(fn, 0) + 1
    for fn, entry, hook, csite, chook, tgt, cword in SITES:
        mid = fn != entry
        if mid:
            check(word(tgt - 4) >> 26 == 0x25, f'main+{tgt:#x} follows a bl (only callee-saved registers live)')
        else:
            check(word(tgt) == RET, f'main+{tgt:#x} is the ret of main+{entry:#x}')
        check(word(csite) == cword, f'main+{csite:#x} holds {cword:08x}')
        check(BASE + entry <= BASE + tgt < ends[BASE + fn], f'main+{tgt:#x} lies inside main+{fn:#x}')
        if fn not in emitted:
            emitted[fn] = emit(fn, snap_gen.MAIN_INLINE_HOOKS, snap_gen.MAIN_BRANCH_HOOKS)
            check(emitted[fn][1]['branch_hook_sites'] == nsites[fn], f'{fn:#x}: {nsites[fn]} branch hook site(s)')
            off, st = emit(fn, base_inline, base_branch)
            check('rt_hot_' not in off and not st['branch_hook_sites'], f'{fn:#x}: without the tables no hook')
            if not mid:   # whole hooked functions; the bone loop hooks are tested as hooked regions below
                hooked.append(re.sub(r'\bvoid f_' + f'{BASE + fn:x}' + r'\(Cpu \*c\) \{', f'void hk_{fn:x}(Cpu *c) {{', emitted[fn][0], count=1))
        on = emitted[fn][0]
        i = on.index(f', {hook});')   # the rt_inline_hook call site
        check(on[i:].split('\n', 2)[1].strip() == f'if ((c)->x[16] == 0) {{ goto L_{BASE + tgt:x}; }}', f'{entry:#x}: the branch to the target follows the entry hook')
        lab = on.index(f'\nL_{BASE + tgt:x}:;')
        j = on.index(f', {chook});')
        if mid:
            check(i < lab < j, f'{entry:#x}: the check hook runs right after the target label')
        else:
            check(i < j < lab, f'{entry:#x}: the check hook runs before the ret label')
    regions = []
    for s, e_ in REGIONS:
        e = Emitter(mod, starts, inline_hooks={}, branch_hooks={}, **kw)
        lines, _ = e.function(BASE + s, BASE + e_)
        txt = '\n'.join(lines)
        check(f'c->pc = 0x{BASE + e_:x}ull;' in txt, f'region {s:#x}: ends at main+{e_:#x}')
        regions.append(re.sub(r'\bvoid f_' + f'{BASE + s:x}' + r'\(Cpu \*c\) \{', f'void rg_{s:x}(Cpu *c) {{', txt, count=1))
        e = Emitter(mod, starts, inline_hooks=snap_gen.MAIN_INLINE_HOOKS, branch_hooks=snap_gen.MAIN_BRANCH_HOOKS, **kw)
        lines, _ = e.function(BASE + s, BASE + e_ + 4)   # with the hooks, through the check hook at the target
        txt = '\n'.join(lines)
        hooked.append(re.sub(r'\bvoid f_' + f'{BASE + s:x}' + r'\(Cpu \*c\) \{', f'void hkrg_{s:x}(Cpu *c) {{', txt, count=1))
    if a.emit:
        a.emit.mkdir(parents=True, exist_ok=True)
        head = '// GENERATED-GAME-CODE (copies for tests/hotfn). Do not commit.\n#include "cpu.h"\n#include "simd.h"\n'
        for name, fns in (('hooked_fns.c', hooked), ('regions.c', regions)):
            body = '\n'.join(fns)
            decls = ''.join(f'void {n}(Cpu *c);\n' for n in sorted(set(re.findall(r'\b(f_[0-9a-f]{10})\(c\)', body))))
            (a.emit / name).write_text(head + decls + body + '\n', encoding='utf-8')
    print('FAILED' if FAILS else 'all passed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
