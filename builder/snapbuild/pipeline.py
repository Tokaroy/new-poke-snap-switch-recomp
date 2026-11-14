"""The build pipeline: preflight, version check, nso2elf, maps, snap_gen, compile, link, config, smoke run. Each step stamps its
inputs under <work>/stamps so a rerun skips what is finished; everything goes to <work>/builder.log."""
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

from . import ghidra_maps, toolchain
from . import versioncheck as vc
from .util import BELOW_NORMAL, CREATE_NO_WINDOW, Stamps, StepError, files_digest, run, sha256_file, sha256_text

REPO = Path(__file__).resolve().parents[2]   # builder/snapbuild/pipeline.py -> repo root (same layout in the release zip)
BUILDER = REPO / 'builder'
STEPS = ('elf', 'maps', 'gen_main', 'gen_sdk', 'compile_main', 'compile_sdk', 'link')
LIFTER_FILES = ('ehlsda.py', 'elfimg.py', 'flags.py', 'gen.py', 'jumptables.py', 'lifter.py', 'module.py', 'patches.py', 'regs.py',
                'simd_c.py', 'snap_common.py', 'snap_gen.py')


class Options:
    """Plain holder for the command-line values."""

    def __init__(self, **kw):
        self.main = self.sdk = self.romfs = self.work = None
        self.maps = self.llvm = self.python = self.vulkan_lib = self.ghidra = self.jdk = None
        self.prebuilt = None
        self.jobs = 0
        self.skip_smoke = False
        self.harness = None
        self.ghost = None
        self.force = []
        self.compare_generated = None
        self.smoke_timeout = 420
        self.__dict__.update(kw)


def tree_digest(folder, pattern='*.c'):
    """Digest over (name, content hash) of the generated C files (manifest.json holds timings and is left out)."""
    h = hashlib.sha256()
    n = 0
    for p in sorted(Path(folder).glob(pattern)):
        h.update(p.name.encode())
        h.update(b'\0')
        h.update(sha256_file(p).encode())
        n += 1
    return h.hexdigest(), n


def find_prebuilt(arg):
    for c in (arg, REPO / 'prebuilt', REPO / 'build' / 'release' / 'prebuilt'):
        if c and (Path(c) / 'manifest.json').exists():
            return Path(c)
    raise StepError('the prebuilt runtime objects were not found. They ship with the builder (folder "prebuilt"); '
                    'a developer makes them with builder/release_prebuilt.py. Pass the folder with --prebuilt.')


def abi_of(include_dir):
    m = re.search(r'^#define SNAP_RUNTIME_ABI (\d+)', (Path(include_dir) / 'cpu.h').read_text(encoding='utf-8'), re.M)
    if not m:
        raise StepError('the prebuilt include folder has no SNAP_RUNTIME_ABI in cpu.h')
    return int(m.group(1))


def check_prebuilt(pre):
    man = json.loads((pre / 'manifest.json').read_text(encoding='utf-8'))
    abi = abi_of(pre / 'include')
    if man.get('runtime_abi') != abi:
        raise StepError(f'the prebuilt folder is inconsistent: manifest says runtime ABI {man.get("runtime_abi")}, headers say {abi}. '
                        f'Use the prebuilt folder and the builder from the same release.')
    for o in man['objs']:
        if not (pre / 'obj' / o['name']).exists():
            raise StepError(f'prebuilt object {o["name"]} is missing: the prebuilt folder is incomplete.')
    return man


