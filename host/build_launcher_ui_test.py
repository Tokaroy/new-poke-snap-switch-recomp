"""Builds host/launcher_ui_test.exe (the themed launcher window alone, for look and feel work) into build/launcher_ui. BelowNormal.
Usage: tools/venv-recomp/Scripts/python -I host/build_launcher_ui_test.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host'))
from build_snap import vc_env  # noqa: E402

LLVM = ROOT / 'tools' / 'llvm' / 'bin'
BELOW_NORMAL = 0x4000


def main():
    out = ROOT / 'build' / 'launcher_ui'
    out.mkdir(parents=True, exist_ok=True)
    imgui = ROOT / 'runtime' / 'ui' / 'third_party' / 'imgui'
    env = vc_env()
    srcs = [ROOT / 'host' / 'launcher_ui.cpp', ROOT / 'host' / 'launcher_ui_test.cpp'] + [imgui / f for f in (
        'imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'imgui_impl_dx11.cpp', 'imgui_impl_win32.cpp')]
    objs = []
    for s in srcs:
        o = out / (s.stem + '.obj')
        r = subprocess.run([str(LLVM / 'clang++.exe'), '-std=c++17', '-O1', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'host'),
                            '-I', str(imgui), '-c', str(s), '-o', str(o)], env=env, creationflags=BELOW_NORMAL)
        if r.returncode:
            return 1
        objs.append(str(o))
    exe = out / 'launcher_ui_test.exe'
    r = subprocess.run([str(LLVM / 'clang++.exe'), '-fuse-ld=lld', '-o', str(exe)] + objs + ['-luser32', '-lgdi32', '-ld3d11', '-ldxgi', '-ld3dcompiler', '-lshell32', '-ldwmapi'], env=env, creationflags=BELOW_NORMAL)
    print('built', exe if r.returncode == 0 else 'FAILED')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
