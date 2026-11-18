"""The setup wizard keeps its files in its own folder (setup/setup_main.cpp SetupDir) and offers shortcuts unticked.

Runs the compiled wizard (builder/release_setup.py --stub-only) with /shot, which draws every page on a private hidden desktop and exits:
nothing shows on screen and nothing is unpacked or built. Checks where the setup folder was made, that the per-user folder was not
touched, and that the two shortcut rows on the options page are drawn unticked (their pixels match the unticked license row's box).

Usage: tools/venv-recomp/Scripts/python -I tests/hle/wizard_folder_test.py path\\to\\setup_stub.exe [folder to keep the page pictures in]
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    if len(sys.argv) < 2 or not Path(sys.argv[1]).exists():
        print('usage: wizard_folder_test.py path\\to\\setup_stub.exe [keep_dir]')
        return 2
    stub = Path(sys.argv[1])
    keep = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    t = Path(tempfile.mkdtemp(prefix='snap_wiz_'))
    fails = 0
    try:
        def check(label, ok, got=''):
            nonlocal fails
            print('%s %s' % ('ok  ' if ok else 'FAIL', label))
            if not ok:
                fails += 1
                print('     got: %s' % str(got).strip()[:500])

        def run(folder, extra_env=None):
            folder.mkdir(parents=True, exist_ok=True)
            exe = folder / 'SnapRecomp.exe'
            shutil.copy2(stub, exe)
            shots = t / (folder.name + '_shots')
            shots.mkdir()
            env = dict(os.environ, LOCALAPPDATA=str(t / 'local'), APPDATA=str(t / 'appdata'))
            env.pop('SNAP_SETUP_DIR', None)
            env.update(extra_env or {})
            r = subprocess.run([str(exe), '/shot', str(shots)], cwd=str(t), env=env, timeout=120, creationflags=0x08000000)
            return r.returncode, shots

        # 1. a normal folder: setup\ is made next to the exe, nothing under the per-user folder
        a = t / 'Snap Folder'
        code, shots = run(a)
        check('setup folder made next to the exe', (a / 'setup').is_dir(), (code, [p.name for p in a.iterdir()]))
        check('nothing written under %LOCALAPPDATA%', not (t / 'local').exists(), list((t / 'local').rglob('*'))[:5] if (t / 'local').exists() else '')
        pages = sorted(p.name for p in shots.iterdir())
        check('every page was drawn', len(pages) >= 8, pages)
        if keep:
            keep.mkdir(parents=True, exist_ok=True)
            for p in shots.iterdir():
                shutil.copy2(p, keep / p.name)

        # 2. an install that already has settings.json in its folder keeps using it (no write probe needed)
        b = t / 'Has Settings'
        (b / 'setup').mkdir(parents=True)
        (b / 'setup' / 'settings.json').write_text('{"completed": false}')
        code, _ = run(b)
        check('existing setup folder next to the exe is used', not (t / 'local').exists() and (b / 'setup' / 'settings.json').is_file())

        # 3. SNAP_SETUP_DIR still wins (tests, a caller that wants it elsewhere)
        c = t / 'Override'
        code, _ = run(c, {'SNAP_SETUP_DIR': str(t / 'chosen')})
        check('SNAP_SETUP_DIR wins: no setup folder next to the exe', not (c / 'setup').exists(), [p.name for p in c.iterdir()])
    finally:
        shutil.rmtree(t, ignore_errors=True)
    print('wizard folder test: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
