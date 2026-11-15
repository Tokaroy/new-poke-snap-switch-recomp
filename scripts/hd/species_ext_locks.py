# SPDX-License-Identifier: MIT
"""Mid-range fur for species_author: `fur_locks` (registered into species_author.TREAT, loaded by species_author_ext3.py).

Why: round 1 fur sat at the painted stroke scale (strands 5-6 texels long, clumps 3-4 at 1024). In game at 4K a Pokemon
400-800 px tall samples the 1024-class mips, so that detail averages away and the body looks unchanged. Real short fur
(and the live-action film look) reads at mid range through its LOCKS: groups of hairs that part, lie over each other and
leave soft shadowed gaps. So this treatment builds locks at 3-6x the strand scale, then keeps the strand field inside them
for point blank.

Each lock: a root at a random point (density from the lock size, no grid, so no pattern), lying along the painted flow
(structure tensor of the painted strokes, gravity guess where unpainted), as wide as `lock_w` at the root and tapering to a
tip `lock_len` away. Height rises from the root and falls to the tip; where locks overlap the higher one wins, so tips lie
over the roots of the next locks. Gaps and roots are the cavity (ambient occlusion in `_par` G, a slight darkening of the
root in albedo); tips are a touch lighter (light passes through hair ends). Strands follow the same flow and are stronger
on the lock crests than in the gaps. Everything stays inside its UV island. Colors are locked to the original afterwards
by species_author (low-frequency ratio), so only the fine structure changes.

params (texels at 1024): lock_len (14-26), lock_w (5-9), density (1.0 = locks just touch), amp (normal tilt RMS of the
whole field, 0.12-0.25), strand_w (share of the strand field, 0.25-0.5), strand params as t_fur (len_texels, width_texels,
painted_mix, stroke_source), tip_light (albedo +, 0.02-0.05), root_dark (albedo -, 0.02-0.05), cav (AO depth in gaps,
0.2-0.45), lock_tensor_sigma (flow smoothing for the locks, 14), curl (radians of bend along a lock, 0 = straight; Bouffalant uses a strong curl for coils), seed.
"""
import cv2
import numpy as np

import material_author as ma
import species_author as sa

F32 = np.float32


