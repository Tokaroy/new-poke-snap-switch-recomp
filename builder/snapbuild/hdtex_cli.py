# SPDX-License-Identifier: MIT
"""`snap_builder.py hdtex ...`: HD AI textures from the player's own game, made on the player's PC.

  hdtex --capture on|off                 save every texture the game loads while you play (the texture cache)
  hdtex --collect <SnapRecomp.exe>       a hidden, muted pass through the courses your save has unlocked, capturing
  hdtex --level sharp|hd|ultra|custom    upscale the cache into the "hd-ai-textures" mod pack and turn it on
  hdtex --level off                      turn the pack off (nothing is deleted)
  hdtex --cats shadow --pack-id hd-shadows --from-romfs <romfs> --level hd --shadow-source rebaked
                                         HD shadows re-baked on the GPU from the course geometry (Stage B), with the
                                         upscaled maps (Stage A) where a re-bake does not match the game; its own pack
                                         (hd-shadows-rebaked). --shadow-source upscaled (default) is Stage A only.
  hdtex --status                         what is captured, what is built, what is on
  hdtex --licenses [--level L]           the licenses level L needs and which are accepted (--accept-license X records X)
For the launcher, --json makes --status, --dry-run and --licenses print one JSON object on stdout, and a build print one
JSON object per line ({"progress": ...}, then {"done": ...}); all other text goes to stderr.
Exit codes: 0 done, 1 failed or stopped (does not fit, no disk space, empty cache, a texture failed), 2 bad arguments,
3 a license is not accepted yet (see --licenses).
Guide: builder/HD-TEXTURES.md.
"""
import contextlib
import json
import re
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from . import fetch, hdtex as H, hdtex_romfs as R, hdtex_setup as S, hdtex_classify as hc
from . import safedel
from . import hdshadows_rebake as RB

ROUTES = H.BUILDER / 'hdtex_routes'
PACK_META = {'hd-shadows': {'name': 'HD shadows (made on this PC)', 'version': '1', 'author': 'Snap Recomp Builder',
                            'description': 'Sharper baked sun shadows for every course, made from your own game files. Not for sharing.'}}


def add_args(p):
    p.add_argument('--level', choices=['off', 'sharp', 'hd', 'ultra', 'custom'], help='quality level (HD is the suggested one)')
    p.add_argument('--set', action='append', default=[], metavar='CAT.KEY=VALUE',
                   help='custom setting, e.g. creature.scale=2 scenery.model=texture-dat2 creature.strength=0.4 ui.denoise=0.5 '
                        'lighting.scale=0 (repeatable)')
    p.add_argument('--base', choices=['sharp', 'hd', 'ultra'], default='hd', help='level that --level custom starts from')
    p.add_argument('--bc7', choices=['off', 'on', 'all'], default='on',
                   help='on: BC3 color maps become BC7 (same memory, better quality); all: BC1 too (twice the memory); off: keep every format')
    p.add_argument('--exclude', action='append', default=[], help='Pokedex numbers or CH_ ids to leave untouched (comma list, repeatable)')
    p.add_argument('--max-edge', type=int, default=H.MAX_EDGE, help='largest texture side (default 4096)')
    p.add_argument('--dry-run', action='store_true', help='only show the plan: count, time, disk and VRAM')
    p.add_argument('--force', action='store_true', help='build even if the VRAM check says the textures will not fit')
    p.add_argument('--vram-reserve-gb', type=float, default=4.0, help='GPU memory kept for the game itself (default 4)')
    p.add_argument('--capture', choices=['on', 'off'], help='turn texture capture during normal play on or off')
    p.add_argument('--collect', metavar='EXE', help='run the capture pass with this SnapRecomp.exe')
    p.add_argument('--save-dir', help='your save folder for the capture pass (it is copied; the copy is used)')
    p.add_argument('--route-timeout', type=int, default=420, help='seconds per course in the capture pass')
    p.add_argument('--status', action='store_true', help='show the cache, the pack and whether it is on')
    p.add_argument('--cache', help='texture cache (default %%LOCALAPPDATA%%\\SnapRecomp\\texcache)')
    p.add_argument('--work', help='work folder (default %%LOCALAPPDATA%%\\SnapRecomp\\hdtex)')
    p.add_argument('--mods-dir', help='mods folder (default %%APPDATA%%\\SnapRecomp\\mods)')
    p.add_argument('--tools-dir', help='tools folder (default %%LOCALAPPDATA%%\\SnapRecomp\\tools)')
    p.add_argument('--accept-license', action='append', default=[], help='license you accept: directml, texconv, ncnn, model:<id> (repeatable)')
    p.add_argument('--yes', action='store_true', help='download without asking (does NOT accept any license)')
    p.add_argument('--offline-dir', action='append', default=[], help='folder with already downloaded files (hash checked)')
    p.add_argument('--limit', type=int, default=0, help='build at most this many textures (testing)')
    p.add_argument('--only', help='only textures whose path matches this regular expression (testing)')
    p.add_argument('--ab-originals', action='store_true', help='also write the originals for the in-game A/B (SNAP_MODS_AB_ORIG)')
    p.add_argument('--pause-locks', help='developer use: pause while a *.lock in this folder names a timing run')
    p.add_argument('--from-data', metavar='ROOT', help='read the course textures straight from your unpacked game files (no play-through); with --cats')
    p.add_argument('--cats', help='only these texture categories, comma list (shadow = the HD shadows row, lightmap = the Lighting row)')
    p.add_argument('--from-romfs', metavar='ROMFS', help='read the course textures from the game archives of this extracted romfs folder (no unpacked tree needed); with --cats')
    p.add_argument('--pack-id', default=H.PACK_ID, help='mod pack to write (default hd-ai-textures; HD shadows uses hd-shadows)')
    p.add_argument('--pause-game', action='store_true', help='background build: wait while the game is running')
    p.add_argument('--report', help='write the run report (JSON) here as well')
    p.add_argument('--licenses', action='store_true', help='list the licenses the level needs and which are accepted')
    p.add_argument('--json', action='store_true', help='machine-readable output for the launcher (see the module help)')
    p.add_argument('--shadow-source', choices=['upscaled', 'rebaked'], default='upscaled',
                   help='HD shadows (--pack-id hd-shadows): upscaled = the game\'s maps enlarged (Stage A); rebaked = re-baked on the '
                        'GPU from the course geometry, upscaled where a re-bake does not match (Stage B, pack hd-shadows-rebaked)')


