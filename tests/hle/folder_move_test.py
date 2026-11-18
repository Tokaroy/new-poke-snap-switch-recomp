"""A moved install still starts: config.json next to the exe follows the folder (host/portable.cpp snap_config_rebase).

Lays out an install the way the builder leaves it (<root>\\work\\out with the exe, config.json holding absolute paths plus install_root and
exe_rel, portable.txt), starts the exe as the launcher once (SNAP_LAUNCHER_AUTOPLAY=1: no window, no game), moves the whole folder,
and starts it again. Checks that every stored path under the old root now points into the new one, that paths outside the root and the
launcher's own settings are left alone, that a staged build (work\\builds\\<id>) keeps its paths, and that the portable saves moved along.

Usage: tools/venv-recomp/Scripts/python -I tests/hle/folder_move_test.py [path\\to\\snap_boot.exe]
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


def main():
    if not EXE.exists():
        print('exe not found:', EXE)
        return 2
    t = Path(tempfile.mkdtemp(prefix='snap_move_'))
    fails = 0
    try:
        def check(label, ok, got=''):
            nonlocal fails
            print('%s %s' % ('ok  ' if ok else 'FAIL', label))
            if not ok:
                fails += 1
                print('     got: %s' % str(got).strip()[:700])

        def conf_for(root, dump):
            return {'romfs_root': str(root / 'game' / 'romfs'), 'main_elf': str(root / 'work' / 'elf' / 'main.elf'),
                    'sdk_elf': str(root / 'work' / 'elf' / 'sdk.elf'), 'builder_version': '0.0.0-test',
                    'builder_dir': str(root / 'setup' / 'app' / '0.0.0-test' / 'builder'), 'python': str(root / 'tools' / 'python' / 'python.exe'),
                    'work': str(root / 'work'), 'install_root': str(root), 'exe_rel': 'work\\out', 'upscalers_offered': True,
                    'update_check': False, 'dump_seen': str(dump), 'lookalike': str(root) + 'shots\\x'}

        def lay_out(root, dump, where='out'):
            exe_dir = root / 'work' / where
            exe_dir.mkdir(parents=True)
            shutil.copy2(EXE, exe_dir / 'SnapRecomp.exe')
            for dll in EXE.parent.glob('*.dll'):
                shutil.copy2(dll, exe_dir / dll.name)
            (root / 'game' / 'romfs').mkdir(parents=True, exist_ok=True)
            (root / 'work' / 'elf').mkdir(parents=True, exist_ok=True)
            for n in ('main.elf', 'sdk.elf'):
                (root / 'work' / 'elf' / n).write_bytes(b'')
            (exe_dir / 'config.json').write_text(json.dumps(conf_for(root, dump), indent=1), encoding='utf-8')
            (exe_dir / 'portable.txt').write_text('')
            return exe_dir

        def run(exe_dir):
            env = dict(os.environ, APPDATA=str(t / 'real_appdata'), LOCALAPPDATA=str(t / 'real_local'), SNAP_LAUNCHER_AUTOPLAY='1')
            for k in ('SNAP_SAFE_MODE', 'SNAP_DATA_DIR', 'SNAP_PORTABLE', 'SNAP_REAL_APPDATA', 'SNAP_REAL_LOCALAPPDATA', 'SNAP_CONFIG_DIR', 'SNAP_PROFILES_DIR', 'SNAP_SAVE_DIR'):
                env.pop(k, None)
            r = subprocess.run([str(exe_dir / 'SnapRecomp.exe')], cwd=exe_dir, env=env, capture_output=True, text=True, timeout=60, creationflags=0x08000000)
            return r.returncode, r.stderr + r.stdout

        dump = t / 'my dump'
        dump.mkdir()

        # 1. where it was built: nothing to rewrite
        a = t / 'Games' / 'Snap Recomp'
        out = lay_out(a, dump)
        before = (out / 'config.json').read_bytes()
        code, text = run(out)
        check('unmoved folder: config.json is not touched', (out / 'config.json').read_bytes() == before and 'was moved' not in text, text)
        save = out / 'portable_data' / 'Roaming' / 'SnapRecomp'
        check('unmoved folder: portable data folder made next to the exe', (out / 'portable_data' / 'Roaming').is_dir(), text)
        save.mkdir(parents=True, exist_ok=True)
        (save / 'settings.cfg').write_text('fps_overlay=fps\n')

        # 2. the whole folder moved (another parent, another name, a non-ASCII character in the path)
        b = t / 'Elsewhere' / 'Snäp moved'
        b.parent.mkdir()
        shutil.move(str(a), str(b))
        out = b / 'work' / 'out'
        code, text = run(out)
        c = json.loads((out / 'config.json').read_text(encoding='utf-8'))
        want = conf_for(b, dump)
        want['lookalike'] = str(a) + 'shots\\x'   # a longer folder name that only starts like the old root is someone else's
        check('moved folder: every path under the old root follows', {k: c.get(k) for k in want} == want, c)
        check('moved folder: said so once', 'was moved' in text, text)
        check('moved folder: the paths exist', all(Path(c[k]).exists() for k in ('romfs_root', 'main_elf', 'sdk_elf', 'work')), c)
        check('moved folder: the old place is not named anywhere in config.json', str(a) not in json.dumps({k: v for k, v in c.items() if k != 'lookalike'}), c)
        check('moved folder: the saves came along', (out / 'portable_data' / 'Roaming' / 'SnapRecomp' / 'settings.cfg').read_text() == 'fps_overlay=fps\n')
        check('moved folder: no temporary file left', not (out / 'config.json.moving').exists())
        again = (out / 'config.json').read_bytes()
        code, text = run(out)
        check('second start after the move: nothing more to do', (out / 'config.json').read_bytes() == again and 'was moved' not in text, text)

        # 3. a staged build (work\builds\<id>) is not the installed place: its config.json keeps the paths it was built with
        s = t / 'Staged'
        st = lay_out(s, dump, where='builds\\b1')
        cfgp = st / 'config.json'
        cfg = json.loads(cfgp.read_text(encoding='utf-8'))
        cfg['install_root'] = str(t / 'SomewhereElse')
        cfgp.write_text(json.dumps(cfg), encoding='utf-8')
        before = cfgp.read_bytes()
        code, text = run(st)
        check('staged build: left alone', cfgp.read_bytes() == before and 'was moved' not in text, text)

        # 4. a config.json from before this (no install_root): left alone
        o = t / 'Old'
        od = lay_out(o, dump)
        cfg = json.loads((od / 'config.json').read_text(encoding='utf-8'))
        del cfg['install_root'], cfg['exe_rel']
        (od / 'config.json').write_text(json.dumps(cfg), encoding='utf-8')
        shutil.move(str(o), str(t / 'Old2'))
        od = t / 'Old2' / 'work' / 'out'
        before = (od / 'config.json').read_bytes()
        code, text = run(od)
        check('older config.json without install_root: left alone', (od / 'config.json').read_bytes() == before, text)
    finally:
        shutil.rmtree(t, ignore_errors=True)
    print('folder move test: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