def build(opt, log):
    t_all = time.time()
    work = Path(opt.work).resolve()
    repo = REPO
    stamps = Stamps(work)
    table = vc.load_table(BUILDER / 'versions.json')
    jobs = opt.jobs or toolchain.default_jobs()

    # ---- preflight and tools
    log.write('step preflight')
    probs = toolchain.preflight(work, need_ghidra=not opt.maps, log=log)
    if probs:
        raise StepError('preflight failed:\n  ' + '\n  '.join(probs))
    llvm = toolchain.find_llvm(opt.llvm, repo)
    py = toolchain.find_python(opt.python, repo)
    vcvars = toolchain.find_vcvars()
    vk = toolchain.find_vulkan_lib(opt.vulkan_lib, repo)
    pre = find_prebuilt(opt.prebuilt)
    man = check_prebuilt(pre)
    log.write(f'tools: clang {toolchain.clang_version(llvm)} in {llvm}; python {py}; prebuilt {pre} (runtime ABI {man["runtime_abi"]})')
    want_llvm = table.get('tools', {}).get('llvm', {}).get('version_major')
    if want_llvm and not toolchain.clang_version(llvm).startswith(str(want_llvm) + '.'):
        log.write(f'warning: clang {toolchain.clang_version(llvm)} differs from the pinned major version {want_llvm}; the result may differ')

    # ---- version check (always runs, takes about a second)
    log.write('step version check')
    res = vc.check_all(opt.main, opt.sdk, opt.romfs, table)
    for n in res.notes:
        log.write('note: ' + n)
    if not res.ok:
        raise StepError('version check failed:\n  ' + '\n  '.join(res.problems))
    log.write('version check passed: main, sdk and romfs are game version ' + vc.EXPECTED_NAME)
    main_digest, sdk_digest = table['nso']['main']['image_sha256'], table['nso']['sdk']['image_sha256']
    for s in opt.force:
        stamps.clear(s)

    def step(name, key, outputs, fn):
        if stamps.done(name, key, outputs):
            log.write(f'step {name}: up to date, skipped')
            return False
        log.write(f'step {name}')
        t0 = time.time()
        stamps.clear(name)
        fn()
        stamps.write(name, key, time.time() - t0)
        log.write(f'step {name} done in {time.time() - t0:.0f}s')
        return True

    # ---- nso2elf
    elf_dir = work / 'elf'
    elf_dir.mkdir(exist_ok=True)
    elf_key = sha256_text(main_digest, sdk_digest, files_digest([repo / 'scripts' / 'nso2elf.py']))

    def do_elf():
        for role, src in (('main', opt.main), ('sdk', opt.sdk)):
            run([py, '-I', repo / 'scripts' / 'nso2elf.py', src, elf_dir / f'{role}.elf'], log, what=f'nso2elf {role}')
    step('elf', elf_key, [elf_dir / 'main.elf', elf_dir / 'sdk.elf'], do_elf)

    # ---- maps
    maps_dir = work / 'maps'
    maps_dir.mkdir(exist_ok=True)
    if opt.maps:
        src = Path(opt.maps)
        for n in ghidra_maps.RESULT_FILES:
            if not (src / n).exists():
                raise StepError(f'--maps folder {src} has no {n}.')
        maps_key = sha256_text('maps-dir', *[sha256_file(src / n) for n in ghidra_maps.RESULT_FILES])

        def do_maps():
            for n in ghidra_maps.RESULT_FILES:
                shutil.copyfile(src / n, maps_dir / n)
    else:
        maps_key = sha256_text('ghidra', main_digest, table.get('tools', {}).get('ghidra', {}).get('version', ''),
                               files_digest(sorted((repo / 'scripts' / 'ghidra').glob('*.java')) + [repo / 'scripts' / 'reconcile_functions.py']))

        def do_maps():
            ghidra = toolchain.find_ghidra(opt.ghidra, repo)
            jdk = toolchain.find_jdk(opt.jdk, repo)
            ghidra_maps.run_ghidra(ghidra, jdk, py, repo, work, opt.main, log)
    step('maps', maps_key, [maps_dir / n for n in ghidra_maps.RESULT_FILES], do_maps)
    check_maps(maps_dir, table, log)

    # ---- generate C and compile, per module
    gen_dir = work / 'generated'
    include = pre / 'include'
    abi = man['runtime_abi']
    lifter_key = files_digest([repo / 'recomp' / f for f in LIFTER_FILES])
    maps_files_key = sha256_text(*[sha256_file(maps_dir / n) for n in ghidra_maps.RESULT_FILES])
    gen_env = dict(os.environ, SNAP_ELF_MAIN=str(elf_dir / 'main.elf'), SNAP_ELF_SDK=str(elf_dir / 'sdk.elf'),
                   SNAP_MAPS=str(maps_dir), SNAP_RUNTIME_INCLUDE=str(include))
    build_env = dict(os.environ, SNAP_LLVM_BIN=str(llvm), SNAP_RUNTIME_INCLUDE=str(include))
    compile_keys = {}
    report = {}
    for mod in ('main', 'sdk'):
        gkey = sha256_text(elf_key, maps_files_key, lifter_key, abi, mod)

        def do_gen(mod=mod):
            run([py, '-I', repo / 'recomp' / 'snap_gen.py', '--module', mod, '--out', gen_dir, '--jobs', str(min(12, max(2, jobs + 2)))],
                log, env=gen_env, timeout=40 * 60, what=f'snap_gen {mod}')
        step(f'gen_{mod}', gkey, [gen_dir / mod / f'{mod}_table.c'], do_gen)
        digest, n = tree_digest(gen_dir / mod)
        log.write(f'generated C {mod}: {n} files, tree digest {digest}')
        report[mod] = {'files': n, 'digest': digest}
        cflags = '-march=x86-64-v3'
        ckey = sha256_text(gkey, digest, cflags, toolchain.clang_version(llvm), files_digest(sorted(include.glob('*.h'))))
        compile_keys[mod] = ckey
        lib = work / 'build' / mod / 'lib' / f'snap_{mod}.lib'

        def do_compile(mod=mod):
            run([py, '-I', repo / 'recomp' / 'snap_build.py', '--gen', gen_dir / mod, '--out', work / 'build' / mod, '--jobs', str(jobs),
                 '--pgo', 'off', '--lib', f'snap_{mod}', '--cflags=' + cflags], log, env=build_env, timeout=90 * 60, what=f'compile {mod}')
        step(f'compile_{mod}', ckey, [lib], do_compile)
    (work / 'generated_digest.json').write_text(json.dumps(report, indent=1), encoding='utf-8')
    if opt.compare_generated:
        cmp = Path(opt.compare_generated)
        same = True
        for mod in ('main', 'sdk'):
            d2, n2 = tree_digest(cmp / mod)
            ok = d2 == report[mod]['digest']
            same &= ok
            log.write(f'compare generated {mod}: {"identical" if ok else "DIFFERENT"} ({report[mod]["files"]} vs {n2} files)')
        if not same:
            raise StepError('the generated C differs from the reference folder (see the log).')

    # ---- link
    out = work / 'out'
    out.mkdir(exist_ok=True)
    exe = out / 'SnapRecomp.exe'
    link_key = sha256_text(compile_keys['main'], compile_keys['sdk'], sha256_file(pre / 'manifest.json'), vk.stat().st_size)

    def do_link():
        env = toolchain.vc_env(vcvars)
        objs = [pre / 'obj' / o['name'] for o in man['objs']]
        libs = [work / 'build' / 'main' / 'lib' / 'snap_main.lib', work / 'build' / 'sdk' / 'lib' / 'snap_sdk.lib', vk]
        libs += [f'-l{n}' for n in ('user32', 'gdi32', 'bcrypt', 'ole32', 'avrt', 'shell32', 'advapi32', 'winmm', 'oleaut32', 'dbghelp', 'dwmapi')]
        for lf in man.get('libs', []):
            libs.append(pre / 'lib' / lf)
        res_file = []
        rc = pre / 'res' / 'snap_icon.rc'
        if rc.exists() and (llvm / 'llvm-rc.exe').exists():   # the shipped fallback icon
            (work / 'build').mkdir(exist_ok=True)
            run([llvm / 'llvm-rc.exe', '/FO', work / 'build' / 'snap_icon.res', rc.name], log, cwd=str(rc.parent), what='compile icon resource')
            res_file = [work / 'build' / 'snap_icon.res']
        cmd = [llvm / 'clang++.exe', '-fuse-ld=lld', '-Wl,/manifest:embed', '-Wl,/manifestinput:' + str(pre / 'res' / 'snap_utf8.manifest'),
               '-o', exe] + objs + res_file + libs
        run(cmd, log, env=env, timeout=30 * 60, what='link SnapRecomp.exe')
        for d in man.get('dlls', []):
            if d.get('path', 'bin/').startswith('bin/'):
                shutil.copyfile(pre / 'bin' / d['name'], out / d['name'])
        if man.get('harness') and (pre / man['harness']).exists():   # self-contained shader harness next to the exe
            if (out / man['harness']).exists():
                shutil.rmtree(out / man['harness'])
            shutil.copytree(pre / man['harness'], out / man['harness'])
    step('link', link_key, [exe], do_link)

    # ---- config
    cfg = out / 'config.json'
    # romfs_root is read by the game; main_elf and sdk_elf are the game images the launcher passes as --main and --sdk
    cfg.write_text(json.dumps({'romfs_root': str(Path(res.romfs_root).resolve()), 'main_elf': str(elf_dir / 'main.elf'),
                               'sdk_elf': str(elf_dir / 'sdk.elf')}, indent=1), encoding='utf-8')
    # the launcher (the exe started without arguments) checks the romfs against the published sentinels at every start
    (out / 'launcher_data.json').write_text(json.dumps({'sentinels': table['romfs']['sentinels'], 'file_count': table['romfs']['file_count']}), encoding='utf-8')
    log.write(f'wrote {cfg}')
    log.write(f'build finished in {time.time() - t_all:.0f}s: {exe}')

    # ---- smoke run
    if opt.skip_smoke:
        log.write('smoke run skipped (--skip-smoke)')
        return exe, None
    ok, info = smoke(opt, exe, cfg, work, table, log)
    return exe, (ok, info)


