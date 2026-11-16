# SPDX-License-Identifier: MIT
"""HD AI textures: upscales the textures the player's own game served (the texture cache) on the player's PC and writes
them as a normal mod pack. The release carries no texture, no model and no game data; see builder/HD-TEXTURES.md.

Flow: texture cache -> plan (category, size, model, format per texture) -> estimate (time, disk, VRAM) -> batches of
decode (texconv) -> per-category processing (AI or filter, mips) -> encode (texconv) -> NUTEXB -> pack folder.
Every output is recorded in the work folder's state.json with a key over its source, its settings and the code version,
so a re-run only redoes what changed and an interrupted run resumes where it stopped.
"""
import ctypes
import hashlib
import json
from concurrent.futures import ThreadPoolExecutor
import os
import re
import shutil
import subprocess
import time
from dataclasses import dataclass, asdict
from pathlib import Path

import numpy as np

from . import hdtex_classify as hc
from . import hdtex_image as im
from . import hdtex_nutexb as nb

CODE_VERSION = 1
PACK_ID = 'hd-ai-textures'
BUILDER = Path(__file__).resolve().parents[1]
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

# ---- folders ------------------------------------------------------------------------------------------------------------

def local_root():
    return Path(os.environ.get('LOCALAPPDATA', '.')) / 'SnapRecomp'


def default_cache():
    e = os.environ.get('SNAP_TEXDUMP_DIR')
    return Path(e) if e else local_root() / 'texcache'


def default_work():
    return local_root() / 'hdtex'


def default_mods():
    e = os.environ.get('SNAP_MODS_DIR')
    return Path(e) if e else Path(os.environ.get('APPDATA', '.')) / 'SnapRecomp' / 'mods'


def load_pins(path=None):
    return json.loads(Path(path or BUILDER / 'hdtex_pins.json').read_text(encoding='utf-8'))


# ---- levels -------------------------------------------------------------------------------------------------------------
# Per category: scale (0 = untouched), model (AI categories), strength (0 = plain Lanczos .. 1 = the model's output),
# denoise (0 = off .. 1 = full 1xDeJPG pass first). Data maps (normal, mask, emissive) and effects get no AI. Light maps
# (the Lighting row) get the HDR-safe light map path; their directional terms (lightdir) are never changed.

DATA_CATS = ('normal', 'mask', 'emissive', 'effect', 'lightmap', 'lightdir', 'shadow')
LIGHT_MODEL = 'aomr2'   # light map bench 2026-11-15: least ringing and flat-area noise of the pinned models (HD-TEXTURES.md)


def _lv(scenery, creature, ui, data=0, lighting=0, boost=0.0, shadow=0):
    out = {
        'scenery': dict(scale=scenery, model='nomos2-plksr', strength=1.0, denoise=0.0),
        'creature': dict(scale=creature, model='texture-dat2', strength=0.5, denoise=0.0),
        'ui': dict(scale=ui, model='spanimation2', strength=1.0, denoise=0.0),
        'lightmap': dict(scale=lighting, model=LIGHT_MODEL, strength=1.0),
        'lightdir': dict(scale=0),
        'shadow': dict(scale=shadow, strength=1.0),   # HD shadows row: only built with --cats shadow (its own pack)
    }
    for c in ('normal', 'mask', 'emissive', 'effect'):
        out[c] = dict(scale=data)
    out['_boost'] = boost   # Ultra: local contrast of the finest grain on tiling ground and rock (terrain bench 2026-11-15)
    return out


LEVELS = {
    'sharp': _lv(2, 0, 2, shadow=2),
    'hd': _lv(4, 2, 2, lighting=2, shadow=2),
    'ultra': _lv(4, 4, 4, data=2, lighting=4, boost=0.7, shadow=4),
}
DEFAULT_LEVEL = 'hd'
MAX_EDGE = 4096


def level_settings(level, overrides=(), base='hd'):
    """'custom' starts from `base` (default HD); overrides are 'category.key=value' strings."""
    s = json.loads(json.dumps(LEVELS[base if level == 'custom' else level]))
    for o in overrides:
        m = re.match(r'^(\w+)\.(\w+)=(.+)$', o.strip())
        if not m or (m.group(1) not in s and m.group(1) != 'lighting') or m.group(1) == '_boost':
            raise ValueError(f'bad setting "{o}": use category.key=value, category one of {", ".join(k for k in s if k[0] != "_")}')
        cat, key, val = m.groups()
        cat = 'lightmap' if cat == 'lighting' else cat   # the Lighting row: lighting.scale=0 turns it off
        if cat == 'lightdir':
            raise ValueError('lightdir (the directional terms of the light maps) is never changed')
        if key == 'model':
            s[cat][key] = val
        elif key in ('scale', 'strength', 'denoise'):
            s[cat][key] = float(val) if key != 'scale' else int(float(val))
        else:
            raise ValueError(f'unknown key "{key}" (scale, model, strength, denoise)')
    return s


# ---- cache index -----------------------------------------------------------------------------------------------------

def read_index(cache, index=None):
    """{rel: (size, crc)} from the cache's index.tsv (last line wins), only files that still exist with that size.
    index: another index file for the same folder (the game-files scan of --from-data)."""
    out = {}
    p = Path(index) if index else Path(cache) / 'index.tsv'
    if not p.exists():
        return out
    for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
        f = line.split('\t')
        if len(f) >= 3 and f[1].isdigit():
            out[f[0]] = (int(f[1]), f[2])
    return {r: v for r, v in out.items() if (Path(cache) / r).is_file() and (Path(cache) / r).stat().st_size == v[0]}