def gb(n):
    return f'{n / 2**30:.2f} GB'


def show_plan(items, counts, est, say):
    say(f'{len(items)} textures to upscale (captured: {sum(counts.values())}; ' + ', '.join(f'{k} {v}' for k, v in sorted(counts.items())) + ')')
    for c, v in sorted(est['by_cat'].items()):
        say(f'  {c:9} {v["n"]:5}  {gb(v["bytes"]):>9}  about {v["seconds"] / 60:.0f} min')
    if est.get('left', len(items)) < len(items):
        say(f'All {len(items)} are already built.' if not est['left'] else
            f'{len(items) - est["left"]} of {len(items)} are already built; the time below is for the other {est["left"]}.')
    say(f'Estimate: about {est["seconds"] / 60:.0f} min, {gb(est["disk"])} on disk, '
        f'{gb(est["vram_peak"])} of textures at the busiest course (originals {gb(est["vram_peak_orig"])}; an upper bound)')


def status(cache, work, mods, say, emit=None, pack_id=H.PACK_ID):
    idx = H.read_index(cache)
    on = (Path(cache) / 'capture.on').exists()
    say(f'Texture cache {cache}: {len(idx)} textures, capture during play {"on" if on else "off"}')
    st = H.load_state(work)
    tot = sum(r.get('bytes', 0) for r in st['outputs'].values())
    kept = len(st.get('retired', {}))
    say(f'Pack {Path(mods) / pack_id}: {len(st["outputs"])} textures, {gb(tot)}' + (f' ({kept} kept from another level)' if kept else ''))
    mj = Path(mods) / 'mods.json'
    en = None
    if mj.exists():
        for e in json.loads(mj.read_text(encoding='utf-8')).get('packs', []):
            if e.get('id') == pack_id:
                en = e.get('enabled', True)
    say('In game: ' + ('on' if en else 'off' if en is not None else 'not set up'))
    last = Path(work) / 'last_run.json'
    r = {}
    if last.exists():
        r = json.loads(last.read_text(encoding='utf-8'))
        say(f'Last build: level {r.get("level")}, {r.get("built")} built, {r.get("failed")} failed, {r.get("seconds", 0) / 60:.1f} min')
    if emit:
        emit({'status': {'cache': str(cache), 'captured': len(idx), 'capture_on': on, 'pack': str(Path(mods) / pack_id),
                         'pack_textures': len(st['outputs']), 'pack_bytes': tot, 'kept_from_other_level': kept, 'enabled': en,
                         'ab_originals': str(Path(work) / 'ab-orig') if (Path(work) / 'ab-orig').is_dir() else None,
                         'last_build': {k: r.get(k) for k in ('level', 'built', 'failed', 'seconds', 'date')} if r else None}})
    return 0


