"""Generator test for the hot-function hooks (recomp/snap_gen.py HOTFN_*, host/mods/hotfn.cpp; project code, ours).

  tools/venv-recomp/Scripts/python -I tests/recomp/hotfn_hook_test.py [--emit DIR]

Emits main+0x96d390, 0x96d5f0 and 0x155600 from the real module with and without the HOTFN tables and checks: the entry,
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
# entry: (entry hook, check site, check hook, ret, check site word)
SITES = {0x96d390: ('rt_hot_matdecomp', 0x96d5b0, 'rt_hot_matdecomp_check', 0x96d5b4, 0x6d000420),     # stp d0, d1, [x1]
         0x96d5f0: ('rt_hot_matdecomp_s', 0x96d8d8, 'rt_hot_matdecomp_s_check', 0x96d8dc, 0x6d000420),
         0x155600: ('rt_hot_reverb', 0x155a1c, 'rt_hot_reverb_check', 0x155a20, 0x4d009100)}         # st1 {v0.s}[3], [x8]


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
    hooked = []
    for entry, (hook, csite, chook, ret, cword) in SITES.items():
        check(word(ret) == RET, f'main+{ret:#x} is the ret of main+{entry:#x}')
        check(word(csite) == cword, f'main+{csite:#x} holds the final store {cword:08x}')
        check(BASE + entry < BASE + ret < ends[BASE + entry], f'main+{ret:#x} lies inside main+{entry:#x}')
        on, st = emit(entry, snap_gen.MAIN_INLINE_HOOKS, snap_gen.MAIN_BRANCH_HOOKS)
        check(st['branch_hook_sites'] == 1, f'{entry:#x}: one branch hook site')
        i = on.index(f', {hook});')   # the rt_inline_hook call site
        check(on[i:].split('\n', 2)[1].strip() == f'if ((c)->x[16] == 0) {{ goto L_{BASE + ret:x}; }}', f'{entry:#x}: the branch to the ret follows the entry hook')
        check(f'\nL_{BASE + ret:x}:;' in on, f'{entry:#x}: the ret has a label')
        j = on.index(f', {chook});')
        check(i < j < on.index(f'L_{BASE + ret:x}:;'), f'{entry:#x}: the check hook runs before the ret label')
        off, st = emit(entry, base_inline, base_branch)
        check('rt_hot_' not in off and not st['branch_hook_sites'], f'{entry:#x}: without the tables no hook')
        hooked.append(re.sub(r'\bvoid f_' + f'{BASE + entry:x}' + r'\(Cpu \*c\) \{', f'void hk_{entry:x}(Cpu *c) {{', on, count=1))
    if a.emit:
        a.emit.mkdir(parents=True, exist_ok=True)
        (a.emit / 'hooked_fns.c').write_text('// GENERATED-GAME-CODE (hooked copies for tests/hotfn). Do not commit.\n#include "cpu.h"\n#include "simd.h"\n'
                                             + '\n'.join(hooked) + '\n', encoding='utf-8')
    print('FAILED' if FAILS else 'all passed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