def scan_data(root, index_path, subdir='nx/Pack/LevelComposite/master', suffixes=('^Shadow.nutexb',)):
    """Index of the course textures straight from the player's unpacked game files (no play-through): every file under
    root/subdir/<pack>/ whose name ends with one of suffixes. Writes index_path (rel, size, change stamp) and returns the
    number of files. The 'crc' column is size and modified time: enough to see that a source changed."""
    base = Path(root) / subdir
    lines = []
    if base.is_dir():
        for pack in sorted(os.listdir(base)):
            d = base / pack
            if not d.is_dir():
                continue
            with os.scandir(d) as it:
                for e in it:
                    if e.name.endswith(suffixes):
                        st = e.stat()
                        lines.append(f'{subdir}/{pack}/{e.name}' + chr(9) + f'{st.st_size}' + chr(9) + f'{st.st_size:x}-{st.st_mtime_ns:x}')
    write_atomic(index_path, (chr(10).join(lines) + chr(10)).encode('utf-8'))
    return len(lines)


# ---- planning -----------------------------------------------------------------------------------------------------------

@dataclass
class Item:
    rel: str
    cat: str
    src_fmt: int
    src_w: int
    src_h: int
    src_mips: int
    src_bytes: int
    out_fmt: int
    out_w: int
    out_h: int
    out_mips: int
    out_bytes: int
    scale: int
    model: str = ''
    strength: float = 1.0
    denoise: float = 0.0
    boost: float = 0.0
    key: str = ''


def out_format(fmt, cat, bc7):
    """Output format: the original's, or BC7 (same sRGB flag) for BC3 colour maps when bc7 is on (same memory, better
    quality); 'all' also turns BC1 into BC7 (twice the memory)."""
    kind, srgb = nb.FORMATS[fmt][3], nb.FORMATS[fmt][4]
    if bc7 and cat in hc.AI_CATEGORIES + ('effect', 'emissive', 'mask'):
        if kind in ('bc3', 'rgba8') or (kind == 'bc1' and bc7 == 'all'):
            return nb.BY_KIND[('bc7', srgb)]
    return fmt


def nutexb_bytes(fmt, w, h, mips):
    bpb, bw, bh = nb.FORMATS[fmt][:3]
    return nb.layout(w, h, bpb, bw, bh, mips)[1] + nb.FOOTER


SHADOW_THREADS = 2
SHADOW_MAX_EDGE = 2048   # sun-shadow maps stop at 2048: a 4096 map is 16 MB, and about 80 of the 4,562 originals are already 2048