def needed_licenses(pins, used, tdir):
    """[{key, title, name, url, author, personal_use_only, accepted}] for a build that uses these models."""
    have = S.accepted_before(tdir)
    out = [('texconv', pins['texconv']['title'], pins['texconv']['license'])]
    if any(pins['models'][m]['engine'] == 'ort' for m in used):
        lic = pins['directml_license']
        out.append(('directml', 'ONNX Runtime with DirectML', {'name': lic['title'], 'url': lic['url'], 'author': 'Microsoft'}))
    if any(pins['models'][m]['engine'] == 'ncnn' for m in used):
        out.append(('ncnn', pins['ncnn']['title'], pins['ncnn']['license']))
    out += [('model:' + m, pins['models'][m]['title'], pins['models'][m]['license']) for m in used]
    return [{'key': k, 'title': ti, 'name': li.get('name', ''), 'url': li.get('url', ''), 'author': li.get('author', ''),
             'personal_use_only': bool(li.get('personal_use_only')), 'accepted': k in have} for k, ti, li in out]


# ---- capture pass --------------------------------------------------------------------------------------------------------

def collect(exe, cache, save_dir, routes, timeout, say, launcher=None, args=()):
    """Runs the game once per route with capture on: hidden, muted, below-normal priority, on a COPY of the save.
    A course the save has not unlocked ends at the time limit and adds nothing. Returns {route: (exit, new files)}."""
    exe = Path(exe)
    if not exe.is_file():
        raise SystemExit(f'{exe} not found')
    files = sorted(Path(routes).glob('*.txt'))
    if not files:
        raise SystemExit(f'no routes in {routes}')
    out = {}
    for r in files:
        before = len(H.read_index(cache))
        tmp = Path(tempfile.mkdtemp(prefix='snap_hdtex_'))
        try:
            save = tmp / 'save'
            if save_dir and Path(save_dir).is_dir():
                shutil.copytree(save_dir, save)
            else:
                save.mkdir()
            (tmp / 'no_mods').mkdir()
            env = dict(os.environ, SNAP_TEXDUMP='1', SNAP_TEXDUMP_DIR=str(cache), SNAP_SAVE_DIR=str(save), SNAP_CONFIG_DIR=str(tmp / 'config'),
                       SNAP_INPUT_SCRIPT=str(r), SNAP_HEADLESS='1', SNAP_INPUT_BACKGROUND='1', SNAP_AUDIO_BACKEND='null',
                       SNAP_WINDOW_PAUSE='0', SNAP_MODS_DIR=str(tmp / 'no_mods'), SNAP_NEO_SPEED='3',
                       SNAP_LAUNCHER_AUTOPLAY='1')   # a release exe started with no arguments opens the launcher otherwise
            shader = Path(os.environ.get('APPDATA', '.')) / 'SnapRecomp' / 'shadercache'
            if shader.is_dir():
                env['SNAP_SHADERCACHE_DIR'] = str(shader)
            cmd = (list(launcher) if launcher else []) + [str(exe)] + list(args)   # args: a dev exe needs 'run'
            say(f'capture pass: {r.stem} ...')
            p = subprocess.Popen(cmd, env=env, cwd=str(exe.parent), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 creationflags=0x4000 | H.NO_WINDOW)   # BELOW_NORMAL_PRIORITY_CLASS
            try:
                code = p.wait(timeout)
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
                code = 'time limit'
            got = len(H.read_index(cache)) - before
            out[r.stem] = (code, got)
            say(f'  {r.stem}: {got} new textures ({code})')
        finally:
            safedel.rmtree(tmp, ignore_errors=True)
    return out


# ---- engines -------------------------------------------------------------------------------------------------------------

def _ask(yes):
    """Download prompt. --yes allows downloads only; hdtex_setup.accept never takes an auto-yes as a license answer."""
    ask = fetch.ask_from_args(yes)
    ask.auto_yes = bool(yes)
    return ask


