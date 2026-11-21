# SPDX-License-Identifier: MIT
"""Image operations for the HD texture builder (numpy only).

Images are float32 arrays (h, w, c). Colour work happens in linear light; sRGB is only the storage encoding.
"""
import numpy as np


def srgb_to_lin(c):
    c = np.asarray(c, np.float32)
    return np.where(c <= 0.04045, c / 12.92, ((np.maximum(c, 0.04045) + 0.055) / 1.055) ** 2.4).astype(np.float32)


def lin_to_srgb(c):
    c = np.clip(np.asarray(c, np.float32), 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.maximum(c, 0.0031308) ** (1 / 2.4) - 0.055).astype(np.float32)


# ---- resampling -------------------------------------------------------------------------------------------------------

def _lanczos(x, a):
    x = np.asarray(x, np.float64)
    out = np.sinc(x) * np.sinc(x / a)
    return np.where(np.abs(x) < a, out, 0.0)


def _taps(n_in, n_out, a, wrap):
    """index (n_out, k) and weight (n_out, k) arrays for one axis (pixel centres aligned, like every GPU and image tool)."""
    scale = n_out / n_in
    support = a if scale >= 1 else a / scale
    centre = (np.arange(n_out) + 0.5) / scale - 0.5
    k = int(np.ceil(support)) * 2 + 1
    first = np.floor(centre - support).astype(np.int64) + 1
    idx = first[:, None] + np.arange(k)[None, :]
    dist = centre[:, None] - idx
    wts = _lanczos(dist * min(scale, 1.0), a)
    wts /= np.maximum(wts.sum(1, keepdims=True), 1e-12)
    idx = np.mod(idx, n_in) if wrap else np.clip(idx, 0, n_in - 1)
    return idx, wts.astype(np.float32)


def resize(img, w, h, wrap=False, a=3):
    """Separable Lanczos-a resize. wrap=True samples across the opposite edge (tiling textures)."""
    img = np.asarray(img, np.float32)
    squeeze = img.ndim == 2
    if squeeze:
        img = img[..., None]
    if img.shape[1] != w:
        idx, wt = _taps(img.shape[1], w, a, wrap)
        img = np.einsum('hwkc,wk->hwc', img[:, idx], wt, optimize=True)
    if img.shape[0] != h:
        idx, wt = _taps(img.shape[0], h, a, wrap)
        img = np.einsum('hkwc,hk->hwc', img[idx], wt, optimize=True)
    return img[..., 0] if squeeze else img


def tiles_seamlessly(img, tol=1.6):
    """True when the texture wraps: the jump across each opposite edge pair is no larger than a typical step between
    neighbouring rows/columns inside the image (scaled by tol)."""
    g = np.asarray(img, np.float32)[..., :3]
    if min(g.shape[:2]) < 16:
        return False
    inner_x = np.abs(np.diff(g, axis=1)).mean()
    inner_y = np.abs(np.diff(g, axis=0)).mean()
    seam_x = np.abs(g[:, 0] - g[:, -1]).mean()
    seam_y = np.abs(g[0] - g[-1]).mean()
    return seam_x <= tol * inner_x + 1e-3 and seam_y <= tol * inner_y + 1e-3


def pad(img, p, wrap):
    if p <= 0:
        return img
    return np.pad(img, ((p, p), (p, p), (0, 0)), mode='wrap' if wrap else 'reflect')


# ---- mips ---------------------------------------------------------------------------------------------------------------

def half(img):
    """2x2 box down (odd sizes: the last row/column is repeated), at least 1x1."""
    h, w = img.shape[:2]
    if h > 1 and h % 2:
        img = np.concatenate([img, img[-1:]], 0)
    if w > 1 and w % 2:
        img = np.concatenate([img, img[:, -1:]], 1)
    if img.shape[0] > 1:
        img = (img[0::2] + img[1::2]) * 0.5
    if img.shape[1] > 1:
        img = (img[:, 0::2] + img[:, 1::2]) * 0.5
    return img


def coverage(alpha, cutoff):
    return float((alpha >= cutoff).mean())


def keep_coverage(alpha, want, cutoff):
    """Scale alpha so the share of texels at or above the cutoff matches `want` (alpha-tested foliage stays as thick at
    distance as up close). Bisection on the scale factor."""
    if want <= 0 or alpha.size == 0:
        return alpha
    lo, hi = 0.0, 8.0
    for _ in range(18):
        mid = (lo + hi) / 2
        if coverage(np.clip(alpha * mid, 0, 1), cutoff) < want:
            lo = mid
        else:
            hi = mid
    return np.clip(alpha * hi, 0, 1)