def check_maps(maps_dir, table, log):
    """Row counts against the published check (versions.json "maps"), when the table has one."""
    exp = table.get('maps') or {}
    for name, key in (('functions_final.tsv', 'functions_final_rows'), ('jump_tables.tsv', 'jump_tables_rows')):
        n = sum(1 for line in open(Path(maps_dir) / name, encoding='utf-8') if line.strip() and not line.startswith('#'))
        want = exp.get(key)
        log.write(f'maps: {name} has {n} rows' + (f' (expected {want})' if want else ''))
        if want and n != want:
            raise StepError(f'{name} has {n} rows but {want} are expected for game version {vc.EXPECTED_NAME}: the function discovery '
                            f'result differs (other Ghidra or loader version?). The generated code would differ from the reference.')


# ------------------------------------------------------------------------------------------------ smoke run

def smoke(opt, exe, cfg, work, table, log):
    """Start the exe on a scratch profile, wait for the title screen marker, stop it. Returns (reached, message)."""
    sm = work / 'smoke'
    for sub in ('save', 'mods', 'shadercache', 'localappdata', 'appdata'):
        (sm / sub).mkdir(parents=True, exist_ok=True)
    trace = sm / 'trace.log'
    trace.unlink(missing_ok=True)
    env = dict(os.environ, SNAP_LAUNCHER_AUTOPLAY='1', SNAP_BOOT_TRACE=str(trace), SNAP_SAVE_DIR=str(sm / 'save'), SNAP_MODS_DIR=str(sm / 'mods'),
               SNAP_SHADERCACHE_DIR=str(sm / 'shadercache'), LOCALAPPDATA=str(sm / 'localappdata'), APPDATA=str(sm / 'appdata'),
               SNAP_AUDIO_BACKEND='null', SNAP_HLE_TRACE='open')   # open traces carry the title-screen marker
    for k in ('NVN_CACHE_DIR', 'NVN_SHADER_HARNESS', 'SNAP_HLE_CONFIG'):
        env.pop(k, None)   # the launcher must find everything itself: config.json and shader_harness\ next to the exe
    if opt.harness:
        env['NVN_SHADER_HARNESS'] = str(Path(opt.harness).resolve())
    elif not (exe.parent / 'shader_harness' / 'ShaderHarness.exe').exists():
        log.write('smoke: no shader harness next to the exe and no --harness given; graphics may not start')
    args = [str(exe)]   # launcher mode: no command-line arguments, config.json next to the exe
    if opt.ghost:   # hidden desktop launcher (developer use)
        env.update(SNAP_INPUT_BACKGROUND='1', SNAP_HEADLESS='1', SNAP_WINDOW_PAUSE='0')
        args = [os.environ.get('PYTHON_FOR_GHOST', 'python'), '-I', str(opt.ghost)] + args
    log.write('smoke: starting the game (hidden and muted when --ghost is used)')
    err = open(sm / 'stderr.log', 'wb')
    p = subprocess.Popen(args, env=env, cwd=str(exe.parent), stdout=err, stderr=err, creationflags=BELOW_NORMAL | CREATE_NO_WINDOW)
    want = table.get('smoke', {}).get('title_marker_sha256')
    marker = re.compile(r'fs: open \S*?([^/\\ ]+\.drp)\b')
    t0 = time.time()
    seen_at, presents = None, 0
    reached, why = False, 'the game did not reach the title screen in time'
    try:
        while time.time() - t0 < opt.smoke_timeout:
            time.sleep(1.0)
            if trace.exists():
                text = trace.read_text(encoding='utf-8', errors='replace')
                if seen_at is None and want:
                    for m in marker.finditer(text):
                        if hashlib.sha256(m.group(1).lower().encode()).hexdigest() == want:
                            seen_at = time.time() - t0
                            break
                pm = re.findall(r'nvn: present #(\d+)', text)
                presents = int(pm[-1]) if pm else 0
                if seen_at is not None and presents >= table.get('smoke', {}).get('min_presents', 60):
                    reached, why = True, f'title screen loaded {seen_at:.0f}s after start, {presents} frames presented'
                    break
            if p.poll() is not None:
                why = f'the game exited with code {p.returncode} before the title screen (see {sm / "stderr.log"})'
                break
    finally:
        if p.poll() is None:
            subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
        err.close()
    log.write(('smoke: PASS: ' if reached else 'smoke: FAIL: ') + why)
    return reached, why