def make_engine_factory(pins, tdir, texconv, work):
    from . import hdtex_engine as E

    def make(mid):
        m = pins['models'][mid]
        if m['engine'] == 'ncnn':
            root = Path(tdir) / 'hdtex' / 'ncnn'
            return E.NcnnEngine(root / 'realesrgan-ncnn-vulkan.exe', root / 'models', m['ncnn_name'], m['scale'], texconv, Path(work) / 'ncnn')
        return E.OrtEngine(S.model_path(pins, mid, tdir), m['scale'], fixed=m.get('fixed'))
    return make


def probe_speed(make, models, say):
    """One 256x256 tile per model on this PC, compared with the reference PC, to correct the time estimate."""
    import numpy as np
    f = []
    rng = np.random.default_rng(1)
    for mid in models:
        try:
            e = make(mid)
            x = rng.random((256, 256, 3), dtype=np.float32)
            e.upscale(x)                     # warm-up (shader compile)
            t0 = time.perf_counter()
            e.upscale(x)
            dt = time.perf_counter() - t0
            ref = H.SPEED.get(mid, 0) * (256 * e.scale) ** 2 / 1e6   # the same tile on the reference PC
            f.append(dt / ref if ref else 1.0)
            say(f'  {mid}: {dt:.2f} s per 256x256 tile ({", ".join(str(p) for p in getattr(e, "providers", []))})')
        except Exception as ex:   # the run itself reports the real error
            say(f'  {mid}: probe failed: {ex}')
    return max(f) if f else 1.0


# ---- main ----------------------------------------------------------------------------------------------------------------

def main(a, table=None, say=None):
    """--json: human text to stderr, one JSON object per line on stdout. Prints are flushed so a launcher sees progress."""
    if not getattr(a, 'json', False):
        return _main(a, table, say or (lambda s: print(s, flush=True)), None)
    out = sys.stdout
    emit = lambda obj: (out.write(json.dumps(obj, default=str) + '\n'), out.flush())   # noqa: E731
    prog = re.compile(r'^\s*(\d+)/(\d+) done \((\d+) failed\), ([\d.]+) min so far, about (\d+) min left')

    def say_json(s):
        print(s, file=sys.stderr, flush=True)
        m = prog.match(s)
        if m:
            d, n, f, el, left = m.groups()
            emit({'progress': {'done': int(d), 'total': int(n), 'failed': int(f), 'minutes': float(el), 'minutes_left': int(left)}})
    with contextlib.redirect_stdout(sys.stderr):   # stray prints (downloads, prompts) must not reach the JSON stream
        return _main(a, table, say_json, emit)


def work_dir(a):
    """--work, else the default folder; another pack than the default gets its own subfolder, so one pack's build never retires
    another pack's records."""
    if a.work:
        return Path(a.work)
    w = H.default_work()
    return w if a.pack_id == H.PACK_ID else w / 'packs' / a.pack_id


