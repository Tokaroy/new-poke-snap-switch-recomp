"""Shimmer metric for an SNAP_AA_PROBE capture (project tooling, ours; docs/advanced-graphics-options.md "Image stability in motion").

One host\\ab_capture.ps1 run with -Effect SNAP_AA_PROBE -KeepRun and values state*16+step (steps 1..8 per state) gives, per state, the
same frozen frame panned in 1/8 render-pixel steps. Shimmer of a state = mean absolute luma change between consecutive steps (0..255 scale),
over the foliage mask (value 16*16+1: alpha-tested draws painted red), over the rest of the frame, and over the whole frame. Pixels the
HUD animates (they differ between the first frame and the control frame at the end) are left out.

  python -E -P scripts/aa/shimmer.py --result <work>\\result.json --shots <analysis\\boot\\route_<tmp>_<run>> --out <prefix> [--crop x,y,w,h]

Writes <prefix>.json (numbers per state), <prefix>_map_<state>.png (shimmer map) and, with --crop, <prefix>_crop.png (step 1 and the
shimmer map of each state side by side, magnified 3x).
"""
import argparse, json, os, re
from collections import defaultdict
import numpy as np
from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument('--result', required=True)
ap.add_argument('--shots', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--crop', default='')
ap.add_argument('--names', default='', help='state=name,... labels for the crop sheet')
a = ap.parse_args()

R = json.load(open(a.result))
dumps = R['dumps']; states = R['states']
names = []
dl = a.shots + '_dumps.txt'
for i, ln in enumerate(l.strip() for l in open(dl) if l.strip()):
    m = re.search(r'_p(\d+)_', ln)
    names.append((i, int(m.group(1)) if m else -1))
used = set()
for d in dumps:
    d['file'] = None
    for i, p in names:
        if i in used or p < d['request_present'] - 1: continue
        used.add(i); d['file'] = '%s_%02d.png' % (a.shots, i); break

def luma(f):
    x = np.asarray(Image.open(f).convert('RGB'), dtype=np.float32)
    return x, x[..., 0] * 0.2126 + x[..., 1] * 0.7152 + x[..., 2] * 0.0722

frames = defaultdict(dict)   # state -> step -> (rgb, luma)
order = []
for d in dumps:
    if d['kind'] != 'scene' or not d['file'] or not os.path.exists(d['file']): continue
    v = int(float(states[d['state']]['value']))
    order.append(v)
    frames[v // 16][v % 16] = luma(d['file'])
first, last = order[0], order[-1]
# animated HUD pixels: the control (first value repeated at the end) against the first frame. Only meaningful when both are the same value.
anim = None
if first == last and len([o for o in order if o == first]) >= 1:
    fs = [d for d in dumps if d['kind'] == 'scene' and d['file'] and int(float(states[d['state']]['value'])) == first]
    if len(fs) >= 2:
        _, l0 = luma(fs[0]['file']); _, l1 = luma(fs[-1]['file'])
        anim = np.abs(l1 - l0) > 2
mask = None
if 16 in frames and 1 in frames[16] and 0 in frames and 1 in frames[0]:
    rm, _ = frames[16][1]; r0, _ = frames[0][1]
    mask = ((rm[..., 0] - r0[..., 0]) > 25) & ((rm[..., 0] - rm[..., 1]) > (r0[..., 0] - r0[..., 1]) + 25)
H, W = next(iter(frames[0].values()))[1].shape
valid = np.ones((H, W), bool)
if anim is not None:
    grow = anim.copy()
    for _ in range(4):   # dilate by 4 pixels
        g = grow.copy()
        g[1:] |= grow[:-1]; g[:-1] |= grow[1:]; g[:, 1:] |= grow[:, :-1]; g[:, :-1] |= grow[:, 1:]
        grow = g
    valid &= ~grow
out = {'run': R.get('freeze_present'), 'size': [W, H], 'mask_px': int(mask.sum()) if mask is not None else 0,
       'anim_px': int(anim.sum()) if anim is not None else 0, 'states': {}}
maps = {}
for s in sorted(frames):
    if s == 16: continue
    st = sorted(k for k in frames[s] if k >= 1)
    if len(st) < 3: continue
    diffs = [np.abs(frames[s][st[i + 1]][1] - frames[s][st[i]][1]) for i in range(len(st) - 1)]
    m = np.mean(diffs, axis=0)
    maps[s] = m
    e = {'steps': len(st), 'all': float(m[valid].mean())}
    if mask is not None:
        e['foliage'] = float(m[valid & mask].mean()); e['other'] = float(m[valid & ~mask].mean())
        e['foliage_p99'] = float(np.percentile(m[valid & mask], 99))
    e['other_p999'] = float(np.percentile(m[valid & (~mask if mask is not None else valid)], 99.9))
    e['mean_luma'] = float(frames[s][st[0]][1][valid].mean())
    out['states'][str(s)] = e
    Image.fromarray(np.clip(m * 8, 0, 255).astype(np.uint8)).save('%s_map_%d.png' % (a.out, s))
if mask is not None:
    Image.fromarray((mask * 255).astype(np.uint8)).save(a.out + '_mask.png')
json.dump(out, open(a.out + '.json', 'w'), indent=1)
for s, e in out['states'].items():
    print('state %3s  all %.3f  foliage %s  other %s  luma %.1f' % (s, e['all'], '%.3f' % e['foliage'] if 'foliage' in e else '-', '%.3f' % e['other'] if 'other' in e else '-', e['mean_luma']))
if a.crop:
    x, y, w, h = [int(v) for v in a.crop.split(',')]
    lab = dict(kv.split('=') for kv in a.names.split(',') if '=' in kv)
    tiles = []
    for s in maps:
        rgb = frames[s][1][0][y:y + h, x:x + w].astype(np.uint8)
        mp = np.clip(maps[s][y:y + h, x:x + w] * 8, 0, 255).astype(np.uint8)
        col = np.concatenate([rgb, np.stack([mp] * 3, -1)], 0)
        im = Image.fromarray(col).resize((w * 3, h * 6), Image.NEAREST)
        ImageDraw.Draw(im).text((6, 6), lab.get(str(s), 'state %d' % s), fill=(255, 255, 0))
        tiles.append(im)
    sheet = Image.new('RGB', (sum(t.width for t in tiles) + 4 * (len(tiles) - 1), tiles[0].height))
    xo = 0
    for t in tiles: sheet.paste(t, (xo, 0)); xo += t.width + 4
    sheet.save(a.out + '_crop.png')