def target_size(w, h, scale, max_edge):
    s = min(float(scale), max_edge / max(w, h))
    if s <= 1.0:
        return w, h
    ow, oh = int(round(w * s)), int(round(h * s))
    return max(4, ow // 4 * 4), max(4, oh // 4 * 4)


def species_id(token):
    t = str(token).strip().upper()
    if re.fullmatch(r'\d{1,4}', t):
        return f'CH_{int(t):03d}'
    if re.fullmatch(r'CH_\d{3}', t):
        return t
    raise ValueError(f'"{token}": give a National Pokedex number (25) or a creature id (CH_025)')


def plan(cache, settings, models_sha, bc7=False, exclude=(), max_edge=MAX_EDGE, only=None, cats=None, index=None):
    """-> (items to build, counts by category incl. skipped). models_sha: {model id: sha256} for the state key.
    cats: only these categories (default: everything except 'shadow', which has its own pack and row)."""
    idx = read_index(cache, index)
    excl = {species_id(x) for x in exclude}
    items, counts = [], {}
    for rel in sorted(idx):
        if only and not re.search(only, rel, re.I):
            continue
        try:
            with open(Path(cache) / rel, 'rb') as f:
                f.seek(-nb.FOOTER, 2)
                foot = f.read()
            t = _footer_only(foot, idx[rel][0])
        except (OSError, nb.NutexbError):
            counts['unreadable'] = counts.get('unreadable', 0) + 1
            continue
        cat = hc.classify(rel, t)
        counts[cat] = counts.get(cat, 0) + 1
        if cat in ('skip', 'lightdir') or hc.species(rel) in excl or (cat not in cats if cats else cat == 'shadow'):
            continue
        cs = settings[cat]
        scale = int(cs.get('scale', 0))
        if scale <= 1:
            continue
        ow, oh = target_size(t.w, t.h, scale, min(max_edge, SHADOW_MAX_EDGE) if cat == 'shadow' else max_edge)
        if (ow, oh) == (t.w, t.h):
            continue
        ofmt = out_format(t.fmt, cat, bc7)
        mips = 1 if t.mips == 1 else im.mip_count(ow, oh)
        it = Item(rel, cat, t.fmt, t.w, t.h, t.mips, idx[rel][0], ofmt, ow, oh, mips, nutexb_bytes(ofmt, ow, oh, mips), scale,
                  cs.get('model', ''), float(cs.get('strength', 1.0)), float(cs.get('denoise', 0.0)),
                  float(settings.get('_boost', 0.0)) if cat == 'scenery' else 0.0)
        key_src = json.dumps([CODE_VERSION, idx[rel][1], idx[rel][0], asdict(it), models_sha.get(it.model, ''),
                              models_sha.get('dejpg', '') if it.denoise else ''], sort_keys=True)
        it.key = hashlib.sha256(key_src.encode()).hexdigest()[:24]
        items.append(it)
    return items, counts


def _footer_only(foot, size):
    """A Tex with the footer fields only (planning reads 0xb0 bytes per file, not the whole cache)."""
    if len(foot) != nb.FOOTER:
        raise nb.NutexbError('short file')
    import struct
    w, h, dep, fmt, dim, mips, tile, layers, dsz, magic, ver = struct.unpack_from('<11I', foot, nb.FOOTER - 44)
    if magic != nb.MAGIC:
        raise nb.NutexbError('not a nutexb')
    return nb.Tex(b'', foot, w, h, dep, fmt, dim, mips, tile, layers, dsz)


# ---- estimates ------------------------------------------------------------------------------------------------------------

# Seconds per million OUTPUT pixels on the reference PC (measured 2026-11-15, see builder/HD-TEXTURES.md); scaled on the
# user's PC by a one-tile probe before the run. Filters (non-AI) cost about the same everywhere.
SPEED = {'nomos2-plksr': 0.16, 'pbrify-plksr': 0.21, 'pbrify-dat': 3.45, 'nomos2': 0.24, 'texture-dat2': 1.75, 'gtav-rgts': 1.82, 'aomr2': 0.25, 'dejpg': 1.4,
         'spanimation2': 0.12, 'ultrasharp2': 1.4, 'x4plus': 0.5, 'x4plus-anime': 0.4, '': 0.0}   # dejpg per INPUT Mpx; 2x models per pass; x4plus/anime include ncnn start-up
FILTER_S_PER_MPX = 0.35   # resize, mips, encode
LIGHT_S_PER_MPX = 2.0     # light maps: fill, gate, energy match and the BC6H encode (611 maps, 120 Mpx: 280 s, 2026-11-15)


def course_of(rel):
    m = re.match(r'nx/pack/levelcomposite/master/(lc_\d{3})', rel, re.I)
    return m.group(1) if m else ''


def estimate(items, speed=SPEED, probe=1.0):
    """{'seconds', 'disk', 'vram_peak', 'vram_peak_orig', 'by_cat'}. VRAM: textures outside the courses (menus, creatures,
    shared sets) plus the largest single course, since one course is loaded at a time."""
    secs = 0.0
    disk = 0
    by_cat = {}
    vr, vo = {}, {}
    for it in items:
        mpx = it.out_w * it.out_h / 1e6
        sec = (LIGHT_S_PER_MPX if it.cat == 'lightmap' else FILTER_S_PER_MPX) * mpx
        if it.model:
            sec += speed.get(it.model, 1.0) * mpx * probe
            if it.denoise:
                sec += speed['dejpg'] * (it.src_w * it.src_h / 1e6) * probe
        secs += sec
        disk += it.out_bytes
        c = by_cat.setdefault(it.cat, {'n': 0, 'bytes': 0, 'seconds': 0.0})
        c['n'] += 1
        c['bytes'] += it.out_bytes
        c['seconds'] += sec
        k = course_of(it.rel)
        vr[k] = vr.get(k, 0) + it.out_bytes
        vo[k] = vo.get(k, 0) + it.src_bytes
    def peak(d):
        return d.get('', 0) + max([v for k, v in d.items() if k] or [0])
    return {'seconds': secs, 'disk': disk, 'vram_peak': peak(vr), 'vram_peak_orig': peak(vo), 'by_cat': by_cat}


def gpu_memory():
    """[(name, dedicated bytes)] of the DXGI adapters (software adapters left out); [] when DXGI is not there."""
    try:
        dxgi = ctypes.WinDLL('dxgi')
    except OSError:
        return []

    class GUID(ctypes.Structure):
        _fields_ = [('a', ctypes.c_uint32), ('b', ctypes.c_uint16), ('c', ctypes.c_uint16), ('d', ctypes.c_ubyte * 8)]

    class DESC1(ctypes.Structure):
        _fields_ = [('Description', ctypes.c_wchar * 128), ('VendorId', ctypes.c_uint), ('DeviceId', ctypes.c_uint),
                    ('SubSysId', ctypes.c_uint), ('Revision', ctypes.c_uint), ('DedicatedVideoMemory', ctypes.c_size_t),
                    ('DedicatedSystemMemory', ctypes.c_size_t), ('SharedSystemMemory', ctypes.c_size_t),
                    ('Luid', ctypes.c_uint64), ('Flags', ctypes.c_uint)]
    iid = GUID(0x770aae78, 0xf26f, 0x4dba, (ctypes.c_ubyte * 8)(0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87))   # IDXGIFactory1
    fac = ctypes.c_void_p()
    if dxgi.CreateDXGIFactory1(ctypes.byref(iid), ctypes.byref(fac)) != 0:
        return []
    def vcall(obj, idx, *args, argtypes=()):
        vt = ctypes.cast(ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        fn = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, *argtypes)(vt[idx])
        return fn(obj, *args)
    out = []
    i = 0
    while True:
        ad = ctypes.c_void_p()
        if vcall(fac, 12, i, ctypes.byref(ad), argtypes=(ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p))) != 0:   # EnumAdapters1
            break
        d = DESC1()
        if vcall(ad, 10, ctypes.byref(d), argtypes=(ctypes.POINTER(DESC1),)) == 0 and not (d.Flags & 2):   # GetDesc1, not software
            out.append((d.Description, int(d.DedicatedVideoMemory)))
        vcall(ad, 2)   # Release
        i += 1
    vcall(fac, 2)
    return out


def vram_check(est, reserve_gb=4.0, adapters=None):
    """(ok, message). The game itself needs about reserve_gb at 4K besides its textures; the texture peak must fit in
    the rest of the largest GPU's dedicated memory."""
    adapters = gpu_memory() if adapters is None else adapters
    if not adapters:
        return True, 'GPU memory unknown (no DXGI adapter found); the VRAM check was skipped'
    name, mem = max(adapters, key=lambda a: a[1])
    budget = mem - reserve_gb * 2**30
    need = est['vram_peak']
    msg = f'{name}: {mem / 2**30:.1f} GB; textures at the busiest course {need / 2**30:.2f} GB (originals {est["vram_peak_orig"] / 2**30:.2f} GB), budget {max(budget, 0) / 2**30:.1f} GB'
    return need <= budget, msg


# ---- texconv ------------------------------------------------------------------------------------------------------------

DXGI_NAMES = {28: 'R8G8B8A8_UNORM', 29: 'R8G8B8A8_UNORM_SRGB', 10: 'R16G16B16A16_FLOAT', 71: 'BC1_UNORM', 72: 'BC1_UNORM_SRGB',
              74: 'BC2_UNORM', 75: 'BC2_UNORM_SRGB', 77: 'BC3_UNORM', 78: 'BC3_UNORM_SRGB', 80: 'BC4_UNORM', 83: 'BC5_UNORM',
              95: 'BC6H_UF16', 96: 'BC6H_SF16', 98: 'BC7_UNORM', 99: 'BC7_UNORM_SRGB', 61: 'R8_UNORM', 2: 'R32G32B32A32_FLOAT'}


