"""Snap Recomp Builder, command line.

  python builder/snap_builder.py check --main <main> --sdk <sdk> --romfs <folder>
  python builder/snap_builder.py build --main <main> --sdk <sdk> --romfs <folder> --work <new empty folder> [options]

See builder/README.md. Exit code 0 = success, 1 = a step failed or the files are the wrong version, 2 = bad arguments.
"""
import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapbuild import fetch, pipeline, pyenv, versioncheck  # noqa: E402
from snapbuild.util import Log, StepError  # noqa: E402


def add_inputs(ap):
    ap.add_argument('--main', required=True, help='the game\'s "main" NSO file (compressed or not)')
    ap.add_argument('--sdk', required=True, help='the game\'s "sdk" NSO file')
    ap.add_argument('--romfs', required=True, help='folder with the extracted romfs of game version 2.0.1 (base game plus update)')


def run_update(a):
    import json
    from snapbuild import update
    state = Path(a.state_dir) if a.state_dir else update.default_state_dir()
    work = Path(a.work) if a.work else None
    try:
        if a.action == 'check':
            if a.startup:
                r = update.startup_check(pipeline.BUILDER, state, a.config or (work / 'out' / 'config.json' if work else ''), work=work, url=a.release_url)
                r = r if r is not None else {'checked': False, 'disabled': True, 'update_available': False}
            else:
                r = update.check(pipeline.BUILDER, state, work=work, url=a.release_url)
            if a.json:
                print(json.dumps(r))
            elif r.get('error'):
                print('Could not check: ' + r['error'])
            elif r.get('update_available'):
                est = r.get('estimate')
                print(f'Version {r["latest"]} is available (you have {r["current"]}). Download {r["size_text"]}'
                      + (f', rebuild about {est["low_min"]} to {est["high_min"]} minutes' if est else '') + '.')
                print(update.clean_text(r.get('notes', '')))
            else:
                print(f'You are up to date (version {r["current"]}).')
            return 0
        if not work:
            print('--work is required')
            return 2
        log = Log(work / 'update.log')
        try:
            if a.action == 'rollback':
                r = update.rollback(work, state, log, wait_pid=a.wait_pid)
                print(f'Rolled back: the build in {work / "out"} is version {r["now"]} again.')
                return 0

            def confirm(info):
                if a.yes:
                    return True
                est = info.get('estimate')
                print(f'Install version {info["version"]}?\nDownload {update.human_size(info["size"])}'
                      + (f', rebuild about {est[0]} to {est[1]} minutes' if est else '') + '.\n' + update.clean_text(info['notes']))
                if not sys.stdin or not sys.stdin.isatty():   # no terminal: nobody can answer, so the answer is no at once
                    print('No terminal to answer on, so nothing was installed. Pass --yes to install.')
                    return False
                return fetch.ask_yes('Install it?')
            r = update.apply(pipeline.BUILDER, work, state, url=a.release_url, log=log, confirm=confirm, wait_pid=a.wait_pid,
                             progress=lambda d, t: print(f'\rdownloading {d * 100 // max(t, 1)}%', end='', flush=True))
        finally:
            log.close()
        print()
        if r.get('updated'):
            print(f'Updated {r["from"]} -> {r["to"]}. The earlier build is kept for rollback.')
        elif r.get('declined'):
            print('Nothing was changed.')
        else:
            print(f'Already up to date (version {r["current"]}).')
        return 0
    except update.UpdateError as e:
        print('Update stopped: ' + str(e))
        return 1


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
    b.add_argument('--no-cache', dest='cache', action='store_false', help='do not reuse or store function lists and generated C in the private cache')
    b.add_argument('--cache-dir', help='private cache folder (default %%LOCALAPPDATA%%\\SnapRecomp\\cache)')
    b.add_argument('--force', action='append', default=[], choices=pipeline.STEPS, help='rerun this step even if it is stamped done')
    b.add_argument('--compare-generated', help='developer use: folder (with main and sdk inside) whose generated C must be identical')
    f = sub.add_parser('fetch', help='download, verify and unpack the pinned tools (asks before every download)')
    f.add_argument('tools', nargs='*', help='tool names (llvm ghidra switch_loader jdk python_embed); none = all of them')
    f.add_argument('--yes', action='store_true', help='answer yes to every download question')
    f.add_argument('--tools-dir', help='where to unpack (default %%LOCALAPPDATA%%\\SnapRecomp\\tools)')
    f.add_argument('--offline-dir', action='append', default=[], help='folder with already downloaded files (hash checked, no network)')
    f.add_argument('--list', action='store_true', help='only list the pins and whether each one is complete')
    p = sub.add_parser('pyenv', help='build the Python environment (capstone, pyelftools, numpy, lz4) from the pinned wheel hashes')
    p.add_argument('--yes', action='store_true', help='answer yes to every download question')
    p.add_argument('--tools-dir', help='where to build (default %%LOCALAPPDATA%%\\SnapRecomp\\tools)')
    p.add_argument('--offline-dir', action='append', default=[], help='folder with already downloaded files (hash checked, no network)')
    p.add_argument('--base-python', help='make a venv of this installed Python 3.14 instead of using the pinned embeddable Python')
    u = sub.add_parser('upscalers', help='fetch AMD FSR, NVIDIA DLSS or Intel XeSS from the vendor and install it next to SnapRecomp.exe')
    u.add_argument('names', nargs='*', help='fsr dlss xess (none = the defaults: fsr)')
    u.add_argument('--install-dir', required=True, help='the folder with SnapRecomp.exe (the "out" folder of a build)')
    u.add_argument('--accept-license', action='append', default=[], help='name of an upscaler whose vendor license you have read and accept (repeatable); without it you are asked')
    u.add_argument('--yes', action='store_true', help='download without asking (does NOT accept any license)')
    u.add_argument('--offline-dir', action='append', default=[], help='unused for now; downloads are cached in a temporary folder')
    u.add_argument('--list', action='store_true', help='show what is installed')
    up = sub.add_parser('update', help='check for a newer builder release, install it (rebuilds here, switches only if the new build passes), or roll back')
    up.add_argument('action', choices=('check', 'apply', 'rollback'))
    up.add_argument('--work', help='the work folder of your build (holds out\\ and the record of the last build)')
    up.add_argument('--state-dir', help='update state folder (default %%LOCALAPPDATA%%\\SnapRecomp\\updates)')
    up.add_argument('--release-url', help='developer use: a release JSON (file:// or https) instead of the GitHub release list')
    up.add_argument('--json', action='store_true', help='check: print the answer as one JSON line')
    up.add_argument('--startup', action='store_true', help='check: the launcher\'s start-up check (at most once per 24 hours, nothing when switched off, silent on errors)')
    up.add_argument('--config', help='--startup: the launcher\'s config.json (holds the update_check switch)')
    up.add_argument('--yes', action='store_true', help='apply: install without the confirmation question')
    up.add_argument('--wait-pid', type=int, help='wait for this process (the launcher) to exit first')
    b.add_argument('--upscalers', help='comma list of upscalers to fetch after the build (default: fsr); a license must be accepted with --accept-license')
    b.add_argument('--accept-license', action='append', default=[], help='upscaler whose vendor license you accept: fsr, dlss, xess (repeatable)')
    hdtex_cli = None
    hdtex_missing = ''
    import importlib.util
    if importlib.util.find_spec('snapbuild.hdtex_cli') is not None:   # absent from the v1 release package: no subcommand at all
        hd = sub.add_parser('hdtex', help="HD AI textures: upscale your own game's textures on this PC with a local AI model (builder/HD-TEXTURES.md)")
        try:
            from snapbuild import hdtex_cli
            hdtex_cli.add_args(hd)
        except ImportError as e:   # numpy etc. are only needed by hdtex; build/update/fetch must not need them
            hdtex_cli = None
            hdtex_missing = str(e)
            hd.add_argument('rest', nargs='*')
    a = ap.parse_args(argv)
    if a.cmd == 'build':
        a.upscalers = [x for x in a.upscalers.split(',') if x] if a.upscalers is not None else None

    if a.cmd == 'update':
        return run_update(a)

    if a.cmd == 'hdtex':
        if hdtex_cli is None:
            print('hdtex needs the Python environment from "snap_builder.py pyenv" (' + hdtex_missing + ').')
            return 1
        return hdtex_cli.main(a, versioncheck.load_table(pipeline.BUILDER / 'versions.json'))

    if a.cmd == 'upscalers':
        from snapbuild import upscalers as up
        table = versioncheck.load_table(pipeline.BUILDER / 'versions.json')
        inst = Path(a.install_dir)
        if a.list:
            for k, ok in up.status(table, inst).items():
                print(f'{k:6} {"installed" if ok else "not installed"}')
            return 0
        names = a.names or up.default_names(table)
        def show(nm, p):
            print('\n--- ' + up.pins(table)[nm]['vendor'] + ' license for ' + up.pins(table)[nm]['title'] + ' ---')
            enc = sys.stdout.encoding or 'utf-8'
            print(Path(p).read_text(encoding='utf-8', errors='replace')[:1500].encode(enc, 'replace').decode(enc) + '\n[... full text: ' + str(p) + ']\n')
        try:
            ask = fetch.ask_from_args(False)
            if a.yes:
                ask = lambda q: (print(q + ' (answer needed: pass --accept-license <name> to accept)'), False)[1]
            res = up.run(table, names, inst, inst / 'downloads', set(a.accept_license), ask, show_license=show)
            shutil.rmtree(inst / 'downloads', ignore_errors=True)
        except fetch.FetchError as e:
            print('Stopped: ' + str(e))
            return 1
        print('Done: ' + ', '.join(f'{k} {v}' for k, v in res.items()))
        return 0

    if a.cmd in ('fetch', 'pyenv'):
        table = versioncheck.load_table(pipeline.BUILDER / 'versions.json')
        tools_dir = Path(a.tools_dir) if a.tools_dir else fetch.default_tools_dir()
        dl = tools_dir / 'downloads'
        if a.cmd == 'fetch' and a.list:
            for k, pin in fetch.list_pins(table):
                print(f'{k:24} {"pinned" if fetch.is_pinned(pin) else "INCOMPLETE"}  {pin.get("filename", "")}  {pin.get("sha256", "")[:16]}')
            return 0
        try:
            ask = fetch.ask_from_args(a.yes)
            if a.cmd == 'fetch':
                got = fetch.fetch_tools(table, a.tools, tools_dir, dl, ask, a.offline_dir)
                print('Done: ' + ', '.join(f'{k} -> {v}' for k, v in got.items()))
            else:
                if a.base_python:
                    py = pyenv.build_with_venv(table, a.base_python, tools_dir / 'venv', dl, ask, a.offline_dir)
                else:
                    py = pyenv.build_embedded(table, tools_dir / 'python', dl, dl, ask, a.offline_dir)
                print('Done: ' + str(py))
            return 0
        except fetch.FetchError as e:
            print('Stopped: ' + str(e))
            return 1

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
        print(f'Built {exe}, but the smoke run did not reach the title screen: {smoke[1]}\nThe previous output folder (if any) was left unchanged.')
        return 1
    print(f'Done: {exe}' + (f' ({smoke[1]})' if smoke else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
