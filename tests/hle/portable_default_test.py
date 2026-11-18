"""Portable by default: the one-time copy of an existing %APPDATA%\\SnapRecomp into the game's folder (host/portable.cpp snap_portable_adopt).

Runs the built exe as the launcher (config.json next to it, SNAP_LAUNCHER_AUTOPLAY=1, scratch folders standing in for %APPDATA% and
%LOCALAPPDATA%): no window opens and no game starts. Checks that the copy happens once, that it is a copy (every source file is still there
with the same bytes), that the shader cache and the updater's backups are left out, that a stopped copy is redone, and that --data-dir and
a folder without portable.txt copy nothing.

Usage: tools/venv-recomp/Scripts/python -I tests/hle/portable_default_test.py [path\\to\\snap_boot.exe]
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build' / 'snap' / 'snap_boot.exe'


def digest(folder):
    """Relative path -> sha1 of every file under folder."""
    out = {}
    for p in sorted(Path(folder).rglob('*')):
        if p.is_file():
            out[p.relative_to(folder).as_posix()] = hashlib.sha1(p.read_bytes()).hexdigest()
    return out


def main():
    if not EXE.exists():
        print('exe not found:', EXE)
        return 2
    work = Path(tempfile.mkdtemp(prefix='snap_portable_'))
    fails = 0
    try:
        def check(label, ok, got=''):
            nonlocal fails
            print('%s %s' % ('ok  ' if ok else 'FAIL', label))
            if not ok:
                fails += 1
                print('     got: %s' % str(got).strip()[:500])

        def make_game(name, portable):
            game = work / name
            game.mkdir()
            shutil.copy2(EXE, game / 'SnapRecomp.exe')
            for dll in EXE.parent.glob('*.dll'):
                shutil.copy2(dll, game / dll.name)
            (game / 'config.json').write_text(json.dumps({
                'main_elf': str(work / 'none.elf'), 'sdk_elf': str(work / 'none.elf'), 'romfs_root': str(work / 'romfs'), 'upscalers_offered': True}))
            if portable:
                (game / 'portable.txt').write_text('')
            return game

        def make_real(name, with_save=True):
            real = work / name / 'SnapRecomp'
            if with_save:
                (real / 'profiles' / 'p1' / 'save').mkdir(parents=True)
                (real / 'profiles' / 'p1' / 'save' / 'CURRENT').write_bytes(b'SNAPSAVE1\n7\n' + os.urandom(64))
                (real / 'settings.cfg').write_text('fps_target=120\n')
                (real / 'input.json').write_text('{}')
                (real / 'mods' / 'pack').mkdir(parents=True)
                (real / 'mods' / 'pack' / 'tex.bin').write_bytes(os.urandom(4096))
            (real / 'shadercache').mkdir(parents=True)
            (real / 'shadercache' / 'pipelines.bin').write_bytes(os.urandom(2048))
            (real / 'update-backups' / 'x').mkdir(parents=True)
            (real / 'update-backups' / 'x' / 'old.cfg').write_text('old')
            return real

        def run(game, real_name, args=()):
            env = dict(os.environ, APPDATA=str(work / real_name), LOCALAPPDATA=str(work / (real_name + '_local')), SNAP_LAUNCHER_AUTOPLAY='1')
            for k in ('SNAP_SAFE_MODE', 'SNAP_DATA_DIR', 'SNAP_PORTABLE', 'SNAP_REAL_APPDATA', 'SNAP_REAL_LOCALAPPDATA', 'SNAP_CONFIG_DIR', 'SNAP_PROFILES_DIR', 'SNAP_SAVE_DIR'):
                env.pop(k, None)
            r = subprocess.run([str(game / 'SnapRecomp.exe'), *args], cwd=game, env=env, capture_output=True, text=True, timeout=60, creationflags=0x08000000)
            return r.returncode, r.stderr + r.stdout

        (work / 'romfs').mkdir()
        (work / 'none.elf').write_bytes(b'')

        # 1. a portable folder and an existing %APPDATA% save: copied once, the source untouched
        game = make_game('game1', True)
        real = make_real('real1')
        before = digest(real)
        code, out = run(game, 'real1')
        dst = game / 'portable_data' / 'Roaming' / 'SnapRecomp'
        got = digest(dst) if dst.exists() else {}
        want = {k: v for k, v in before.items() if not k.startswith(('shadercache/', 'update-backups/'))}
        check('first portable start: saves, settings and mods copied', got == want and len(want) == 4, (code, out, sorted(got)))
        check('the copy is a copy: every source file is still there, unchanged', digest(real) == before, sorted(digest(real)))
        check('shader cache and update backups left out', not (dst / 'shadercache').exists() and not (dst / 'update-backups').exists())
        check('the player is told where the saves live', 'launcher: Your saves and settings now live' in out and str(dst) in out, out)
        check('no half-copied folder left', not (game / 'portable_data' / 'Roaming' / 'SnapRecomp.importing').exists())

        # 2. the second start copies nothing: a change in either place stays where it is
        (dst / 'settings.cfg').write_text('fps_target=144\n')
        (real / 'settings.cfg').write_text('fps_target=72\n')
        code, out = run(game, 'real1')
        check('second start: no second copy, no message', (dst / 'settings.cfg').read_text() == 'fps_target=144\n' and 'now live' not in out, out)
        check('second start: the old folder is still not touched', (real / 'settings.cfg').read_text() == 'fps_target=72\n')

        # 3. a copy that was stopped half way is thrown away and redone
        game = make_game('game3', True)
        real = make_real('real3')
        before = digest(real)
        half = game / 'portable_data' / 'Roaming' / 'SnapRecomp.importing'
        half.mkdir(parents=True)
        (half / 'junk.bin').write_bytes(b'half')
        code, out = run(game, 'real3')
        dst = game / 'portable_data' / 'Roaming' / 'SnapRecomp'
        check('stopped copy: redone in full, the leftover is gone',
              (dst / 'settings.cfg').is_file() and not (dst / 'junk.bin').exists() and not half.exists() and digest(real) == before, (code, out))

        # 4. nothing worth copying in %APPDATA% (only caches): no copy, no message
        game = make_game('game4', True)
        make_real('real4', with_save=False)
        code, out = run(game, 'real4')
        check('only caches in the old folder: nothing copied', 'now live' not in out and not (game / 'portable_data' / 'Roaming' / 'SnapRecomp' / 'shadercache').exists(), out)

        # 5. no portable.txt: the per-user folders are used as before
        game = make_game('game5', False)
        real = make_real('real5')
        before = digest(real)
        code, out = run(game, 'real5')
        check('no portable.txt: no portable_data, nothing copied', not (game / 'portable_data').exists() and 'now live' not in out and digest(real) == before, out)

        # 6. --data-dir: a folder the caller chose starts empty, even next to portable.txt
        game = make_game('game6', True)
        real = make_real('real6')
        dd = work / 'chosen'
        code, out = run(game, 'real6', ['--data-dir', str(dd)])
        check('--data-dir: nothing copied into it', (dd / 'Roaming').is_dir() and not (dd / 'Roaming' / 'SnapRecomp' / 'settings.cfg').exists()
              and not (game / 'portable_data').exists() and 'now live' not in out, out)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('portable default test: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