class TexconvError(RuntimeError):
    pass


def texconv(exe, files, out_dir, dxgi, extra=(), work=None):
    """Converts many DDS files in one texconv run (keeps the given mip chain: -m is not passed unless in extra)."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    lst = Path(work or out_dir) / f'texconv_{dxgi}.txt'
    lst.write_text('\n'.join(str(Path(f).resolve()) for f in files) + '\n', encoding='utf-8')
    cmd = [str(exe), '-nologo', '-y', '-dx10', '-f', DXGI_NAMES[dxgi], '-o', str(out_dir.resolve()), *extra, '-flist', str(lst)]
    r = subprocess.run(cmd, capture_output=True, text=True, creationflags=NO_WINDOW)
    missing = [f for f in files if not (out_dir / Path(f).name).exists()]
    if r.returncode != 0 or missing:
        raise TexconvError(f'texconv {DXGI_NAMES[dxgi]}: {len(missing)} of {len(files)} not written: {(r.stdout + r.stderr)[-600:]}')


def decode_dxgi(fmt):
    kind, srgb = nb.FORMATS[fmt][3], nb.FORMATS[fmt][4]
    if kind in ('bc6u', 'bc6s', 'rgba16f', 'rgba32f'):
        return nb.DXGI_RGBA16F
    return nb.DXGI_RGBA8_SRGB if srgb else nb.DXGI_RGBA8


# ---- processing ---------------------------------------------------------------------------------------------------------

PAD = 8   # pixels of context around an image sent to a model (wrap for tiling textures, mirror otherwise)


def alpha_tested(a):
    """Cut-out alpha (foliage, fences): mostly fully clear or fully solid texels, some of each."""
    lo, hi = (a < 0.1).mean(), (a > 0.9).mean()
    return lo > 0.02 and hi > 0.02 and (lo + hi) > 0.9


class Upscaler:
    """Runs model passes for one batch; engines are made on first use and kept."""

    def __init__(self, make_engine):
        self.make = make_engine
        self.engines = {}
        self.ai_seconds = 0.0

    def engine(self, model):
        if model not in self.engines:
            self.engines[model] = self.make(model)
        return self.engines[model]

    def run(self, model, items):
        """items: [(key, rgb float32 sRGB-encoded)] -> {key: rgb}. One call per model pass (ncnn starts once per call)."""
        if not items:
            return {}
        e = self.engine(model)
        t0 = time.perf_counter()
        out = e.upscale_many(items)
        self.ai_seconds += time.perf_counter() - t0
        return out


def ai_upscale(up, model, scale_of, reqs, total):
    """reqs: {key: padded rgb}; repeats the model's pass until `total` (2 or 4) is reached; returns {key: (rgb, done scale)}."""
    cur = dict(reqs)
    done = 1
    while done < total:
        cur = up.run(model, list(cur.items()))
        done *= scale_of(model)
    return cur, done


def process_color(it, px, ai_rgb, ai_scale, wrap):
    """AI colour categories: blend the model's output (sRGB-encoded) with a Lanczos base in linear light, alpha by filter."""
    f = px.astype(np.float32) / 255
    srgb = nb.FORMATS[it.src_fmt][4]
    tolin = im.srgb_to_lin if srgb else (lambda x: x)
    base = im.resize(tolin(f[..., :3]), it.out_w, it.out_h, wrap=wrap)
    out = base
    if ai_rgb is not None:
        h, w = f.shape[:2]
        crop = ai_rgb[PAD * ai_scale:(PAD + h) * ai_scale, PAD * ai_scale:(PAD + w) * ai_scale]
        ai = tolin(crop)
        if ai.shape[:2] != (it.out_h, it.out_w):
            ai = im.resize(ai, it.out_w, it.out_h, wrap=wrap)
        out = im.blend_detail(base, ai, it.strength)
    if it.boost and wrap:
        out = im.local_contrast(out, it.boost, wrap)
    a = np.clip(im.resize(f[..., 3], it.out_w, it.out_h, wrap=wrap), 0, 1)
    img = np.concatenate([np.clip(out, 0, 1), a[..., None]], -1)
    cutoff = 0.5 if nb.FORMATS[it.src_fmt][3] in ('bc1', 'bc3', 'bc7', 'rgba8') and alpha_tested(f[..., 3]) else None
    levels = im.mip_chain(img, it.out_mips, 'color', cutoff)
    return [to_store(l, srgb) for l in levels]


def to_store(l, srgb):
    if srgb:
        l = np.concatenate([im.lin_to_srgb(l[..., :3]), l[..., 3:]], -1)
    return (np.clip(l, 0, 1) * 255 + 0.5).astype(np.uint8)


def process_normal(it, px, wrap):
    f = px.astype(np.float32) / 255
    two = nb.FORMATS[it.src_fmt][3] == 'bc5'
    n = im.normal_from_rg(f[..., :2]) if two else f[..., :3] * 2 - 1
    n = im.renormalize(im.resize(n, it.out_w, it.out_h, wrap=wrap))
    a = np.clip(im.resize(f[..., 3], it.out_w, it.out_h, wrap=wrap), 0, 1)
    out = []
    for l in im.mip_chain(np.concatenate([n, a[..., None]], -1), it.out_mips, 'normal'):
        if two:
            rgb = np.concatenate([im.rg_from_normal(l), np.zeros_like(l[..., :1])], -1)
            alpha = np.ones_like(l[..., :1])
        else:
            rgb, alpha = (l[..., :3] + 1) * 0.5, l[..., 3:]
        out.append((np.clip(np.concatenate([rgb, alpha], -1), 0, 1) * 255 + 0.5).astype(np.uint8))
    return out


