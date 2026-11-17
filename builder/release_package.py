"""Build the release zip of the builder (developer tool; run after release_prebuilt.py).

  python -I builder/release_package.py [--prebuilt build/release/prebuilt] [--out build/release] [--version 1.0.0]

Layout of SnapRecomp-Builder-v<version>-win-x64.zip (the builder finds its own files relative to the zip root, the same as the repository):

  README.md                     the user guide (builder/release/README.md)
  LICENSE, THIRD_PARTY_NOTICES.md
  licenses/                     license texts of the third-party binaries in prebuilt/
  builder/                      the builder (no tests, no developer tools)
  recomp/                       the AArch64-to-C generator and the compile driver
  scripts/                      nso2elf.py, reconcile_functions.py, ghidra/*.java used by the function-list step
  prebuilt/                     the compiled runtime objects, import library, headers, shader harness

The zip holds no game data and no vendor upscaler files. It is checked with builder/leak_scan.py before release; this script runs
that check and deletes the zip if it fails. Nothing is uploaded or published here."""
import argparse
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'builder'))
from snapbuild import pipeline  # noqa: E402
import ci_gate  # noqa: E402

BUILDER_SKIP_DIRS = {'__pycache__', 'tests', 'dev'}
BUILDER_SKIP_FILES = {'release_prebuilt.py', 'release_harness.py', 'release_sign.py', 'release_package.py', 'ci_gate.py', 'leak_scan.py',
                      'leak_allow_names.txt', 'leak_allow_content.txt', 'abi_tables.json'}
GHIDRA_SCRIPTS = ('SeedFdeStarts.java', 'DumpAnalysisOptions.java', 'ExportMaps.java')
SCRIPTS = ('nso2elf.py', 'reconcile_functions.py')
RECOMP = tuple(pipeline.LIFTER_FILES) + ('snap_build.py',)


def collect(prebuilt, recomp_dir=None):
    """(source path, name in the zip) for every file in the package, sorted. recomp_dir: take the recomp/ files from there
    instead of the tree (for a package whose prebuilt objects are older than the tree, e.g. `git archive <rev> recomp`)."""
    rdir = Path(recomp_dir) if recomp_dir else ROOT / 'recomp'
    items = []
    for p in sorted((ROOT / 'builder').rglob('*')):
        rel = p.relative_to(ROOT / 'builder')
        if p.is_dir() or set(rel.parts[:-1]) & BUILDER_SKIP_DIRS or (len(rel.parts) == 1 and p.name in BUILDER_SKIP_FILES):
            continue
        if p.suffix == '.pyc':
            continue
        items.append((p, 'builder/' + rel.as_posix()))
    for f in RECOMP:
        items.append((rdir / f, 'recomp/' + f))
    for f in SCRIPTS:
        items.append((ROOT / 'scripts' / f, 'scripts/' + f))
    for f in GHIDRA_SCRIPTS:
        items.append((ROOT / 'scripts' / 'ghidra' / f, 'scripts/ghidra/' + f))
    for p in sorted(Path(prebuilt).rglob('*')):
        if p.is_file():
            items.append((p, 'prebuilt/' + p.relative_to(prebuilt).as_posix()))
    items.append((ROOT / 'builder' / 'release' / 'README.md', 'README.md'))
    for p in sorted((ROOT / 'build' / 'release' / 'shots').glob('*.png')):   # real screenshots the README shows
        items.append((p, 'screenshots/' + p.name))
    items.append((ROOT / 'LICENSE', 'LICENSE'))
    items.append((ROOT / 'LICENSE-MIT', 'LICENSE-MIT'))
    items.append((ROOT / 'THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md'))
    for p in sorted(Path(prebuilt, 'licenses').glob('*')):
        items.append((p, 'licenses/' + p.name))
    names = [n for _, n in items]
    dup = {n for n in names if names.count(n) > 1}
    # README.md is both builder/README.md (developer text) and the root guide: the root one wins, builder/README.md stays as the reference
    if dup:
        raise SystemExit('duplicate names in the package: ' + ', '.join(sorted(dup)))
    missing = [str(p) for p, _ in items if not p.is_file()]
    if missing:
        raise SystemExit('missing files: ' + ', '.join(missing))
    return items


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--prebuilt', default=str(ROOT / 'build' / 'release' / 'prebuilt'))
    ap.add_argument('--out', default=str(ROOT / 'build' / 'release'))
    ap.add_argument('--version')
    ap.add_argument('--recomp-dir', help='take recomp/*.py from this folder instead of the tree (matches an older prebuilt)')
    ap.add_argument('--romfs', help='romfs folder for the game-file-name check of the leak scan (read only)')
    ci_gate.add_args(ap)
    a = ap.parse_args()
    ci_gate.gate(a, 'release_package')
    version = a.version or (ROOT / 'builder' / 'VERSION').read_text(encoding='utf-8').strip()
    if version != (ROOT / 'builder' / 'VERSION').read_text(encoding='utf-8').strip():
        raise SystemExit('--version must match builder/VERSION (the builder reports that file)')
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    zp = out / f'SnapRecomp-Builder-v{version}-win-x64.zip'
    items = collect(a.prebuilt, a.recomp_dir)
    tmp = zp.with_name(zp.name + '.part')
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p, name in items:
            z.write(p, name)
    tmp.replace(zp)
    print(f'{zp} {zp.stat().st_size / 1e6:.1f} MB, {len(items)} files')
    cmd = [sys.executable, '-I', str(ROOT / 'builder' / 'leak_scan.py'), str(zp), '--strip-prefix', 'prebuilt/',
           '--manifest', str(Path(a.prebuilt) / 'manifest.json')]
    if a.romfs:
        cmd += ['--romfs', a.romfs]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        zp.unlink()
        raise SystemExit('leak scan failed; the zip was deleted')
    print('leak scan clean')


if __name__ == '__main__':
    main()
