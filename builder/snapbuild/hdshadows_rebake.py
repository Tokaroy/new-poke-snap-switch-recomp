# SPDX-License-Identifier: MIT
"""HD shadows, Stage B: re-bake the sun shadow maps of every course on the GPU from the course geometry.

Runs after the Stage A (upscaled) build of the `hd-shadows` pack, which it uses as the fallback: a map whose re-bake does
not match the game's own closely enough (shrunk back to the original size) gets the Stage A map instead. The result is its
own pack, `hd-shadows-rebaked`; turning it on turns `hd-shadows` off and back (only one of them is on at a time).

Pieces (all in a repository checkout): `tools/hd-shadows` (Rust, Vulkan ray query; built with cargo when its exe is
missing), the measured sun of every course level in `tools/hd-shadows/sun.tsv`, `scripts/hd/hdshadows_orig.py`,
`hdshadows_alpha.py` and `hdshadows_pack.py`. Everything it writes stays in the work folder and the player's mods folder.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = ROOT / 'tools' / 'hd-shadows'
SUN_TABLE = TOOL_DIR / 'sun.tsv'
SCRIPTS = ROOT / 'scripts' / 'hd'
PACK_ID = 'hd-shadows-rebaked'
STAGE_A_ID = 'hd-shadows'
SUB = Path('nx/Pack/LevelComposite/master')
# everything of the course packs but their textures, plus the base colour (cut-outs) and the game's own shadow maps
GEO_RX = r'(?<!\.nutexb)$|_dif\.nutexb$|\^Shadow\.nutexb$'
LOW = getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)


def suns(path=SUN_TABLE):
    """course -> {level key: row} of the measured sun table."""
    out = {}
    if not Path(path).is_file():
        return out
    for line in Path(path).read_text(encoding='utf-8').splitlines()[1:]:
        c = line.split('\t')
        if len(c) >= 8 and ':' in c[0]:
            out.setdefault(c[0].split(':', 1)[0], {})[c[0]] = c
    return out


def tool_exe(work, say):
    """The bake tool: $SNAP_HDSHADOWS_EXE, a built one in the checkout, else built now with cargo (None when it cannot)."""
    env = os.environ.get('SNAP_HDSHADOWS_EXE')
    if env and Path(env).is_file():
        return Path(env)
    for t in (TOOL_DIR / 'target' / 'release' / 'hd-shadows.exe', Path(work) / 'cargo' / 'release' / 'hd-shadows.exe'):
        if t.is_file():
            return t
    cargo = shutil.which('cargo')
    if not cargo or not (TOOL_DIR / 'Cargo.toml').is_file():
        say('The re-bake needs the HD shadows bake tool (tools/hd-shadows) and Rust (cargo) to build it.')
        return None
    say('Building the HD shadows bake tool (once, a minute or two) ...')
    env = dict(os.environ, CARGO_TARGET_DIR=str(Path(work) / 'cargo'))
    r = subprocess.run([cargo, 'build', '--release'], cwd=TOOL_DIR, env=env, capture_output=True, text=True, creationflags=LOW)
    t = Path(work) / 'cargo' / 'release' / 'hd-shadows.exe'
    if r.returncode != 0 or not t.is_file():
        say('Building the bake tool failed: ' + (r.stderr.strip().splitlines() or ['?'])[-1])
        return None
    return t


def extract_geometry(romfs, work, say, unpack=None):
    """Course geometry, levels, materials and base colours into <work>/romfs-src (next to the Stage A shadow maps)."""
    from . import hdtex_romfs as R
    py, script = unpack or R.tools()
    if not py:
        say('Reading the game files needs the Oodle tools (tools/venv-oodle and scripts/drp_unpack.py).')
        return None
    out = Path(work) / 'romfs-src'
    cmd = [py, '-I', str(script), '--set', 'all', '--archive', 'LevelComposite', '--name', GEO_RX, '--romfs', str(romfs),
           '--outdir', str(out), '--manifest', str(Path(work) / 'romfs-geo.tsv'), '--max-minutes', '60']
    say('Reading the course geometry from your game files (a few minutes; a stop continues where it left off) ...')
    r = subprocess.run(cmd, capture_output=True, text=True, creationflags=LOW)
    last = (r.stdout.strip().splitlines() or [''])[-1]
    if r.returncode != 0:
        say(f'Reading the course geometry failed: {last or r.stderr.strip()[-200:]}')
        return None
    say(last)
    return out


def run(romfs, work, mods, scale, say, wait_game=None, data=None, unpack=None):
    """Bakes, scores and packs every course that has a measured sun. Returns a report dict, or None when it cannot run."""
    work = Path(work)
    st = work / 'stageb'
    table = suns()
    if not table:
        say(f'No measured sun table ({SUN_TABLE}).')
        return None
    exe = tool_exe(work, say)
    if not exe:
        return None
    root = Path(data) if data else extract_geometry(romfs, work, say, unpack)
    if not root:
        return None
    master = root / SUB
    stage_a = Path(mods) / STAGE_A_ID / SUB
    pack = Path(mods) / PACK_ID
    courses = sorted(c for c in table if (master / f'{c}.drp').is_dir())
    total = sum(len(list((master / f'{c}.drp').glob('*^Shadow.nutexb'))) for c in courses)
    done = failed = 0
    t0 = time.time()
    rep = {'courses': {}, 'rebaked': 0, 'upscaled': 0, 'original': 0, 'failed_courses': []}
    py = sys.executable
    for c in courses:
        if wait_game:
            wait_game(say)
        src = master / f'{c}.drp'
        n = len(list(src.glob('*^Shadow.nutexb')))
        orig, alpha, out = st / 'orig' / c, st / 'alpha' / c, st / 'bake' / c
        steps = [
            [py, str(SCRIPTS / 'hdshadows_orig.py'), '--pack', str(src), '--out', str(orig)],
            [py, str(SCRIPTS / 'hdshadows_alpha.py'), '--pack', str(src), '--out', str(alpha)],
            [str(exe), 'bake', '--data', str(root), '--course', c, '--orig', str(orig), '--alpha', str(alpha), '--sun-in', str(SUN_TABLE),
             '--out', str(out), '--scale', str(scale), '--samples', '32', '--skip-existing', '--report', str(out / 'report.tsv')],
        ]
        out.mkdir(parents=True, exist_ok=True)
        for cmd in steps:
            r = subprocess.run(cmd, capture_output=True, text=True, creationflags=LOW)
            if r.returncode != 0:
                err = [x for x in r.stderr.strip().splitlines() if 'RUST_BACKTRACE' not in x] or ['?']
                say(f'{c}: {Path(cmd[1]).name if cmd[0] == py else "bake"} failed: {err[-1]}')
                rep['failed_courses'].append(c)
                failed += n
                break
        # a course whose bake failed still gets the Stage A maps (no report rows: every map falls back)
        report = out / 'report.tsv' if c not in rep['failed_courses'] else out / 'no-report.tsv'
        r = subprocess.run([py, str(SCRIPTS / 'hdshadows_pack.py'), '--masks', str(out), '--pack', str(pack), '--course', c, '--source', str(src),
                            '--report', str(report), '--fallback', str(stage_a / f'{c}.drp')],
                           capture_output=True, text=True, creationflags=LOW)
        line = (r.stdout.strip().splitlines() or ['{}'])[-1]
        try:
            counts = json.loads(line)
        except ValueError:
            counts = {}
        rep['courses'][c] = counts
        for k in ('rebaked', 'upscaled', 'original'):
            rep[k] += int(counts.get(k, 0))
        done += n
        el = (time.time() - t0) / 60
        left = el / max(done, 1) * (total - done)
        say(f'  {done}/{total} done ({failed} failed), {el:.1f} min so far, about {left:.0f} min left')
    rep['seconds'] = round(time.time() - t0)
    return rep
