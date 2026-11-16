# SPDX-License-Identifier: MIT
"""Read the course textures straight from the game's own DRP archives (no play-through, no 17 GB unpacked tree).

The HD shadows build needs only the `^Shadow.nutexb` entries of the course packs (about 1.6 GB decoded, a minute or two). This runs the
repository's `scripts/drp_unpack.py` for just those entries into a work folder; it is resumable (every entry is recorded) and
BelowNormal. The unpacker decodes the game's own Oodle streams with the decoder in the user's own `main` (offline tool, see AGENTS.md).
"""
import os
import subprocess
import sys
from pathlib import Path

SUFFIX_RX = {'shadow': r'\^Shadow\.nutexb$', 'lightmap': r'\^0\^Lightmap\.nutexb$'}


def tools():
    """(python of the Oodle environment, unpack script) of a repository checkout, or (None, None)."""
    root = Path(__file__).resolve().parents[2]
    script = root / 'scripts' / 'drp_unpack.py'
    py = os.environ.get('SNAP_OODLE_PYTHON') or str(root / 'tools' / 'venv-oodle' / 'Scripts' / 'python.exe')
    return (py, script) if script.is_file() and Path(py).is_file() else (None, None)


def extract(romfs, work, cats, say, max_minutes=30.0):
    """Unpack the course textures of `cats` into <work>/romfs-src; returns that folder, or None when it cannot."""
    py, script = tools()
    if not py:
        say('Reading the game files needs the Oodle tools (tools/venv-oodle and scripts/drp_unpack.py). '
            'Pass --from-data with an unpacked game folder instead.')
        return None
    out = Path(work) / 'romfs-src'
    rx = '|'.join(SUFFIX_RX[c] for c in cats if c in SUFFIX_RX)
    cmd = [py, '-I', str(script), '--set', 'all', '--archive', 'LevelComposite', '--name', rx, '--romfs', str(romfs),
           '--outdir', str(out), '--manifest', str(Path(work) / 'romfs-src.tsv'), '--max-minutes', str(max_minutes)]
    out.mkdir(parents=True, exist_ok=True)
    say('Reading the course textures from your game files (a minute or two; a stop continues where it left off) ...')
    r = subprocess.run(cmd, capture_output=True, text=True, creationflags=getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0))
    last = (r.stdout.strip().splitlines() or [''])[-1]
    if r.returncode != 0 or 'fail=0' not in last:
        say(f'Reading the game files did not finish cleanly: {last or r.stderr.strip()[-200:]}')
        return None if r.returncode != 0 else out
    say(last)
    return out
