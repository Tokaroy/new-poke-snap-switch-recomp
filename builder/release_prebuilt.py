"""Release build script (developer use): compiles our runtime, host, mods, ImGui and upscaler glue to plain COFF objects for the builder.

  tools/venv-recomp/Scripts/python -I builder/release_prebuilt.py [--out build/release/prebuilt] [--jobs 8] [--romfs <folder>]

1. Checks the ABI guard (the MAIN_INLINE_HOOKS and SDK_HOOKS tables against builder/abi_tables.json: change them only together with
   a SNAP_RUNTIME_ABI bump in runtime/cpu.h).
2. host/build_snap.py --release-objs: non-LTO, non-PGO, no debug info, no baked harness path, nvn_cache under %LOCALAPPDATA%.
3. Stages obj/, include/ (the headers the generated C includes), lib/, bin/ (listed DLLs), res/, licenses/ and manifest.json.
4. Runs builder/leak_scan.py on the staged folder (terms file: build/release/leak_terms.txt, outside git).
No game data goes in. The staging folder is git-ignored (build/).
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
LLVM = ROOT / 'tools' / 'llvm' / 'bin'
SDL = ROOT / 'tools' / 'dl_sdl3' / 'x' / 'SDL3-3.4.20'
FFX_DLL = ROOT / 'tools' / 'amd' / 'ffx_sdk_v1.1.4' / 'PrebuiltSignedDLL' / 'amd_fidelityfx_vk.dll'
HEADERS = ('cpu.h', 'simd.h', 'simd_fp.h')


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def abi_guard(update):
    import snap_gen
    m = (ROOT / 'runtime' / 'cpu.h').read_text(encoding='utf-8')
    import re
    abi = int(re.search(r'^#define SNAP_RUNTIME_ABI (\d+)', m, re.M).group(1))
    tables = hashlib.sha256(json.dumps([sorted(snap_gen.MAIN_INLINE_HOOKS.items()), sorted(snap_gen.SDK_HOOKS.items())]).encode()).hexdigest()
    f = ROOT / 'builder' / 'abi_tables.json'
    old = json.loads(f.read_text(encoding='utf-8')) if f.exists() else None
    if old and old['tables_sha256'] == tables:
        return abi
    if old and old['runtime_abi'] == abi:
        sys.exit('MAIN_INLINE_HOOKS or SDK_HOOKS (recomp/snap_gen.py) changed but SNAP_RUNTIME_ABI in runtime/cpu.h did not: bump it, '
                 f'then run this script again (it records the new table hash in {f.name}).')
    if update or old is None or old['runtime_abi'] != abi:
        f.write_text(json.dumps({'runtime_abi': abi, 'tables_sha256': tables}, indent=1) + '\n', encoding='utf-8')
        print(f'recorded runtime ABI {abi} and the hook-table hash in {f}')
    return abi


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'release' / 'prebuilt')
    ap.add_argument('--work', type=Path, default=ROOT / 'build' / 'release' / 'objwork')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--rebuild', action='store_true')
    ap.add_argument('--romfs', default=str(ROOT / 'extracted' / 'romfs-v262144'), help='romfs folder for the game-file-name rule of the leak scan')
    a = ap.parse_args()
    abi = abi_guard(False)

    cmd = [sys.executable, '-I', str(ROOT / 'host' / 'build_snap.py'), '--release-objs', str(a.work), '--jobs', str(a.jobs)]
    if a.rebuild:
        cmd.append('--rebuild')
    r = subprocess.run(cmd, creationflags=0x4000)
    if r.returncode:
        return r.returncode

    out = a.out
    if out.exists():
        shutil.rmtree(out)
    for d in ('obj', 'include', 'lib', 'bin', 'res', 'licenses'):
        (out / d).mkdir(parents=True)
    objs = sorted((a.work / 'obj').glob('*.obj'))
    for o in objs:
        shutil.copyfile(o, out / 'obj' / o.name)
    for h in HEADERS:
        shutil.copyfile(ROOT / 'runtime' / h, out / 'include' / h)
    shutil.copyfile(ROOT / 'host' / 'snap_utf8.manifest', out / 'res' / 'snap_utf8.manifest')
    # the original fallback icon (SNAP_ICON_SRC=none: never the game's own icon)
    subprocess.check_call([sys.executable, '-I', str(ROOT / 'host' / 'make_icon.py'), '--out', str(out / 'res')],
                          env=dict(os.environ, SNAP_ICON_SRC='none'))
    shutil.copyfile(SDL / 'lib' / 'x64' / 'SDL3.lib', out / 'lib' / 'SDL3.lib')
    dll_src = {'SDL3.dll': SDL / 'lib' / 'x64' / 'SDL3.dll', 'amd_fidelityfx_vk.dll': FFX_DLL}
    for n, p in dll_src.items():
        shutil.copyfile(p, out / 'bin' / n)
    shutil.copyfile(SDL / 'LICENSE.txt', out / 'licenses' / 'SDL3-LICENSE.txt')
    ffx_lic = FFX_DLL.parents[1] / 'LICENSE.txt'
    if ffx_lic.exists():
        shutil.copyfile(ffx_lic, out / 'licenses' / 'FidelityFX-LICENSE.txt')
    r = subprocess.run([sys.executable, '-I', str(ROOT / 'builder' / 'release_harness.py'), '--out', str(out)], creationflags=0x4000)
    if r.returncode:
        return r.returncode
    clang = subprocess.run([str(LLVM / 'clang.exe'), '--version'], capture_output=True, text=True).stdout.splitlines()[0]
    manifest = {
        'format': 1,
        'runtime_abi': abi,
        'compiler': clang,
        'flags': 'non-LTO, non-PGO, -O2 -march=x86-64-v3 -fno-strict-aliasing, no debug info, SNAP_RELEASE_BUILD',
        'objs': [{'name': o.name, 'sha256': sha(out / 'obj' / o.name)} for o in objs],
        'libs': ['SDL3.lib'],
        'dlls': [{'name': n, 'path': 'bin/' + n, 'sha256': sha(out / 'bin' / n), 'license': lic} for n, lic in
                 (('SDL3.dll', 'zlib (licenses/SDL3-LICENSE.txt)'), ('amd_fidelityfx_vk.dll', 'MIT (licenses/FidelityFX-LICENSE.txt)'))],
        'harness': 'shader_harness',
    }
    hroot = out / 'shader_harness'
    for f in sorted(hroot.rglob('*')):
        if f.is_file() and f.suffix.lower() in ('.dll', '.exe'):
            manifest['dlls'].append({'name': f.name, 'path': f.relative_to(out).as_posix(), 'sha256': sha(f),
                                     'license': 'MIT (Ryujinx translator, .NET runtime; see shader_harness/NOTICE.txt)'})
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8')
    total = sum(f.stat().st_size for f in out.rglob('*') if f.is_file())
    print(f'staged {len(objs)} objects, {len(HEADERS)} headers, {len(manifest["dlls"])} DLLs: {total / 2**20:.1f} MB in {out}')
    scan = [sys.executable, '-I', str(ROOT / 'builder' / 'leak_scan.py'), str(out)]
    if a.romfs and Path(a.romfs).exists():
        scan += ['--romfs', a.romfs]
    return subprocess.run(scan).returncode


if __name__ == '__main__':
    sys.exit(main())
