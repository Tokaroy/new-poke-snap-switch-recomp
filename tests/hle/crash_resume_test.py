"""Crash resume (docs/crash-resume.md): the launcher reads the session marker the last session left behind.

Runs the built exe as the launcher (config.json next to it, SNAP_LAUNCHER_AUTOPLAY=1, scratch data folders): no window opens and no game
starts, because the converted game files named in config.json do not exist. A planted session.marker in <LOCALAPPDATA>\\SnapRecomp\\logs is
checked for: no marker (nothing offered), a dead owner with each last screen (target map, lab menu, none), _crash.txt newer than the marker,
an owner that is still running (nothing), the switches that turn it off (--crash-prompt off, SNAP_CRASH_PROMPT=0, config.json
"crash_prompt": false), --resume (the answer is given and the resume script is written), and what happens to the marker after an answer.

Usage: tools/venv-recomp/Scripts/python -I tests/hle/crash_resume_test.py [path\\to\\snap_boot.exe]
"""
import ctypes
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build' / 'snap' / 'snap_boot.exe'


def filetime_of_self():
    k = ctypes.windll.kernel32
    c, e, kt, u = (ctypes.c_ulonglong() for _ in range(4))
    k.GetCurrentProcess.restype = ctypes.c_void_p
    k.GetProcessTimes(ctypes.c_void_p(k.GetCurrentProcess()), ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(u))
    return c.value


def dead_pid():
    p = subprocess.Popen(['cmd', '/c', 'exit', '0'], creationflags=0x08000000)
    p.wait()
    return p.pid


