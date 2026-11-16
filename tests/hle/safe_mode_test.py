"""Safe mode offer (docs/safe-mode.md): the launcher reads the launch marker the last start left behind.

Runs the built exe as the launcher (config.json next to it, SNAP_LAUNCHER_AUTOPLAY=1, scratch data folders): no window opens and no game
starts, because the converted game files named in config.json do not exist. A planted launch.marker in <LOCALAPPDATA>\\SnapRecomp\\logs is
checked for: no marker (no offer), a dead start with and without _crash.txt ("crashed" / "stopped"), a dead safe-mode start, a start that
is still running (no offer), and safe mode already asked for (--safe-mode / SNAP_SAFE_MODE=1: no offer).

Usage: tools/venv-recomp/Scripts/python -I tests/hle/safe_mode_test.py [path\\to\\snap_boot.exe]
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
    """Creation time of this process as the marker stores it (FILETIME as a number)."""
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
    work = Path(tempfile.mkdtemp(prefix='snap_safe_'))
    fails = 0
    try:
        game = work / 'game'
        game.mkdir()
        shutil.copy2(EXE, game / 'SnapRecomp.exe')
        for dll in EXE.parent.glob('*.dll'):
            shutil.copy2(dll, game / dll.name)
        (work / 'romfs').mkdir()
        (work / 'none.elf').write_bytes(b'')
        (game / 'config.json').write_text(json.dumps({
            'main_elf': str(work / 'none.elf'), 'sdk_elf': str(work / 'none.elf'), 'romfs_root': str(work / 'romfs'), 'data_root': str(work / 'data'),
            'upscalers_offered': True}))
        logs = work / 'local' / 'SnapRecomp' / 'logs'

        def run(args=(), extra_env=None):
            env = dict(os.environ, SNAP_CONFIG_DIR=str(work / 'cfg'), SNAP_PROFILES_DIR=str(work / 'profiles'), LOCALAPPDATA=str(work / 'local'),
                       APPDATA=str(work / 'appdata'), SNAP_LAUNCHER_AUTOPLAY='1')
            env.pop('SNAP_SAFE_MODE', None)
            env.update(extra_env or {})
            r = subprocess.run([str(game / 'SnapRecomp.exe'), *args], cwd=game, env=env, capture_output=True, text=True, timeout=60, creationflags=0x08000000)
            return r.stderr + r.stdout

        def marker(pid, start, safe, crash):
            logs.mkdir(parents=True, exist_ok=True)
            (logs / '_crash.txt').unlink(missing_ok=True)
            (logs / 'launch.marker').write_text('pid=%d\r\nstart=%d\r\nsafe=%d\r\n' % (pid, start, safe))
            if crash:
                time.sleep(0.05)
                (logs / '_crash.txt').write_text('Snap Recomp crash report\r\n')

        def check(label, text, want, absent=()):
            nonlocal fails
            ok = all(w in text for w in want) and not any(a in text for a in absent)
            print('%s %s' % ('ok  ' if ok else 'FAIL', label))
            if not ok:
                fails += 1
                print('     wanted %r, not %r; got: %s' % (want, absent, text.strip()[:400]))

        dead = dead_pid()
        # no marker: nothing offered
        shutil.rmtree(logs, ignore_errors=True)
        check('no marker: no offer', run(), [], ['safe mode offered'])
        # a dead start with a crash report newer than the marker
        marker(dead, 1, 0, True)
        check('dead start + _crash.txt: offer says crashed', run(), ['safe mode offered: The last start crashed before the title screen'])
        # a dead start with no crash report: stopped
        marker(dead, 1, 0, False)
        check('dead start, no _crash.txt: offer says stopped', run(), ['safe mode offered: The last start stopped before the title screen'], ['crashed'])
        # the dead start was already safe mode
        marker(dead, 1, 1, True)
        check('dead safe-mode start: offer says so', run(), ['already in safe mode'])
        # still running (this process, with its real start time): not a crash
        marker(os.getpid(), filetime_of_self(), 0, False)
        check('start still running: no offer', run(), [], ['safe mode offered'])
        # safe mode already asked for: no question
        marker(dead, 1, 0, True)
        check('--safe-mode: no offer', run(['--safe-mode']), [], ['safe mode offered'])
        check('SNAP_SAFE_MODE=1: no offer', run(extra_env={'SNAP_SAFE_MODE': '1'}), [], ['safe mode offered'])
        # the answer yes only changes the next start: it is logged, the marker stays for the real start to replace
        marker(dead, 1, 0, True)
        check('answer yes: still offered once, no crash', run(extra_env={'SNAP_LAUNCHER_SAFE_ANSWER': 'yes'}), ['safe mode offered'])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('safe mode offer test: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
