"""Empty, cut-off and wrong converted game files (main and sdk) must give a plain message and a nonzero exit code, never a crash.

Runs the built exe twice per bad file: as the launcher (config.json next to the exe, SNAP_LAUNCHER_AUTOPLAY=1, scratch data folders) and
as the plain command line (--main/--sdk). No window opens: autoplay shows no dialog, and the loader's message box is only set up by
the launcher after a successful check.

Usage: tools/venv-recomp/Scripts/python -I tests/hle/bad_game_files_test.py [path\\to\\snap_boot.exe]
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build' / 'snap' / 'snap_boot.exe'
CRASH_CODES = {0xC0000005, 0xC00000FD, 0xC0000409, 0xC000001D}   # access violation, stack overflow, fast-fail, illegal instruction


def real_header():
    """The first bytes of a real ELF (so 'truncated' means a half-copied real file), or a synthetic ELF header."""
    for p in (ROOT / 'analysis' / 'import' / 'v262144-main.elf',):
        if p.exists():
            return p.read_bytes()[:0x4000]
    h = bytearray(64)
    h[0:4] = b'\x7fELF'
    h[4] = 2
    h[5] = 1
    h[18] = 0xB7
    return bytes(h)


def main():
    if not EXE.exists():
        print('exe not found:', EXE)
        return 2
    work = Path(tempfile.mkdtemp(prefix='snap_bad_elf_'))
    fails = 0
    try:
        game = work / 'game'
        game.mkdir()
        shutil.copy2(EXE, game / 'SnapRecomp.exe')
        for dll in EXE.parent.glob('*.dll'):
            shutil.copy2(dll, game / dll.name)
        (work / 'romfs').mkdir()
        head = real_header()
        fixtures = {
            'empty': b'',
            'one byte': b'\x7f',
            'header only (cut)': head[:64],
            'real start (cut)': head[:0x2000],
            'text file': b'this is not a game file\n' * 100,
        }
        imp = ROOT / 'analysis' / 'import'
        real = {'main': imp / 'v262144-main.elf', 'sdk': imp / 'v262144-sdk.elf'}   # whole real files (local only); without them only the main cases run
        scratch_env = dict(os.environ, SNAP_CONFIG_DIR=str(work / 'cfg'), SNAP_PROFILES_DIR=str(work / 'profiles'), LOCALAPPDATA=str(work / 'local'),
                           APPDATA=str(work / 'appdata'), SNAP_LAUNCHER_AUTOPLAY='1')
        for which in ('main', 'sdk'):
            other = real['sdk' if which == 'main' else 'main']
            if which == 'sdk' and not other.exists():
                print('skip sdk cases: no whole real main file at', other)
                continue
            good_other = other if other.exists() else work / 'other.bin'
            if not good_other.exists():
                good_other.write_bytes(head)
            for name, data in fixtures.items():
                bad = work / 'bad.bin'
                bad.write_bytes(data)
                main_elf, sdk_elf = (bad, good_other) if which == 'main' else (good_other, bad)
                (game / 'config.json').write_text(json.dumps({
                    'main_elf': str(main_elf), 'sdk_elf': str(sdk_elf), 'romfs_root': str(work / 'romfs'), 'data_root': str(work / 'data'), 'upscalers_offered': True}))
                label = '%s file, %s' % (which, name)
                for mode in ('launcher', 'cli'):
                    cmd = [str(game / 'SnapRecomp.exe')] if mode == 'launcher' else [str(game / 'SnapRecomp.exe'), '--main', str(main_elf), '--sdk', str(sdk_elf), 'run']
                    try:
                        r = subprocess.run(cmd, cwd=game, env=scratch_env, capture_output=True, text=True, timeout=60, creationflags=0x4000)
                    except subprocess.TimeoutExpired:
                        print('FAIL [%s] %s: timed out' % (mode, label))
                        fails += 1
                        continue
                    err = r.stderr.strip()
                    crashed = (r.returncode & 0xFFFFFFFF) in CRASH_CODES
                    # the launcher checks the file it is given; in the CLI run the loader checks main first, so the sdk case with a good main
                    # also reaches the sdk check
                    ok = r.returncode == 2 and not crashed and ('The %s file' % which in err) and 'CRASH' not in err
                    if not ok:
                        print('FAIL [%s] %s: exit %#x stderr: %s' % (mode, label, r.returncode & 0xFFFFFFFF, err[:300]))
                        fails += 1
                    else:
                        print('ok   [%s] %s: %s' % (mode, label, err.splitlines()[-1][:110]))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('bad_game_files_test:', 'all passed' if not fails else '%d FAILED' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