def main():
    if not EXE.exists():
        print('exe not found:', EXE)
        return 2
    work = Path(tempfile.mkdtemp(prefix='snap_resume_'))
    fails = 0
    try:
        game = work / 'game'
        game.mkdir()
        shutil.copy2(EXE, game / 'SnapRecomp.exe')
        for dll in EXE.parent.glob('*.dll'):
            shutil.copy2(dll, game / dll.name)
        (work / 'romfs').mkdir()
        (work / 'none.elf').write_bytes(b'')
        cfg = {'main_elf': str(work / 'none.elf'), 'sdk_elf': str(work / 'none.elf'), 'romfs_root': str(work / 'romfs'), 'data_root': str(work / 'data'),
               'upscalers_offered': True}
        (game / 'config.json').write_text(json.dumps(cfg))
        logs = work / 'local' / 'SnapRecomp' / 'logs'

        def run(args=(), extra_env=None):
            env = dict(os.environ, SNAP_CONFIG_DIR=str(work / 'cfg'), SNAP_PROFILES_DIR=str(work / 'profiles'), LOCALAPPDATA=str(work / 'local'),
                       APPDATA=str(work / 'appdata'), SNAP_LAUNCHER_AUTOPLAY='1')
            for k in ('SNAP_SAFE_MODE', 'SNAP_CRASH_PROMPT', 'SNAP_CRASH_RESUME', 'SNAP_INPUT_SCRIPT', 'SNAP_LAUNCHER_CRASH_ANSWER'):
                env.pop(k, None)
            env.update(extra_env or {})
            r = subprocess.run([str(game / 'SnapRecomp.exe'), *args], cwd=game, env=env, capture_output=True, text=True, timeout=60, creationflags=0x08000000)
            return r.stderr + r.stdout

        def marker(pid, start, scene, crash):
            logs.mkdir(parents=True, exist_ok=True)
            (logs / '_crash.txt').unlink(missing_ok=True)
            (logs / 'launch.marker').unlink(missing_ok=True)
            (logs / 'session.marker').write_text('pid=%d\r\nstart=%d\r\nscene=%s\r\nwhen=1795000000\r\n' % (pid, start, scene))
            if crash:
                time.sleep(0.05)
                (logs / '_crash.txt').write_text('Snap Recomp crash report\r\n')

        def check(label, text, want, absent=()):
            nonlocal fails
            ok = all(w in text for w in want) and not any(a in text for a in absent)
            print('%s %s' % ('ok  ' if ok else 'FAIL', label))
            if not ok:
                fails += 1
                print('     wanted %r, not %r; got: %s' % (want, absent, text.strip()[:500]))

        def truth(label, cond):
            nonlocal fails
            print('%s %s' % ('ok  ' if cond else 'FAIL', label))
            if not cond:
                fails += 1

        dead = dead_pid()
        shutil.rmtree(logs, ignore_errors=True)
        check('no marker: nothing offered', run(), [], ['crash resume offered'])
        marker(dead, 1, 'map_select', True)
        out = run()
        check('dead owner, last screen the course map, crash report newer', out,
              ['crash resume offered: The game closed unexpectedly (the last screen it showed was the course map)', 'target=map_select', 'crashed=1', 'save_flagged=0'])
        truth('answer defaults to a normal start and the marker is removed', not (logs / 'session.marker').exists())
        marker(dead, 1, 'main_menu', False)
        check('lab menu, no crash report (a kill)', run(), ['target=main_menu', 'crashed=0'])
        marker(dead, 1, 'title', False)
        check('never past the title: no target', run(), ['crash resume offered', 'target= '])
        marker(dead, 1, '', False)
        check('empty scene: no target, no label', run(), ['crash resume offered: The game closed unexpectedly.', 'target= '])
        marker(os.getpid(), filetime_of_self(), 'map_select', False)
        check('owner still running (a second copy): nothing offered', run(), [], ['crash resume offered'])
        truth('...and the marker is left alone', (logs / 'session.marker').exists())
        marker(dead, 1, 'map_select', True)
        check('--crash-prompt off', run(['--crash-prompt', 'off']), [], ['crash resume offered'])
        check('SNAP_CRASH_PROMPT=0', run(extra_env={'SNAP_CRASH_PROMPT': '0'}), [], ['crash resume offered'])
        (game / 'config.json').write_text(json.dumps(dict(cfg, crash_prompt=False)))
        check('config.json crash_prompt false', run(), [], ['crash resume offered'])
        check('...--crash-prompt on wins for one run', run(['--crash-prompt', 'on']), ['crash resume offered'])
        (game / 'config.json').write_text(json.dumps(cfg))
        marker(dead, 1, 'map_select', True)
        check('answer none keeps the marker for next time', run(extra_env={'SNAP_LAUNCHER_CRASH_ANSWER': 'none'}), ['crash resume offered'])
        truth('...marker kept', (logs / 'session.marker').exists())
        marker(dead, 1, 'photo_submit', True)
        out = run(['--play', '--resume'])
        check('--play --resume: resumes to the course map without asking', out, ['crash resume offered', 'resuming to map_select'])
        script = logs / 'resume_script.txt'
        truth('...the script was written with both waits', script.exists() and 'wait ui=main_menu' in script.read_text() and 'wait ui=map_select' in script.read_text())
        truth('...marker removed', not (logs / 'session.marker').exists())
        marker(dead, 1, 'option', False)
        out = run(['--play', '--resume'])
        check('--resume from a menu: lab menu only', out, ['resuming to main_menu'])
        truth('...script has no map wait', 'wait ui=map_select' not in script.read_text())
        marker(dead, 1, 'title', False)
        check('--resume with nothing to skip: plain start', run(['--play', '--resume']), ['crash resume offered'], ['resuming to'])
        marker(dead, 1, 'map_select', False)
        check('--play without --resume: asks nothing, starts normally', run(['--play']), ['crash resume offered'], ['resuming to'])
        marker(dead, 1, 'map_select', False)
        check('answer resume (test switch)', run(extra_env={'SNAP_LAUNCHER_CRASH_ANSWER': 'resume'}), ['resuming to map_select'])
        marker(dead, 1, 'map_select', False)
        check('a script already set by the caller is kept', run(['--play', '--resume'], {'SNAP_INPUT_SCRIPT': str(work / 'mine.txt')}), ['resume skipped'])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('crash resume test: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
