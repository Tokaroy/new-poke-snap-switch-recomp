# SPDX-License-Identifier: MIT
"""Sorts every captured texture into the category that decides how it is upscaled.

Rules come from the census of all 28,369 textures of game version 2.0.1 (format, archive group and name suffix):
  creature  Pokemon, people and item colour maps (CH_*), AI with a low detail strength, never chunky fur
  scenery   level and shared colour maps (LevelComposite, Resident, Other), AI; tiling ones get wrap padding
  ui        everything under common/ui (menus, icons, silhouettes, stamps, text) and *_thumbnail: the UI model or a font-safe filter
  effect    FX_* particles, gradients and flashes: soft by design, Lanczos only
  normal    BC5 or *_nor / *_n: upscaled as vectors and renormalised, no AI
  mask      parameter maps, single-channel data, splat/route masks (*_par, BC4, *_A/_B/_Route*), no AI
  shadow    the courses' baked per-mesh sun-shadow maps (*^Shadow: black RGB, the shadow in alpha): edge-aware alpha upscale
            (HD shadows row), never touched by the other rows
  emissive  *_emi*: Lanczos, no AI (glow shapes must not grow detail)
  lightmap  BC6H light maps (the ^0^Lightmap of each set, and any other BC6H without an _emi suffix): the light the
            courses bake, shadows included. HDR-safe AI (Lighting row): log space, gated to shadow edges, energy kept
  lightdir  the ^1^, ^2^ and ^3^Lightmap of a set: signed directional terms of the light, not light. Never changed
  skip      data, not pictures (float curves and dither tables), cube maps, arrays, tiny or 1-pixel strips
"""
import re

CATEGORIES = ('creature', 'scenery', 'ui', 'effect', 'normal', 'mask', 'shadow', 'emissive', 'lightmap', 'lightdir', 'skip')
AI_CATEGORIES = ('creature', 'scenery', 'ui')

_SKIP_SUFFIX = re.compile(r'_(curve|dither|cubemap|specular|lut|noise)\d*$', re.I)
_NORMAL = re.compile(r'_(nor|n|nrm|normal)\d*$', re.I)
_MASK = re.compile(r'_(par|a|b|c|d|msk|mask|route\d*|flp|rgh|ao|spc|tr|alp|alpha|op|id)\d*$', re.I)
_EMI = re.compile(r'_emi\d*$', re.I)
_LIGHTDIR = re.compile(r'\^[1-9]\d*\^lightmap$', re.I)   # light map sets: ^0^ light, ^1^..^3^ directional terms (signed)


def stem(rel):
    name = rel.rsplit('/', 1)[-1]
    return name[:-7] if name.lower().endswith('.nutexb') else name


def classify(rel, tex):
    """rel: '<archive>/<name>.nutexb' as in the texture cache; tex: hdtex_nutexb.Tex. Returns a category name."""
    r = rel.lower()
    s = stem(rel)
    kind = tex.kind
    if not tex.plain_2d or kind in ('rgba32f', 'rgba16f') or _SKIP_SUFFIX.search(s):
        return 'skip'
    if min(tex.w, tex.h) < 16 or max(tex.w, tex.h) < 32:
        return 'skip'
    if kind in ('bc6u', 'bc6s'):
        if _EMI.search(s):
            return 'emissive'
        return 'lightdir' if _LIGHTDIR.search(s) else 'lightmap'
    if kind == 'bc5' or _NORMAL.search(s):
        return 'normal'
    if _EMI.search(s):
        return 'emissive'
    if r.endswith('^shadow.nutexb'):
        return 'shadow'
    if kind in ('bc4', 'r8'):
        return 'mask'
    if r.startswith('common/ui/') or s.lower().endswith('_thumbnail'):
        return 'ui'
    if _MASK.search(s):
        return 'mask'
    low = s.lower()
    if low.startswith(('fx_', 'eff_')):
        return 'effect'
    if low.startswith('ch_') or '/characters/' in r:
        return 'creature'
    return 'scenery'


def species(rel):
    """'CH_097' style creature id of a texture, or ''. Used by the exclude list."""
    m = re.search(r'(ch_\d{3})', rel, re.I)
    return m.group(1).upper() if m else ''
