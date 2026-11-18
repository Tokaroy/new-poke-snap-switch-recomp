"""One exe (release standard 1): the downloaded exe is the setup until a build exists, then it starts the built game.

Uses the compiled setup exe without a payload (see wizard_folder_test.py for how it is compiled) and the built game exe. Nothing
shows on screen: the game runs only as the launcher with SNAP_LAUNCHER_AUTOPLAY=1 or prints and exits, and the setup half is only
asked for text. The job-object checks use copies of cmd.exe and ping.exe standing in for the built program.

Checks: flags, standard output and the exit code pass through to the built game; setup flags are not passed on; --build-dir finds a
build elsewhere; with no build the exe answers --help and --version itself; stopping the exe stops the game; a restart the game does
itself is waited for.

Usage: tools/venv-recomp/Scripts/python -I tests/hle/one_exe_test.py path\\to\\setup_stub.exe [path\\to\\snap_boot.exe]
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NOWIN = 0x08000000


def main():
    if len(sys.argv) < 2 or not Path(sys.argv[1]).exists():
        print('usage: one_exe_test.py path\\to\\setup_stub.exe [path\\to\\snap_boot.exe]')
        return 2
    stub = Path(sys.argv[1])
    game = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'build' / 'snap' / 'snap_boot.exe'
    if not game.exists():
        print('game exe not found:', game)
        return 2
    t = Path(tempfile.mkdtemp(prefix='snap_one_'))
    fails = 0
    try:
        def check(label, ok, got=''):
            nonlocal fails
            print('%s %s' % ('ok  ' if ok else 'FAIL', label))
            if not ok:
                fails += 1
                print('     got: %s' % str(got).strip()[:700])

        env = dict(os.environ, LOCALAPPDATA=str(t / 'local'), APPDATA=str(t / 'appdata'), SNAP_LAUNCHER_AUTOPLAY='1')
        for k in ('SNAP_SETUP_DIR', 'SNAP_SAFE_MODE', 'SNAP_DATA_DIR', 'SNAP_PORTABLE', 'SNAP_REAL_APPDATA', 'SNAP_REAL_LOCALAPPDATA', 'SNAP_TOP_EXE'):
            env.pop(k, None)

        def install(root, built_from=None, top=True):
            """<root>\\SnapRecomp.exe (the downloaded exe) and <root>\\work\\out with a build; built_from = another program to stand in."""
            out = root / 'work' / 'out'
            out.mkdir(parents=True)
            if top:
                shutil.copy2(stub, root / 'SnapRecomp.exe')
            if built_from is None:
                shutil.copy2(game, out / 'SnapRecomp.exe')
                for dll in game.parent.glob('*.dll'):
                    shutil.copy2(dll, out / dll.name)
            else:
                shutil.copy2(built_from, out / 'SnapRecomp.exe')
            (t / 'romfs').mkdir(exist_ok=True)
            (t / 'none.elf').write_bytes(b'')
            (out / 'config.json').write_text(json.dumps({'main_elf': str(t / 'none.elf'), 'sdk_elf': str(t / 'none.elf'), 'romfs_root': str(t / 'romfs'),
                                                         'upscalers_offered': True, 'update_check': False, 'builder_version': '9.8.7'}))
            (out / 'portable.txt').write_text('')
            return out

        def run(exe, args=(), timeout=90):
            r = subprocess.run([str(exe), *args], cwd=str(t), env=env, capture_output=True, text=True, timeout=timeout, creationflags=NOWIN, stdin=subprocess.DEVNULL)
            return r.returncode, r.stdout + r.stderr

        # ---- a folder with a finished build: the exe is the game
        a = t / 'Snap Recomp'
        out = install(a)
        top = a / 'SnapRecomp.exe'
        code, text = run(top, ['--version'])
        check('built: --version is answered by the game', code == 0 and 'Snap Recomp 9.8.7' in text, (code, text))
        code, text = run(top, ['--help'])
        check('built: --help lists the game flags', code == 0 and '--fullscreen' in text and '--data-dir' in text, (code, text))
        code, text = run(top, ['--fullscren'])
        check('built: the exit code comes back (mistyped flag = 2)', code == 2 and '--fullscren' in text, (code, text))
        code, text = run(top)
        direct_code, direct = run(out / 'SnapRecomp.exe')
        check('built: no flags starts the launcher, same result as the built exe itself', 'launcher:' in text and code == direct_code, (code, direct_code, text))
        dd = t / 'data with space'
        code, text = run(top, ['--data-dir', str(dd), '--windowed', '--width', '960'])
        check('built: values with spaces arrive whole', (dd / 'Roaming').is_dir() and 'launcher:' in text, (code, text))
        check('built: no setup folder was unpacked for a start', not (a / 'setup' / 'app').exists(), list((a / 'setup').rglob('*'))[:5] if (a / 'setup').exists() else '')

        # ---- --build-dir: the build is somewhere else; the flag itself is not passed on (the game would refuse it)
        b = t / 'Elsewhere'
        install(b, top=False)
        lone = t / 'Just The Exe'
        lone.mkdir()
        shutil.copy2(stub, lone / 'SnapRecomp.exe')
        code, text = run(lone / 'SnapRecomp.exe', ['--build-dir', str(b), '--version'])
        check('--build-dir: finds the build there and starts it', code == 0 and 'Snap Recomp 9.8.7' in text, (code, text))

        # ---- no build yet: the exe answers for itself, and never opens a window for these
        code, text = run(lone / 'SnapRecomp.exe', ['--version'])
        check('not built: --version says so', code == 0 and 'not built yet' in text, (code, text))
        code, text = run(lone / 'SnapRecomp.exe', ['--help'])
        check('not built: --help lists the setup flags', code == 0 and all(w in text for w in ('--game', '--build-dir', '--yes', '--repair', '--uninstall', '--setup')), (code, text))
        code, text = run(lone / 'SnapRecomp.exe', ['--yes'])
        check('not built: --yes without --game stops with exit 2', code == 2 and '--game' in text, (code, text))
        half = t / 'Half Built'
        (half / 'work' / 'out').mkdir(parents=True)
        shutil.copy2(stub, half / 'SnapRecomp.exe')
        shutil.copy2(game, half / 'work' / 'out' / 'SnapRecomp.exe')     # no config.json: the build never finished
        code, text = run(half / 'SnapRecomp.exe', ['--version'])
        check('unfinished build (no config.json): not started', code == 0 and 'not built yet' in text, (code, text))

        # ---- the job: stopping the downloaded exe stops the game
        sysdir = Path(os.environ.get('SystemRoot', 'C:\\Windows')) / 'System32'
        j = t / 'Job'
        jout = install(j, built_from=sysdir / 'PING.EXE')
        p = subprocess.Popen([str(j / 'SnapRecomp.exe'), '-n', '60', '127.0.0.1'], cwd=str(t), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             stdin=subprocess.DEVNULL, creationflags=NOWIN)

        def children_of(path):
            q = "Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq '%s' } | ForEach-Object { $_.ProcessId }" % str(path).replace("'", "''")
            r = subprocess.run(['powershell', '-NoProfile', '-Command', q], capture_output=True, text=True, creationflags=NOWIN)
            return [int(x) for x in r.stdout.split()]

        kids = []
        for _ in range(40):
            kids = children_of(jout / 'SnapRecomp.exe')
            if kids:
                break
            time.sleep(0.25)
        check('job: the built program is running under the exe', len(kids) == 1 and p.poll() is None, (kids, p.poll()))
        subprocess.run(['taskkill', '/PID', str(p.pid), '/F'], capture_output=True, creationflags=NOWIN)   # the exe alone, not its tree
        p.wait(timeout=10)
        gone = False
        for _ in range(40):
            if not children_of(jout / 'SnapRecomp.exe'):
                gone = True
                break
            time.sleep(0.25)
        check('job: stopping the exe stopped the game', gone, children_of(jout / 'SnapRecomp.exe'))
        for pid in children_of(jout / 'SnapRecomp.exe'):   # only if the check failed: our own stand-in, by PID
            subprocess.run(['taskkill', '/PID', str(pid), '/F'], capture_output=True, creationflags=NOWIN)

        # ---- a restart the game does itself: the exe stays until that one is finished too
        r2 = t / 'Restart'
        install(r2, built_from=sysdir / 'cmd.exe')
        t0 = time.time()
        code, text = run(r2 / 'SnapRecomp.exe', ['/c', 'start', '/b', 'ping', '-n', '6', '127.0.0.1'], timeout=60)
        took = time.time() - t0
        check('restart: the exe waited for the process the game started (about 5 s)', took >= 3.5, took)
    finally:
        shutil.rmtree(t, ignore_errors=True)
    print('one exe test: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