def process_filter(it, px, wrap):
    """mask, emissive, effect: Lanczos on every channel (linear light for sRGB formats), no AI."""
    f = px.astype(np.float32) / 255
    srgb = nb.FORMATS[it.src_fmt][4]
    if srgb:
        f = np.concatenate([im.srgb_to_lin(f[..., :3]), f[..., 3:]], -1)
    g = np.clip(im.resize(f, it.out_w, it.out_h, wrap=wrap), 0, 1)
    return [to_store(l, srgb) for l in im.mip_chain(g, it.out_mips, 'color')]


def _box(x, fn, r=1):
    """Min or max filter over a (2r+1)^2 window of a 2-D array, edges repeated."""
    h, w = x.shape
    p = np.pad(x, r, mode='edge')
    out = None
    for dy in range(2 * r + 1):
        for dx in range(2 * r + 1):
            v = p[dy:dy + h, dx:dx + w]
            out = v if out is None else fn(out, v)
    return out


def process_shadow(it, px, wrap):
    """HD shadows, no AI: the game's per-mesh shadow maps hold the shadow in alpha (black colour). Lanczos enlargement, kept
    inside the original's local min/max (no ringing, the amount of shadow stays the game's), then the hard edges, and only
    those, are steepened by the scale so the ramp stays as narrow in texels as it was: crisp where the bake was crisp,
    plain enlargement where it was a soft half-tone. strength 0 is plain Lanczos."""
    a = px[..., 3].astype(np.float32) / 255
    h, w = a.shape
    base = np.clip(im.resize(a, it.out_w, it.out_h), 0, 1)
    yi = np.clip(((np.arange(it.out_h) + 0.5) * h / it.out_h).astype(int), 0, h - 1)
    xi = np.clip(((np.arange(it.out_w) + 0.5) * w / it.out_w).astype(int), 0, w - 1)
    lo = _box(a, np.minimum)[yi][:, xi]
    hi = _box(a, np.maximum)[yi][:, xi]
    b = np.clip(base, lo, hi)
    rng = hi - lo
    edge = np.clip((rng - 0.35) / 0.3, 0, 1) * float(max(min(it.strength, 1.0), 0.0))
    gain = 1.0 + (float(it.scale) - 1.0) * edge
    t = np.clip((b - lo) / np.maximum(rng, 1e-3), 0, 1)
    out = np.clip(lo + np.clip((t - 0.5) * gain + 0.5, 0, 1) * rng, 0, 1)
    if it.out_mips == 1:   # the usual case: black colour, so no sRGB step and no float RGBA
        q = np.zeros((it.out_h, it.out_w, 4), np.uint8)
        q[..., 3] = (out * 255 + 0.5).astype(np.uint8)
        return [q]
    img = np.concatenate([np.zeros((it.out_h, it.out_w, 3), np.float32), out[..., None]], -1)
    return [to_store(l, nb.FORMATS[it.src_fmt][4]) for l in im.mip_chain(img, it.out_mips, 'color')]


# Light maps (the Lighting row). Studied against MatyMeatBoy/NewSnapRecomp newsnap/hdshadows.py (MIT, commit 10e5a20); no code
# taken. Differences: float log input (no 8-bit step), directional terms found by name (^1^..^3^) not by sign, filled
# gutters kept (no dark seams), the model gated to shadow edges, and texconv's full BC6H encoder.
LIGHT_FLOOR = 1.0 / 1024   # light below this counts as black in the log encoding


