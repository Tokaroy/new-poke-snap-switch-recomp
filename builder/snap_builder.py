"""Snap Recomp Builder, command line.

  python builder/snap_builder.py check --main <main> --sdk <sdk> --romfs <folder>
  python builder/snap_builder.py build --main <main> --sdk <sdk> --romfs <folder> --work <new empty folder> [options]

See builder/README.md. Exit code 0 = success, 1 = a step failed or the files are the wrong version, 2 = bad arguments.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapbuild import pipeline, versioncheck  # noqa: E402
from snapbuild.util import Log, StepError  # noqa: E402


def add_inputs(ap):
    ap.add_argument('--main', required=True, help='the game\'s "main" NSO file (compressed or not)')
    ap.add_argument('--sdk', required=True, help='the game\'s "sdk" NSO file')
    ap.add_argument('--romfs', required=True, help='folder with the extracted romfs of game version 2.0.1 (base game plus update)')


def main(argv=None):
    ap = argparse.ArgumentParser(description='Builds SnapRecomp.exe from your own game files.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check', help='only check that the files are the right game version')
    add_inputs(c)
    b = sub.add_parser('build', help='run the whole build')
    add_inputs(b)
    b.add_argument('--work', required=True, help='work folder (new and empty, or a previous work folder of this builder to resume)')
    b.add_argument('--maps', help='folder with functions_final.tsv and jump_tables.tsv; skips the Ghidra step')
    b.add_argument('--llvm', help='LLVM bin folder (clang, clang++, lld, llvm-ar)')
    b.add_argument('--python', help='Python with capstone, pyelftools, numpy (and lz4) for the lifter')
    b.add_argument('--vulkan-lib', help='path of vulkan-1.lib')
    b.add_argument('--ghidra', help='Ghidra 12.1.2 folder (with the Switch loader extension); needed without --maps')
    b.add_argument('--jdk', help='JDK 21 folder for Ghidra')
    b.add_argument('--prebuilt', help='folder with the prebuilt runtime objects (default: the one shipped with the builder)')
    b.add_argument('--jobs', type=int, default=0, help='parallel compile jobs (default: from CPUs and RAM, at most 8)')
    b.add_argument('--skip-smoke', action='store_true', help='do not start the game at the end')
    b.add_argument('--harness', help='ShaderHarness.dll for the smoke run')
    b.add_argument('--ghost', help='developer use: hidden-desktop launcher script for the smoke run')
    b.add_argument('--smoke-timeout', type=int, default=420, help='seconds to wait for the title screen')
    b.add_argument('--force', action='append', default=[], choices=pipeline.STEPS, help='rerun this step even if it is stamped done')
    b.add_argument('--compare-generated', help='developer use: folder (with main and sdk inside) whose generated C must be identical')
    a = ap.parse_args(argv)

    if a.cmd == 'check':
        table = versioncheck.load_table(pipeline.BUILDER / 'versions.json')
        r = versioncheck.check_all(a.main, a.sdk, a.romfs, table)
        for n in r.notes:
            print('note:', n)
        if r.ok:
            print('OK: main, sdk and romfs are game version', versioncheck.EXPECTED_NAME)
            return 0
        print('The files do not match:')
        for p in r.problems:
            print(' -', p)
        return 1

    work = Path(a.work)
    if work.exists() and any(work.iterdir()) and not (work / 'stamps').exists() and not (work / 'builder.log').exists():
        print(f'The work folder {work} is not empty and was not made by this builder. Use a new empty folder.')
        return 2
    work.mkdir(parents=True, exist_ok=True)
    log = Log(work / 'builder.log')
    opt = pipeline.Options(**{k: v for k, v in vars(a).items() if k != 'cmd'})
    try:
        exe, smoke = pipeline.build(opt, log)
    except StepError as e:
        log.write('FAILED: ' + str(e))
        print(f'\nThe build stopped. Log: {log.path}')
        return 1
    except Exception:   # a bug in the builder: keep the traceback in the log
        import traceback
        log.write('INTERNAL ERROR: ' + traceback.format_exc())
        print('The builder hit an unexpected error. Details are in the log: ' + str(log.path))
        return 1
    finally:
        log.close()
    if smoke is not None and not smoke[0]:
        print(f'Built {exe}, but the smoke run did not reach the title screen: {smoke[1]}')
        return 1
    print(f'Done: {exe}' + (f' ({smoke[1]})' if smoke else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