def mip_chain(img, n, kind='color', alpha_cutoff=None):
    """List of n float32 levels, level 0 first.
    kind 'color': img is linear-light RGB (+ alpha); averaged as is.
    kind 'normal': img xyz in [-1, 1]; averaged then renormalised.
    alpha_cutoff: alpha-tested texture; each level's alpha keeps level 0's coverage at that cutoff."""
    out = [img]
    want = coverage(img[..., 3], alpha_cutoff) if alpha_cutoff is not None and img.shape[2] == 4 else None
    cur = img
    for _ in range(n - 1):
        cur = half(cur)
        lv = cur
        if kind == 'normal':
            lv = renormalize(cur)
        if want is not None:
            lv = lv.copy()
            lv[..., 3] = keep_coverage(cur[..., 3], want, alpha_cutoff)
        out.append(lv)
    return out


def mip_count(w, h):
    return max(w, h).bit_length()


# ---- normal maps --------------------------------------------------------------------------------------------------------

def normal_from_rg(rg):
    """two-channel unorm normal (BC5 style) in [0, 1] -> xyz in [-1, 1]."""
    x = rg[..., 0] * 2 - 1
    y = rg[..., 1] * 2 - 1
    z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
    return np.stack([x, y, z], -1).astype(np.float32)


def renormalize(n):
    v = n[..., :3]
    ln = np.sqrt((v * v).sum(-1, keepdims=True))
    v = v / np.maximum(ln, 1e-6)
    return np.concatenate([v, n[..., 3:]], -1) if n.shape[-1] > 3 else v


def rg_from_normal(n):
    return np.clip((n[..., :2] + 1) * 0.5, 0, 1)


# ---- HDR (light maps) ---------------------------------------------------------------------------------------------------

def to_log(x):
    return np.log2(1 + np.maximum(x, 0)).astype(np.float32)


def from_log(y):
    return (np.exp2(np.maximum(y, 0)) - 1).astype(np.float32)


# ---- detail blend -------------------------------------------------------------------------------------------------------

def blend_detail(base, ai, strength):
    """strength 0 = plain Lanczos, 1 = the model's output; the low frequencies (and so the average colour) follow the base."""
    if strength >= 1:
        return ai
    return base + (ai - base) * float(max(strength, 0.0))


def back_project(out, src, wrap=False, iters=1):
    """Holds an upscale to its source: the part of the source the upscale lost or changed (src - down(out)) is enlarged and
    added back. Downscaled again, the result matches the source, so AI colour drift (spanimation2 tinted menu art 3 to 6
    levels warm) and thin low-contrast lines the model smoothed away come back, while the model's edges stay. Linear light."""
    sh, sw = src.shape[:2]
    for _ in range(iters):
        out = np.clip(out + resize(src - resize(out, sw, sh, wrap=wrap), out.shape[1], out.shape[0], wrap=wrap), 0, None)
    return out.astype(np.float32)


def gauss(x, s, wrap=False):
    """Separable Gaussian blur (sigma s px) over the first two axes; wrap for tiling textures, else edge."""
    r = max(1, int(3 * s))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / s) ** 2)
    k /= k.sum()
    mode = 'wrap' if wrap else 'edge'
    o = np.pad(x, ((r, r),) + ((0, 0),) * (x.ndim - 1), mode=mode)
    o = sum(k[i] * o[i:i + x.shape[0]] for i in range(2 * r + 1))
    o = np.pad(o, ((0, 0), (r, r)) + ((0, 0),) * (x.ndim - 2), mode=mode)
    return sum(k[i] * o[:, i:i + x.shape[1]] for i in range(2 * r + 1)).astype(np.float32)


def local_contrast(rgb, amount, wrap=True, s=1.2):
    """Detail for 'Ultra' tiling ground and rock: the finest band of the upscaled image's own luminance (sigma s output
    px), relative to the local light level, added back amount times. Grain gets crisper; colour, the average and the
    tiling stay. rgb: linear light; returns the sharpened rgb (never negative)."""
    lum = rgb[..., :3].mean(-1)
    band = (lum - gauss(lum, s, wrap)) / np.maximum(gauss(lum, 4 * s, wrap), 1e-3)
    return np.clip(rgb[..., :3] + np.maximum(rgb[..., :3], 1e-4) * band[..., None] * float(amount), 0, None)