def _remap(img, x, y):
    return cv2.remap(img, x, y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def lock_field(C, p, m, fl, rng):
    """height (0..1) of the lock layer and the position t (0 root .. 1 tip) of the winning lock, at output res."""
    out, k, S = C['out'], C['k'], C['S']
    ks = k * 1024.0 / S
    L = p.get('lock_len', 18.0) * ks
    W = p.get('lock_w', 7.0) * ks
    dens = p.get('density', 1.0)
    lab = C['lab_out']
    labf = lab.astype(F32)
    # roots: Poisson points, about one per lock area (L * W / 2.2 overlap factor), only inside the region
    n = int(dens * float((m > 0.05).sum()) / max(L * W / 2.2, 1.0))
    seeds = np.zeros((out, out), F32)
    if n <= 0:
        return np.zeros((out, out), F32), np.zeros((out, out), F32)
    ys = rng.integers(0, out, n * 3)
    xs = rng.integers(0, out, n * 3)
    keep = m[ys, xs] > 0.05
    ys, xs = ys[keep][:n], xs[keep][:n]
    seeds[ys, xs] = rng.uniform(0.75, 1.0, len(ys)).astype(F32)
    # blob of the root width; normalised so a lone root peaks at about its weight
    sig = max(W * 0.35, 0.8)
    blob = cv2.GaussianBlur(seeds, (0, 0), sig) * (2 * np.pi * sig * sig)
    blob = np.clip(blob, 0, 1.2)
    # march from every pixel BACK along the flow: a root found at distance d puts this pixel at t = d / L of that lock
    r = fl.shape[0]
    ratio = r / out
    fx0, fy0 = np.ascontiguousarray(fl[..., 0]), np.ascontiguousarray(fl[..., 1])
    step = max(1.0, W * 0.25)
    nsteps = int(np.ceil(L / step))
    xs0 = np.arange(out, dtype=F32)
    px = np.broadcast_to(xs0[None, :], (out, out)).copy()
    py = np.broadcast_to(xs0[:, None], (out, out)).copy()
    alive = np.ones((out, out), F32)
    pdx = np.zeros((out, out), F32)
    pdy = np.zeros((out, out), F32)
    h = np.zeros((out, out), F32)
    tpos = np.zeros((out, out), F32)
    curl = p.get('curl', 0.0)
    for i in range(nsteps + 1):
        t = i / nsteps
        if i:
            fx = _remap(fx0, px * ratio, py * ratio)
            fy = _remap(fy0, px * ratio, py * ratio)
            sgn = np.where(fx * pdx + fy * pdy < 0, -1.0, 1.0).astype(F32)
            fx, fy = fx * sgn, fy * sgn
            if curl:
                a = curl / nsteps
                ca, sa_ = F32(np.cos(a)), F32(np.sin(a))
                fx, fy = fx * ca - fy * sa_, fx * sa_ + fy * ca
            pdx, pdy = fx, fy
            px = px - step * fx
            py = py - step * fy
            alive *= (cv2.remap(labf, px, py, cv2.INTER_NEAREST, borderMode=cv2.BORDER_REPLICATE) == labf).astype(F32)
        s = _remap(blob, px, py) * alive
        thr = 0.18 + 0.62 * t                                     # the lock narrows toward its tip
        w = sa.smoothstep(thr, thr + 0.22, s)
        prof = (t ** 0.7) * (1.0 - t) ** 0.45 / 0.53                  # tucked root, crest past the middle, thin tip
        v = (w * prof).astype(F32)
        better = v > h
        tpos = np.where(better, t, tpos)
        h = np.maximum(h, v)
    return h, tpos


def t_fur_locks(C, p, m, ms):
    out, k, S = C['out'], C['k'], C['S']
    flow_geo, conf_geo = C['flows'][p.get('fallback_flow', 'fur')]
    # locks follow the broad lie of the fur, not every painted stroke: a wider tensor window than the strands use
    fl, coh = sa.painted_flow(C, dict(p, tensor_sigma=p.get('lock_tensor_sigma', 14.0)), flow_geo, conf_geo)
    rng = np.random.default_rng(C['seed'] + p.get('seed', 0) + 707)
    h_lock, tpos = lock_field(C, p, m, fl, rng)
    soft = p.get('lock_soft', 0.6) * k
    h_lock = sa.gb(h_lock, soft)
    # strands (point blank) from the existing fur treatment, stronger on lock crests
    fur = sa.t_fur(C, dict(p, amp=1.0), m, ms)
    strands = sa.unit_std(fur['h'], m)
    hl = sa.unit_std(h_lock, m)
    sw = p.get('strand_w', 0.35)
    crest = np.clip(h_lock / max(float(np.percentile(h_lock[m > 0.5], 95)) if (m > 0.5).any() else 1.0, 1e-3), 0, 1)
    h = (1 - sw) * hl + sw * strands * (0.45 + 0.55 * crest)
    nx, ny = sa.tilt(h, p.get('amp', 0.16), m)
    gap = np.clip(1.0 - crest, 0, 1)
    cav = np.clip(0.65 * gap + 0.35 * np.clip(-strands * 0.4, 0, 1), 0, 1)
    tip = np.clip((tpos - 0.45) / 0.55, 0, 1) * crest
    root = np.clip(1 - tpos / 0.3, 0, 1) * crest
    alb = (p.get('tip_light', 0.03) * tip - p.get('root_dark', 0.03) * (root + 0.6 * gap)
           + p.get('alb_amp', 0.015) * strands * 0.5)
    alb = alb - float(alb[m > 0.5].mean()) if (m > 0.5).any() else alb
    return dict(nx=nx, ny=ny, alb=alb.astype(F32), cav=(cav * p.get('cav', 0.3)).astype(F32), h=h)


sa.TREAT['fur_locks'] = t_fur_locks


def t_stack(C, p, m, ms):
    """several treatments on one region: params {"layers": [{"treatment": name, "w": weight, "params": {...}}, ...]}.
    Normals, albedo and height add (weighted), cavity takes the deepest layer. Used for Bouffalant's afro: the painted cloud
    clumps (curls, large clump scale) for mid range plus curled locks for close range."""
    acc = {}
    for L in p['layers']:
        f = sa.TREAT[L['treatment']](C, L.get('params', {}), m, ms)
        w = L.get('w', 1.0)
        for key in ('nx', 'ny', 'alb', 'h'):
            if key in f:
                acc[key] = acc.get(key, 0) + w * f[key]
        if 'cav' in f:
            acc['cav'] = np.maximum(acc.get('cav', 0), w * f['cav'])
    return {k: (v.astype(F32) if hasattr(v, 'astype') else v) for k, v in acc.items()}


sa.TREAT['stack'] = t_stack
