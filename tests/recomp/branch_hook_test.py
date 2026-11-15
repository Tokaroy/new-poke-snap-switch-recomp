"""Generator test for MAIN_BRANCH_HOOKS (recomp/gen.py Emitter branch_hooks, recomp/snap_gen.py UI_PACE_*).

  tools/venv-recomp/Scripts/python -I tests/recomp/branch_hook_test.py

Emits the interface handler (main+0x7835d0), and the pad update (0x925b60) from the real module, with and
without the branch hooks, and checks: the hook sites and instruction words are the ones the hooks were written for; with the hooks the
branch is emitted before the hooked instruction, its target is a label of the function, and the other hook sites are plain hooks; without
them the C is byte-identical to a build that has no branch hook table at all. Needs analysis/maps and the dumps like the generator does.
"""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'recomp'))
os.environ['SNAP_GEN_UI_PACE'] = '1'
from snap_common import BASE, functions, load_module  # noqa: E402
import ehlsda  # noqa: E402
import snap_gen  # noqa: E402
from gen import Emitter  # noqa: E402

FAILS = []


def check(cond, msg):
    print(('ok   ' if cond else 'FAIL ') + msg)
    if not cond:
        FAILS.append(msg)


def main():
    mod = load_module(name='main')
    fl = functions()
    starts = {BASE + s for s, e, k, t in fl}
    ends = {BASE + s: BASE + e for s, e, k, t in fl}
    lsda = ehlsda.module_pads(mod)
    words = {0x783610: 0xf900c7e0, 0x794b90: 0xf940c7e0, 0x9263ac: 0xa95b7bfd, 0x643cc8: 0xbd401e61}
    for off, w in words.items():
        check(int(mod.words[(off - mod.nso.text[0]) >> 2]) == w, f'main+{off:#x} holds the instruction word {w:08x}')
    check(snap_gen.MAIN_BRANCH_HOOKS == snap_gen.UI_PACE_BRANCH and set(snap_gen.UI_PACE_INLINE) <= set(snap_gen.MAIN_INLINE_HOOKS),
          'SNAP_GEN_UI_PACE=1 turns the tables on')
    kw = dict(lsda=lsda, resume=True, setjmp_targets=set())

    def emit(entry, inline, branch):
        e = Emitter(mod, starts, inline_hooks=inline, branch_hooks=branch, **kw)
        lines, _ = e.function(BASE + entry, ends[BASE + entry])
        return '\n'.join(lines), e.stats

    base_inline = {a: n for a, n in snap_gen.MAIN_INLINE_HOOKS.items() if a not in snap_gen.UI_PACE_INLINE}
    # handler: two branch hooks
    on, st = emit(0x7835d0, snap_gen.MAIN_INLINE_HOOKS, snap_gen.MAIN_BRANCH_HOOKS)
    check(st['branch_hook_sites'] == 2, 'the handler has two branch hook sites')
    check('rt_gm_ui_enter' in on and 'rt_gm_ui_event' in on, 'both hooks are called')
    i = on.index('rt_gm_ui_enter')
    tail = on[i:].split('\n', 2)
    check(tail[1].strip() == 'if ((c)->x[16] == 0) { goto L_7100794b4c; }', 'enter: the branch to the drawing section follows the hook')
    check('\nL_7100794b4c:;' in on, 'enter: the target is a label of the function')
    j = on.index('rt_gm_ui_event')
    check(on[j:].split('\n', 2)[1].strip() == 'if ((c)->x[16] == 0) { goto L_7100795f08; }', 'event: the branch to the exit follows the hook')
    check(on.index('L_7100794b90:;') < j, 'event: the hook sits after the label, so every path into the raise runs it')
    # the other sites are plain hooks
    pad, st = emit(0x925b60, snap_gen.MAIN_INLINE_HOOKS, snap_gen.MAIN_BRANCH_HOOKS)
    check('rt_gm_ui_pad' in pad and st['branch_hook_sites'] == 0 and 'x[16] == 0' not in pad, 'pad: a plain hook, no branch')
    ptr, st = emit(0x643a70, snap_gen.MAIN_INLINE_HOOKS, snap_gen.MAIN_BRANCH_HOOKS)
    check('rt_gm_ui_pointer' in ptr and st['branch_hook_sites'] == 0 and 'x[16] == 0' not in ptr, 'pointer (FPS-02): a plain hook, no branch')
    k = ptr.find('rt_gm_ui_pointer')
    check(k > 0 and ptr.rfind('0x1cull', 0, k) > ptr.rfind('\n}', 0, k), 'pointer: the hook runs after `ldr s1, [x19, #0x1c]`')
    # without the tables the handler is what the generator made before the feature (no ui hooks, no branch)
    off, st = emit(0x7835d0, base_inline, {})
    check('rt_gm_ui_' not in off and 'x[16] == 0' not in off and not st['branch_hook_sites'], 'without the tables: no hook and no branch')
    off2, _ = emit(0x7835d0, base_inline, None)
    check(off == off2, 'an empty and a missing branch table give the same C')
    print('FAILED' if FAILS else 'all passed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