def light_fill(rgb, mask):
    """Push-pull: texels outside the UV islands take the average of the nearest island texels, so resizing and the
    game's own filtering never pull black into an island's border (dark seams)."""
    if mask.all() or not mask.any():
        return rgb
    levels = [(rgb * mask[..., None], mask.astype(np.float32))]
    while min(levels[-1][1].shape) > 1:
        c, m = levels[-1]
        hh, ww = (c.shape[0] + 1) // 2 * 2, (c.shape[1] + 1) // 2 * 2
        c = np.pad(c, ((0, hh - c.shape[0]), (0, ww - c.shape[1]), (0, 0)))
        m = np.pad(m, ((0, hh - m.shape[0]), (0, ww - m.shape[1])))
        levels.append((c.reshape(hh // 2, 2, ww // 2, 2, -1).sum((1, 3)), m.reshape(hh // 2, 2, ww // 2, 2).sum((1, 3))))
    col = levels[-1][0] / np.maximum(levels[-1][1], 1e-6)[..., None]
    for c, m in reversed(levels[:-1]):
        up = np.repeat(np.repeat(col, 2, 0), 2, 1)[:c.shape[0], :c.shape[1]]
        col = np.where((m > 0)[..., None], c / np.maximum(m, 1e-6)[..., None], up)
    return np.where(mask[..., None], rgb, col).astype(np.float32)


def light_prepare(px):
    """-> (model input in [0, 1]: log2 light spread over this map's own range, (low, range)). Float all the way: no 8-bit
    step, so gradients do not band."""
    rgb = np.maximum(px[..., :3].astype(np.float32), 0)
    f = light_fill(rgb, rgb.max(2) > 1e-6)
    lg = np.log2(f + LIGHT_FLOOR)
    lo = float(lg.min())
    top = max(float(lg.max()) - lo, 1e-6)
    return (lg - lo) / top, (lo, top)


def _blur(x, s):
    return im.gauss(x, s, False)


def light_gate(base, s):
    """Where the model's output is used: along coherent shadow edges (gradient of the blurred light), not in flat light and
    not where the map itself is noisy (that is where models invent cell patterns)."""
    lum = base.mean(2)
    g = np.hypot(*np.gradient(_blur(lum, 1.0 * s)))
    noise = _blur(np.abs(lum - _blur(lum, 0.5 * s)), 1.5 * s)
    w = np.clip(g / (np.percentile(g, 90) + 1e-6), 0, 1) * np.clip(1 - 0.25 * noise / (g + 1e-6), 0, 1)
    return _blur(w, 0.5 * s)[..., None]


def process_lightmap(it, px, wrap, ai=None, rng=None, ai_scale=1):
    """Light maps, HDR-safe: log2 light, the model gated to shadow edges, back to linear, then the low frequencies matched
    to a plain log-Lanczos enlargement so the amount, colour and spread of the light stay the game's. Empty texels keep
    the filled (nearest island) light instead of black. ai: the model's output on light_prepare's input (padded)."""
    x, (lo, top) = rng if rng else light_prepare(px)
    s = max(it.out_w / it.src_w, 1.0)
    base = im.resize(x, it.out_w, it.out_h, wrap=wrap)
    y = base
    if ai is not None:
        hh, ww = x.shape[:2]
        crop = ai[PAD * ai_scale:(PAD + hh) * ai_scale, PAD * ai_scale:(PAD + ww) * ai_scale].astype(np.float32)
        if crop.shape[:2] != (it.out_h, it.out_w):
            crop = im.resize(crop, it.out_w, it.out_h, wrap=wrap)
        y = base + light_gate(base, s) * (crop - base) * float(max(min(it.strength, 1.0), 0.0))
    plain = np.exp2(base * top + lo) - LIGHT_FLOOR
    out = np.exp2(y * top + lo) - LIGHT_FLOOR
    ratio = (_blur(plain, 1.5 * s) + 1e-4) / (_blur(out, 1.5 * s) + 1e-4)
    out = np.maximum(out * np.clip(ratio, 0.6, 1.6), 0)
    a = im.resize(px[..., 3].astype(np.float32), it.out_w, it.out_h, wrap=wrap)
    lv = im.mip_chain(np.concatenate([out, a[..., None]], -1), it.out_mips, 'color')
    return [np.nan_to_num(l, nan=0.0, posinf=60000.0).astype(np.float16) for l in lv]


# ---- state and pack -------------------------------------------------------------------------------------------------------

def pending(items, work, pack):
    """The items a build would still make: not in the work state with the same key, or missing from the pack."""
    out = load_state(work)['outputs']
    return [it for it in items if not ((r := out.get(it.rel)) and r['key'] == it.key and (Path(pack) / it.rel).is_file())]


def load_state(work):
    p = Path(work) / 'state.json'
    if p.exists():
        try:
            return json.loads(p.read_text(encoding='utf-8'))
        except ValueError:
            pass
    return {'outputs': {}}


def save_state(work, st):
    p = Path(work) / 'state.json'
    tmp = p.with_suffix('.tmp')
    tmp.write_text(json.dumps(st, indent=0, sort_keys=True), encoding='utf-8')
    os.replace(tmp, p)


def write_atomic(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.part')
    tmp.write_bytes(data)
    os.replace(tmp, path)


def pack_json(level, settings):
    return {'name': 'HD AI textures (made on this PC)', 'version': f'{level}-{CODE_VERSION}', 'author': 'Snap Recomp Builder',
            'description': f'Your own textures, upscaled on this PC at level "{level}". Not for sharing: it is made from your game files.'}


def set_enabled(mods_dir, enabled, pack_id=PACK_ID):
    """Turns the pack on or off in mods.json. Off only flips the flag; no file is deleted. On adds the pack last, so a
    hand-made pack listed before it still wins."""
    mods_dir = Path(mods_dir)
    p = mods_dir / 'mods.json'
    data = {'packs': []}
    if p.exists():
        data = json.loads(p.read_text(encoding='utf-8') or '{}')
        data.setdefault('packs', [])
    for e in data['packs']:
        if e.get('id', e.get('folder')) == pack_id:
            e['enabled'] = bool(enabled)
            break
    else:
        if not enabled:
            return False
        data['packs'].append({'id': pack_id, 'enabled': True})
    mods_dir.mkdir(parents=True, exist_ok=True)
    write_atomic(p, (json.dumps(data, indent=1) + '\n').encode('utf-8'))
    return True


def retire_stale(pack, work, st, keep, say):
    """Outputs of an earlier run that this plan no longer wants (lower level, excluded species) move to work/retired, keyed
    by their state key, so switching back is a file move. Only files this builder wrote and recorded are touched."""
    moved = 0
    for rel, rec in list(st['outputs'].items()):
        if rel in keep:
            continue
        src = Path(pack) / rel
        if src.exists():
            dst = Path(work) / 'retired' / rec['key'] / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            os.replace(src, dst)
            moved += 1
        st.setdefault('retired', {})[rel] = rec
        del st['outputs'][rel]
    if moved:
        say(f'{moved} textures not in this level moved to {Path(work) / "retired"} (kept for a later switch back)')
    return moved


def restore_retired(pack, work, st, it):
    rec = st.get('retired', {}).get(it.rel)
    if not rec or rec['key'] != it.key:
        return False
    src = Path(work) / 'retired' / rec['key'] / it.rel
    if not src.exists():
        return False
    dst = Path(pack) / it.rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    os.replace(src, dst)
    st['outputs'][it.rel] = rec
    del st['retired'][it.rel]
    return True


# ---- pausing for other GPU work (developer PCs) -----------------------------------------------------------------------------

def busy_locks(lock_dir, pattern=r'cp_|perf|(exclusive)'):
    out = []
    for f in Path(lock_dir).glob('*.lock'):
        try:
            t = f.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        if re.search(pattern, t, re.I):
            out.append(f'{f.name}: {t.strip()[:80]}')
    return out


def wait_for_locks(lock_dir, say, poll=20.0, sleep=time.sleep):
    if not lock_dir:
        return 0.0
    t0 = time.perf_counter()
    told = False
    while True:
        b = busy_locks(lock_dir)
        if not b:
            return time.perf_counter() - t0
        if not told:
            say('paused while a timing run holds a slot: ' + b[0])
            told = True
        sleep(poll)


GAME_EXES = ('snaprecomp.exe', 'snap_boot.exe')


def game_running():
    """True while a game process (the release SnapRecomp.exe or a development snap_boot.exe) is running."""
    try:
        out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True, timeout=30,
                             creationflags=NO_WINDOW).stdout.lower()
    except (OSError, subprocess.SubprocessError):
        return False
    return any(f'"{n}"' in out for n in GAME_EXES)


def wait_for_game(say, poll=15.0, sleep=time.sleep):
    """Blocks while the game runs, so the background build never competes with it for the GPU. Returns the seconds waited."""
    t0 = time.perf_counter()
    told = False
    while game_running():
        if not told:
            say('paused while the game is running; the build goes on when you quit it')
            told = True
        sleep(poll)
    return time.perf_counter() - t0


def below_normal():
    try:
        k = ctypes.windll.kernel32
        k.SetPriorityClass(k.GetCurrentProcess(), 0x4000)
    except (AttributeError, OSError):
        pass


# ---- the run --------------------------------------------------------------------------------------------------------------

@dataclass
class RunOptions:
    cache: Path
    work: Path
    pack: Path
    texconv: Path
    make_engine: object          # model id -> engine (hdtex_engine.OrtEngine / NcnnEngine)
    model_scale: object          # model id -> int
    batch: int = 32
    batch_mpx: float = 48.0      # output megapixels per batch (bounds RAM: the AI results of a batch are held together)
    lock_dir: str = ''
    ab_orig: bool = False        # also write originals (in the output format) for SNAP_MODS_AB_ORIG
    limit: int = 0
    level: str = DEFAULT_LEVEL
    settings: dict = None
    pause_game: bool = False     # background build: wait while the game runs
    pack_meta: dict = None       # pack.json of a pack that is not hd-ai-textures


def batches(todo, n, mpx):
    """(start index, items) chunks of at most n items and about mpx output megapixels (always at least one item)."""
    i = 0
    while i < len(todo):
        j, tot = i, 0.0
        while j < len(todo) and j - i < n and (j == i or tot + todo[j].out_w * todo[j].out_h / 1e6 <= mpx):
            tot += todo[j].out_w * todo[j].out_h / 1e6
            j += 1
        yield i, todo[i:j]
        i = j


def run(items, o, say=print):
    """Builds every item that is not already up to date. Returns a report dict."""
    below_normal()
    work, pack = Path(o.work), Path(o.pack)
    work.mkdir(parents=True, exist_ok=True)
    pack.mkdir(parents=True, exist_ok=True)
    write_atomic(pack / 'pack.json', (json.dumps((o.pack_meta or pack_json(o.level, o.settings)), indent=1) + '\n').encode())
    st = load_state(work)
    retire_stale(pack, work, st, {it.rel for it in items}, say)
    todo = []
    reused = 0
    for it in items:
        rec = st['outputs'].get(it.rel)
        if rec and rec['key'] == it.key and (pack / it.rel).is_file():
            continue
        if restore_retired(pack, work, st, it):
            reused += 1
            continue
        todo.append(it)
    if o.limit:
        todo = todo[:o.limit]
    save_state(work, st)
    say(f'{len(items) - len(todo)} of {len(items)} textures already up to date ({reused} brought back from an earlier level); {len(todo)} to build')
    up = Upscaler(o.make_engine)
    t_start = time.perf_counter()
    paused = 0.0
    done = failed = 0
    tmp = work / 'tmp'
    for b0, batch in batches(todo, o.batch, o.batch_mpx):
        paused += wait_for_locks(o.lock_dir, say)
        if o.pause_game:
            paused += wait_for_game(say)
        shutil.rmtree(tmp, ignore_errors=True)
        (tmp / 'src').mkdir(parents=True)
        # 1. decode level 0 of every source (one texconv run per decode format)
        tex = {}
        groups = {}
        for i, it in enumerate(batch):
            t = nb.parse((Path(o.cache) / it.rel).read_bytes())
            tex[it.rel] = t
            d = tmp / 'src' / f'{i:04d}.dds'
            nb.write_dds(d, nb.FORMATS[t.fmt][5], t.w, t.h, [nb.level_blocks(t, 0)])
            groups.setdefault(decode_dxgi(t.fmt), []).append((it, d))
        px = {}
        for dx, lst in groups.items():
            texconv(o.texconv, [d for _, d in lst], tmp / f'dec{dx}', dx, ('-m', '1'), work=tmp)
            for it, d in lst:
                gdx, w, h, lv = nb.read_dds(tmp / f'dec{dx}' / d.name)
                px[it.rel] = nb.pixels(lv[0], gdx, w, h)
        # 2. AI passes, grouped by model
        wrap = {it.rel: it.cat == 'scenery' and im.tiles_seamlessly(px[it.rel].astype(np.float32) / 255) for it in batch}
        ai = {}
        by_model = {}
        for it in batch:
            if it.cat in hc.AI_CATEGORIES and it.model:
                by_model.setdefault((it.model, it.denoise > 0, it.scale), []).append(it)
        light = {}
        for it in batch:
            if it.cat == 'lightmap' and it.model and it.scale > 1:
                light[it.rel] = light_prepare(px[it.rel])
                by_model.setdefault((it.model, False, it.scale), []).append(it)
        for (model, dn, scale), its in by_model.items():
            reqs = {}
            for it in its:
                if it.rel in light:
                    reqs[it.rel] = im.pad(light[it.rel][0], PAD, False)
                    continue
                rgb = im.pad(px[it.rel][..., :3].astype(np.float32) / 255, PAD, wrap[it.rel])
                reqs[it.rel] = rgb
            if dn:
                clean = up.run('dejpg', list(reqs.items()))
                reqs = {k: v + (clean[k] - v) * next(i.denoise for i in its if i.rel == k) for k, v in reqs.items()}
            res, got = ai_upscale(up, model, o.model_scale, reqs, scale)
            for k, v in res.items():
                ai[k] = (v if k in light else v.astype(np.float16), got)   # light maps stay float32: log light must not band
        # 3. per-category processing, mips, then one texconv encode run per output format
        enc = {}
        pool = ThreadPoolExecutor(SHADOW_THREADS)   # numpy releases the GIL in the resize: two threads halve the shadow time
        pre = {it.rel: pool.submit(process_shadow, it, px[it.rel], False) for it in batch if it.cat == 'shadow'}
        for i, it in enumerate(batch):
            try:
                p = px[it.rel]
                if it.cat in hc.AI_CATEGORIES:
                    r = ai.get(it.rel)
                    levels = process_color(it, p, r[0].astype(np.float32) if r else None, r[1] if r else 1, wrap[it.rel])
                    in_dx = nb.DXGI_RGBA8_SRGB if nb.FORMATS[it.src_fmt][4] else nb.DXGI_RGBA8
                elif it.cat == 'normal':
                    levels, in_dx = process_normal(it, p, wrap[it.rel]), nb.DXGI_RGBA8
                elif it.cat == 'shadow':
                    levels = pre[it.rel].result()
                    in_dx = nb.DXGI_RGBA8_SRGB if nb.FORMATS[it.src_fmt][4] else nb.DXGI_RGBA8
                elif it.cat == 'lightmap':
                    r = ai.get(it.rel)
                    levels = process_lightmap(it, p, wrap[it.rel], r[0] if r else None, light.get(it.rel), r[1] if r else 1)
                    in_dx = nb.DXGI_RGBA16F
                else:
                    levels = process_filter(it, p, wrap[it.rel])
                    in_dx = nb.DXGI_RGBA8_SRGB if nb.FORMATS[it.src_fmt][4] else nb.DXGI_RGBA8
                d = tmp / 'enc_in' / f'{i:04d}.dds'
                d.parent.mkdir(exist_ok=True)
                nb.write_dds(d, in_dx, it.out_w, it.out_h, levels)
                # one-level textures go in their own texconv run with -m 1, or texconv would add a mip chain
                enc.setdefault((nb.FORMATS[it.out_fmt][5], it.out_mips == 1), []).append((it, d))
            except Exception as e:   # one bad texture must not stop the run
                failed += 1
                say(f'  {it.rel}: {type(e).__name__}: {e}')
        pool.shutdown()
        for (dx, one), lst in enc.items():
            extra = (('-at', '0.5') if dx in (71, 72) else ()) + (('-m', '1') if one else ())
            od = tmp / f'enc{dx}_{int(one)}'
            texconv(o.texconv, [d for _, d in lst], od, dx, extra, work=tmp)
            for it, d in lst:
                gdx, w, h, lv = nb.read_dds(od / d.name)
                if len(lv) != it.out_mips or (w, h) != (it.out_w, it.out_h):
                    failed += 1
                    say(f'  {it.rel}: texconv wrote {w}x{h} with {len(lv)} mips, wanted {it.out_w}x{it.out_h} with {it.out_mips}')
                    continue
                rows = [nb.rows(l, gdx, max(w >> m, 1), max(h >> m, 1)) for m, l in enumerate(lv)]
                write_atomic(pack / it.rel, nb.build(tex[it.rel], it.out_fmt, w, h, rows))
                st['outputs'][it.rel] = {'key': it.key, 'bytes': it.out_bytes, 'cat': it.cat}
                done += 1
        if o.ab_orig:
            write_ab_originals(batch, tex, px, o, tmp)
        save_state(work, st)
        el = time.perf_counter() - t_start - paused
        left = len(todo) - b0 - len(batch)
        rate = el / max(b0 + len(batch), 1)
        say(f'  {b0 + len(batch)}/{len(todo)} done ({failed} failed), {el / 60:.1f} min so far, about {left * rate / 60:.0f} min left')
    shutil.rmtree(tmp, ignore_errors=True)
    total = time.perf_counter() - t_start
    return {'built': done, 'failed': failed, 'up_to_date': len(items) - len(todo), 'seconds': total - paused, 'paused_seconds': paused,
            'ai_seconds': up.ai_seconds, 'engines': {k: getattr(e, 'providers', []) for k, e in up.engines.items()}}


def write_ab_originals(batch, tex, px, o, tmp):
    """For the in-game A/B (SNAP_MODS_AB_ORIG=<work>/ab-orig): the original texture at its own size, in the OUTPUT format,
    so the renderer can pair it with the replacement. Same format: a hard link to the cache file (no extra disk)."""
    root = Path(o.work) / 'ab-orig'
    conv = []
    for i, it in enumerate(batch):
        dst = root / it.rel
        if dst.exists():
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        if it.out_fmt == it.src_fmt:
            try:
                os.link(Path(o.cache) / it.rel, dst)
            except OSError:
                shutil.copyfile(Path(o.cache) / it.rel, dst)
            continue
        srgb = nb.FORMATS[it.src_fmt][4]
        f = px[it.rel].astype(np.float32) / 255
        if srgb:
            f = np.concatenate([im.srgb_to_lin(f[..., :3]), f[..., 3:]], -1)
        n = 1 if it.src_mips == 1 else im.mip_count(it.src_w, it.src_h)
        levels = [to_store(l, srgb) for l in im.mip_chain(f, n, 'color')]
        d = tmp / 'ab_in' / f'{i:04d}.dds'
        d.parent.mkdir(exist_ok=True)
        nb.write_dds(d, nb.DXGI_RGBA8_SRGB if srgb else nb.DXGI_RGBA8, it.src_w, it.src_h, levels)
        conv.append((it, d))
    by = {}
    for it, d in conv:
        by.setdefault((nb.FORMATS[it.out_fmt][5], it.src_mips == 1), []).append((it, d))
    for (dx, one), lst in by.items():
        od = tmp / f'ab{dx}_{int(one)}'
        texconv(o.texconv, [d for _, d in lst], od, dx, ('-m', '1') if one else (), work=tmp)
        for it, d in lst:
            gdx, w, h, lv = nb.read_dds(od / d.name)
            rows = [nb.rows(l, gdx, max(w >> m, 1), max(h >> m, 1)) for m, l in enumerate(lv)]
            write_atomic(root / it.rel, nb.build(tex[it.rel], it.out_fmt, w, h, rows))