def _main(a, table, say, emit):
    cache = Path(a.cache) if a.cache else H.default_cache()
    work = work_dir(a)
    mods = Path(a.mods_dir) if a.mods_dir else H.default_mods()
    tdir = S.tools_dir(a.tools_dir)
    pins = H.load_pins()
    if a.capture:
        cache.mkdir(parents=True, exist_ok=True)
        flag = cache / 'capture.on'
        if a.capture == 'on':
            flag.write_text('Texture capture for HD AI textures (snap_builder.py hdtex --capture off turns it off).\n', encoding='utf-8')
        else:
            flag.unlink(missing_ok=True)
        say(f'Texture capture during play is {a.capture}. Cache: {cache}')
        return 0
    if a.status:
        return status(cache, work, mods, say, emit, a.pack_id)
    if a.collect:
        collect(a.collect, cache, a.save_dir, ROUTES, a.route_timeout, say)
        if not a.level:
            return 0
    if a.licenses and not a.level:
        a.level = H.DEFAULT_LEVEL
    if not a.level:
        say('Nothing to do: pass --level, --capture, --collect, --status or --licenses (see --help).')
        return 2
    if a.level == 'off':
        changed = H.set_enabled(mods, False, a.pack_id)
        if a.pack_id == RB.STAGE_A_ID:
            changed = H.set_enabled(mods, False, RB.PACK_ID) or changed
        say('HD AI textures are off.' + ('' if changed else ' (They were not set up.)') + ' No file was deleted; --level <level> turns them back on.')
        if emit:
            emit({'done': {'level': 'off', 'enabled': False}})
        return 0
    try:
        settings = H.level_settings(a.level, a.set, a.base)
        exclude = [x.strip() for e in a.exclude for x in e.split(',') if x.strip()]
        for x in exclude:
            H.species_id(x)
    except ValueError as e:
        say(str(e))
        return 2
    only_cats = [c.strip() for c in a.cats.split(',') if c.strip()] if a.cats else None
    ai_cats = tuple(c for c in hc.AI_CATEGORIES + ('lightmap',) if not only_cats or c in only_cats)
    used = sorted({settings[c]['model'] for c in ai_cats if settings[c].get('scale', 0) > 1 and settings[c].get('model')} |
                  ({'dejpg'} if any(settings[c].get('denoise') for c in ai_cats) else set()))
    for mid in used:
        if mid not in pins['models']:
            say(f'unknown model "{mid}". Models: ' + ', '.join(f'{k} ({v["title"]}, {v["license"]["name"]})' for k, v in pins['models'].items()))
            return 2
    if a.licenses:
        lic = needed_licenses(pins, used, tdir)
        for x in lic:
            if x['key'] in a.accept_license and not x['accepted']:
                S.record_accept(tdir, x['key'], x)
                x['accepted'] = True
        for x in lic:
            say(f'{"accepted" if x["accepted"] else "NOT accepted":13} {x["key"]:22} {x["title"]}: {x["name"]} {x["url"]}'
                + (' (personal, non-commercial use only)' if x['personal_use_only'] else ''))
        if emit:
            emit({'licenses': lic, 'level': a.level, 'models': used})
        return 0
    sha = {m: S.model_sha(pins, m) for m in used}
    bc7 = {'off': False, 'on': True, 'all': 'all'}[a.bc7]
    cats = [c.strip() for c in a.cats.split(',') if c.strip()] if a.cats else None
    index = None
    if a.from_romfs and not a.from_data:
        c = cats or ['shadow']
        got = R.extract(a.from_romfs, work, [x for x in c if x in ('shadow', 'lightmap')], say)
        if got is None:
            return 1
        a.from_data = str(got)
    if a.from_data:
        suffixes = tuple(({'shadow': '^Shadow.nutexb', 'lightmap': '^0^Lightmap.nutexb'}[c]) for c in (cats or ['shadow']) if c in ('shadow', 'lightmap'))
        if not suffixes:
            say('--from-data reads the course shadow and light map textures: pass --cats shadow and/or lightmap.')
            return 2
        cache, index = Path(a.from_data), work / 'data-index.tsv'
        work.mkdir(parents=True, exist_ok=True)
        say(f'{H.scan_data(cache, index, suffixes=suffixes)} course textures found in {cache}')
    items, counts = H.plan(cache, settings, sha, bc7=bc7, exclude=exclude, max_edge=a.max_edge, only=a.only, cats=cats, index=index)
    if not counts:
        say(f'The texture cache {cache} is empty. Capture first: --collect <SnapRecomp.exe>, or --capture on and play.')
        if emit and a.dry_run:
            emit({'plan': {'level': a.level, 'textures': 0, 'captured': 0, 'fits': True, 'empty_cache': True}})
        return 1
    est = H.estimate(items)
    left = H.pending(items, work, mods / a.pack_id)   # a rebuild only makes what changed: time only those
    el = H.estimate(left)
    est['seconds'], est['left'] = el['seconds'], len(left)
    for c, v in est['by_cat'].items():
        v['seconds'] = el['by_cat'].get(c, {}).get('seconds', 0.0)
    ok, msg = H.vram_check(est, a.vram_reserve_gb)
    if not os.environ.get('SNAP_HDTEX_CHILD'):   # the HD textures Python run below would print it a second time
        show_plan(items, counts, est, say)
        say('VRAM: ' + msg)
    if emit and a.dry_run:
        emit({'plan': {'level': a.level, 'textures': len(items), 'captured': sum(counts.values()), 'counts': counts,
                       'by_category': est['by_cat'], 'seconds': round(est['seconds']), 'to_build': est['left'], 'disk_bytes': est['disk'],
                       'vram_peak_bytes': est['vram_peak'], 'vram_peak_orig_bytes': est['vram_peak_orig'], 'fits': ok,
                       'vram': msg, 'models': used, 'licenses': needed_licenses(pins, used, tdir)}})
    if not ok and not a.force:
        say('That does not fit. Pick a lower level (or --level custom with smaller scales), or pass --force.')
        return 1
    if a.dry_run:
        return 0
    missing = [x['key'] for x in needed_licenses(pins, used, tdir) if not x['accepted'] and x['key'] not in a.accept_license]
    if missing and (emit or not sys.stdin or not sys.stdin.isatty()):   # the launcher (--json) never answers a prompt
        say('Licenses not accepted yet: ' + ', '.join(missing) + '. See --licenses; accept with --accept-license <key>.')
        if emit:
            emit({'error': {'code': 3, 'licenses_needed': missing}})
        return 3

    # The AI engine needs onnxruntime: build tools/hdtex-python once and run this command again inside it.
    try:
        if used:
            import onnxruntime  # noqa: F401
    except ImportError:
        if os.environ.get('SNAP_HDTEX_CHILD'):
            say('onnxruntime is missing in the HD textures Python')
            return 1
        try:
            py = S.ensure_python(table, pins, tdir, set(a.accept_license), _ask(a.yes), a.offline_dir, say)
        except fetch.FetchError as e:
            say('Stopped: ' + str(e))
            return 1
        env = dict(os.environ, SNAP_HDTEX_CHILD='1')
        return subprocess.call([str(py), '-I', str(H.BUILDER / 'snap_builder.py')] + sys.argv[1:], env=env)

    ask = _ask(a.yes)
    acc = set(a.accept_license)
    try:
        tc = S.ensure_texconv(pins, tdir, acc, ask, a.offline_dir, say)
        for mid in used:
            S.ensure_model(pins, mid, tdir, acc, ask, a.offline_dir, say)
    except fetch.FetchError as e:
        say('Stopped: ' + str(e))
        return 1
    mods.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(mods).free
    if free < est['disk'] * 1.2 + 2**30:
        say(f'Not enough disk space: the pack needs about {gb(est["disk"])}, {gb(free)} is free.')
        return 1
    make = make_engine_factory(pins, tdir, tc, work)
    if used and left:
        say('Measuring this GPU:')
        f = probe_speed(make, used, say)
        est = H.estimate(left, probe=f)
        say(f'Corrected estimate: about {est["seconds"] / 60:.0f} min')
    o = H.RunOptions(cache=cache, work=work, pack=mods / a.pack_id, texconv=tc, make_engine=make,
                     model_scale=lambda m: pins['models'][m]['scale'], lock_dir=a.pause_locks or '', ab_orig=a.ab_originals,
                     limit=a.limit, level=a.level, settings=settings, pause_game=a.pause_game,
                     pack_meta=PACK_META.get(a.pack_id))
    rep = H.run(items, o, say)
    rep.update(level=a.level, settings=settings, estimate=est, date=time.strftime('%Y-%m-%d %H:%M'))
    body = (json.dumps(rep, indent=1, default=str) + '\n').encode()
    H.write_atomic(work / 'last_run.json', body)
    if a.report:
        H.write_atomic(Path(a.report), body)
    if a.pack_id == RB.STAGE_A_ID and a.shadow_source == 'rebaked':
        rb = RB.run(a.from_romfs, work, mods, 4 if a.level == 'ultra' else 2, say, wait_game=H.wait_for_game if a.pause_game else None,
                    data=None if a.from_romfs else a.from_data)
        if rb is None:
            say('The re-baked shadows could not be made; the upscaled HD shadows are kept on.')
            H.set_enabled(mods, True, a.pack_id)
            if emit:
                emit({'error': {'code': 1, 'rebake': False}})
            return 1
        rep['rebake'] = rb
        H.write_atomic(work / 'last_run.json', (json.dumps(rep, indent=1, default=str) + '\n').encode())
        H.set_enabled(mods, False, a.pack_id)
        H.set_enabled(mods, True, RB.PACK_ID)
        o.pack = mods / RB.PACK_ID
        say(f'Re-baked: {rb["rebaked"]} maps, {rb["upscaled"]} upscaled where the re-bake did not match, {rb["original"]} left as the game has them.')
    else:
        H.set_enabled(mods, True, a.pack_id)
        if a.pack_id == RB.STAGE_A_ID:
            H.set_enabled(mods, False, RB.PACK_ID)
    if emit:
        emit({'done': {k: rep.get(k) for k in ('level', 'built', 'failed', 'up_to_date', 'seconds')} | {'enabled': True, 'pack': str(o.pack),
                                                                                                    'shadow_source': a.shadow_source}})
    say(f'Done: {rep["built"]} built, {rep["failed"]} failed, {rep["up_to_date"]} already up to date, {rep["seconds"] / 60:.1f} min. '
        f'{"HD shadows" if a.pack_id == "hd-shadows" else "HD AI textures"} are on ({o.pack}). Turn them off with --level off.')
    return 0 if rep['failed'] == 0 else 1
