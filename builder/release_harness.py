"""Release build (developer use): self-contained publish of the shader harness into the prebuilt staging folder.

  python -I builder/release_harness.py [--ryujinx <translator source tree>] [--out build/release/prebuilt]

Publishes scripts/shader_harness (our code) against the Ryubing Ryujinx shader translator (MIT) for win-x64, self-contained, so
the user needs no .NET SDK or runtime. The result goes to <out>/shader_harness/ (ShaderHarness.exe is what the game starts), with
our GPL-3.0 license and section 7 permission, the translator's MIT license, and a notice file. Run builder/release_prebuilt.py first (it recreates <out>), then this script,
then update the manifest with `release_prebuilt.py --manifest-only`.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--ryujinx', type=Path, default=ROOT / 'tools' / 'ref' / 'ryubing-ryujinx')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'release' / 'prebuilt')
    ap.add_argument('--work', type=Path, default=ROOT / 'build' / 'release' / 'harness_work')
    a = ap.parse_args()
    src = ROOT / 'scripts' / 'shader_harness'
    if a.work.exists():
        shutil.rmtree(a.work)
    a.work.mkdir(parents=True)
    for f in ('Program.cs', 'Directory.Packages.props'):
        shutil.copyfile(src / f, a.work / f)
    csproj = (src / 'ShaderHarness.csproj').read_text(encoding='utf-8')
    # own project copy: no baked output folders, translator tree from --ryujinx
    keep = [l for l in csproj.splitlines() if '<BaseOutputPath' not in l and '<BaseIntermediateOutputPath' not in l]
    (a.work / 'ShaderHarness.csproj').write_text('\n'.join(keep), encoding='utf-8')
    dest = a.out / 'shader_harness'
    if dest.exists():
        shutil.rmtree(dest)
    cmd = ['dotnet', 'publish', str(a.work / 'ShaderHarness.csproj'), '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true',
           '-p:PublishSingleFile=false', '-p:DebugType=none', '-p:DebugSymbols=false', '-p:RyujinxDir=' + str(a.ryujinx),
           '-o', str(dest), '--nologo', '-v', 'q']
    r = subprocess.run(cmd, creationflags=0x4000)
    if r.returncode:
        return r.returncode
    lic = a.ryujinx / 'LICENSE.txt'
    (a.out / 'licenses').mkdir(parents=True, exist_ok=True)
    if lic.exists():
        shutil.copyfile(lic, a.out / 'licenses' / 'Ryujinx-LICENSE.txt')
        shutil.copyfile(lic, dest / 'LICENSE-Ryujinx.txt')
    shutil.copyfile(ROOT / 'LICENSE', dest / 'LICENSE-SnapRecomp.txt')
    shutil.copyfile(ROOT / 'LICENSE-MIT', dest / 'LICENSE-SnapRecomp-MIT.txt')
    (dest / 'NOTICE.txt').write_text(
        'This folder holds a self-contained build of Snap Recomp\'s shader harness.\n'
        'Our harness code from v1.2.0 is GPL-3.0 with the section 7 DLSS/Streamline/XeSS permission in LICENSE-SnapRecomp.txt.\n'
        'Releases up to v1.1.0 were MIT; that text is in LICENSE-SnapRecomp-MIT.txt. Existing MIT component notices remain applicable.\n'
        'It links the Ryubing fork of the Ryujinx shader translator (Ryujinx.Graphics.Shader), MIT license, kept in LICENSE-Ryujinx.txt.\n'
        'The .NET runtime files are Microsoft\'s (MIT license, https://github.com/dotnet/runtime).\n', encoding='utf-8')
    exe = dest / 'ShaderHarness.exe'
    n = sum(1 for f in dest.rglob('*') if f.is_file())
    size = sum(f.stat().st_size for f in dest.rglob('*') if f.is_file())
    print(f'published {n} files, {size / 2**20:.0f} MB: {exe}')
    return 0 if exe.exists() else 1


if __name__ == '__main__':
    sys.exit(main())
