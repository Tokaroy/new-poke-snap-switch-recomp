# SPDX-License-Identifier: MIT
"""NUTEXB and DDS containers for the HD texture builder (numpy only; no game data here).

A NUTEXB is the game's texture file: block data in the Tegra X1 block-linear layout, then a 0xb0-byte footer:
  +0x00 u32 mip_sizes[16]  +0x40 " XNT"  +0x44 char name[0x40]  +0x84 u32 width, height, depth, format
  +0x94 u32 dim (2 = 2D, 3 = 3D, 4 = cube)  +0x98 u32 mips  +0x9c u32 tile  +0xa0 u32 layers
  +0xa4 u32 data_size  +0xa8 " XET"  +0xac u32 version (checked against the game's own files)
The layout rule matches tests/nvn/nutexb_ref.py, which was checked against every texture of the game.

The builder never decodes or encodes BC blocks itself: it deswizzles to linear blocks, wraps them in a DDS, and lets
texconv (DirectXTex, MIT) decode or encode; then it swizzles the result back. DDS here = DX10 header, 2D, one array slice.
"""
import struct
from dataclasses import dataclass, field

import numpy as np

FOOTER = 0xb0
MAGIC = 0x54455820   # " XET"

# nutexb format -> (bytes per block, block w, block h, kind, srgb, DXGI format)
FORMATS = {
    0x100: (1, 1, 1, 'r8', False, 61),
    0x400: (4, 1, 1, 'rgba8', False, 28), 0x405: (4, 1, 1, 'rgba8', True, 29),
    0x434: (16, 1, 1, 'rgba32f', False, 2), 0x435: (8, 1, 1, 'rgba16f', False, 10),
    0x480: (8, 4, 4, 'bc1', False, 71), 0x485: (8, 4, 4, 'bc1', True, 72),
    0x490: (16, 4, 4, 'bc2', False, 74), 0x495: (16, 4, 4, 'bc2', True, 75),
    0x4a0: (16, 4, 4, 'bc3', False, 77), 0x4a5: (16, 4, 4, 'bc3', True, 78),
    0x180: (8, 4, 4, 'bc4', False, 80), 0x280: (16, 4, 4, 'bc5', False, 83),
    0x4d7: (16, 4, 4, 'bc6u', False, 95), 0x4d8: (16, 4, 4, 'bc6s', False, 96),
    0x4e0: (16, 4, 4, 'bc7', False, 98), 0x4e5: (16, 4, 4, 'bc7', True, 99),
}
BY_KIND = {}
for _f, _v in FORMATS.items():
    BY_KIND.setdefault((_v[3], _v[4]), _f)

DXGI_RGBA8, DXGI_RGBA8_SRGB, DXGI_RGBA16F, DXGI_RGBA32F, DXGI_R8 = 28, 29, 10, 2, 61
DXGI_BPP = {28: 4, 29: 4, 10: 8, 2: 16, 61: 1}           # uncompressed formats: bytes per pixel
DXGI_BC = {71: 8, 72: 8, 74: 16, 75: 16, 77: 16, 78: 16, 80: 8, 83: 16, 95: 16, 96: 16, 98: 16, 99: 16}


class NutexbError(ValueError):
    pass


@dataclass
class Tex:
    data: bytes
    footer: bytes
    w: int
    h: int
    d: int
    fmt: int
    dim: int
    mips: int
    tile: int
    layers: int
    data_size: int
    mip_sizes: tuple = field(default_factory=tuple)

    @property
    def info(self):
        return FORMATS.get(self.fmt)

    @property
    def kind(self):
        return FORMATS[self.fmt][3] if self.fmt in FORMATS else f'fmt{self.fmt:x}'

    @property
    def srgb(self):
        return FORMATS[self.fmt][4] if self.fmt in FORMATS else False

    @property
    def plain_2d(self):
        """2D, one layer, depth 1, block-linear: the textures the builder can upscale."""
        return self.dim == 2 and self.layers == 1 and self.d == 1 and self.tile != 0 and self.fmt in FORMATS


def parse(blob):
    if len(blob) < FOOTER:
        raise NutexbError('too short for a nutexb')
    t = bytes(blob[-FOOTER:])
    sizes = struct.unpack_from('<16I', t, 0)
    w, h, dep, fmt, dim, mips, tile, layers, dsz, magic, ver = struct.unpack_from('<11I', t, FOOTER - 44)
    if magic != MAGIC:
        raise NutexbError('not a nutexb (footer magic)')
    if dsz > len(blob) - FOOTER:
        raise NutexbError('data size past the end of the file')
    return Tex(bytes(blob[:-FOOTER]), t, w, h, dep, fmt, dim, mips, tile, layers, dsz, sizes)


def name_of(tex):
    raw = tex.footer[0x44:0x84].split(b'\0', 1)[0]
    return raw.decode('ascii', 'replace')


# ---- Tegra X1 block-linear layout -------------------------------------------------------------------------------------

def _bh_log2(hb):
    hh = hb + hb // 2
    return 4 if hh >= 128 else 3 if hh >= 64 else 2 if hh >= 32 else 1 if hh >= 16 else 0


def _level_bh(bh0, hb):
    while bh0 > 0 and (8 << (bh0 - 1)) >= hb:
        bh0 -= 1
    return bh0


def layout(w, h, bpb, bw, bh, mips):
    """2D single layer: list of (offset, wb, hb, bh_log2, size) per mip, and the total size."""
    bh0 = _bh_log2(-(-h // bh))
    out, off = [], 0
    for m in range(mips):
        mw, mh = max(w >> m, 1), max(h >> m, 1)
        wb, hb = -(-mw // bw), -(-mh // bh)
        lbh = _level_bh(bh0, hb)
        size = (-(-wb * bpb // 64)) * 512 * (1 << lbh) * (-(-hb // (8 << lbh)))
        out.append((off, wb, hb, lbh, size))
        off += size
    return out, off


def _addr(rowb, hb, bhl):
    x = np.arange(rowb, dtype=np.int64)[None, :]
    y = np.arange(hb, dtype=np.int64)[:, None]
    gob_rows = 8 << bhl
    width_gobs = -(-rowb // 64)
    block_size = 512 << bhl
    a = (y // gob_rows) * width_gobs * block_size + (x // 64) * block_size + ((y % gob_rows) // 8) * 512
    return a + ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)


def deswizzle(src, off, wb, hb, bpb, bhl):
    """block-linear bytes -> linear block rows, shape (hb, wb*bpb) uint8."""
    buf = np.frombuffer(src, dtype=np.uint8)
    return buf[off + _addr(wb * bpb, hb, bhl)]


def swizzle(lin, size, wb, hb, bpb, bhl):
    """linear block rows (hb, wb*bpb) -> block-linear bytes of `size`."""
    out = np.zeros(size, np.uint8)
    out[_addr(wb * bpb, hb, bhl)] = lin
    return out


def level_blocks(tex, mip=0):
    """Linear block bytes (hb, wb*bpb) of one mip of a plain 2D texture."""
    if not tex.plain_2d:
        raise NutexbError('only plain 2D textures')
    bpb, bw, bh = FORMATS[tex.fmt][:3]
    lv, total = layout(tex.w, tex.h, bpb, bw, bh, tex.mips)
    if total > len(tex.data):
        raise NutexbError('layout is larger than the data')
    off, wb, hb, bhl, _ = lv[mip]
    return deswizzle(tex.data, off, wb, hb, bpb, bhl)


def build(template, fmt, w, h, level_rows):
    """A new nutexb: the template's footer (name, tile, version) with a new format, size and mip chain.
    level_rows: linear block rows per mip, level 0 first (as from read_dds + rows())."""
    if fmt not in FORMATS:
        raise NutexbError(f'format {fmt:#x} not supported')
    bpb, bw, bh = FORMATS[fmt][:3]
    lv, total = layout(w, h, bpb, bw, bh, len(level_rows))
    buf = bytearray()
    sizes = []
    for (off, wb, hb, bhl, size), rows in zip(lv, level_rows):
        rows = np.asarray(rows, np.uint8)
        if rows.shape != (hb, wb * bpb):
            raise NutexbError(f'mip {len(sizes)}: rows {rows.shape}, want {(hb, wb * bpb)}')
        buf += swizzle(rows, size, wb, hb, bpb, bhl).tobytes()
        sizes.append(size)
    foot = bytearray(template.footer)
    struct.pack_into('<16I', foot, 0, *(sizes + [0] * (16 - len(sizes))))
    struct.pack_into('<IIII', foot, 0x84, w, h, 1, fmt)
    struct.pack_into('<I', foot, 0x98, len(sizes))
    struct.pack_into('<I', foot, 0xa4, len(buf))
    return bytes(buf) + bytes(foot)


# ---- DDS (DX10 header) ------------------------------------------------------------------------------------------------

def _pitch(dxgi, w, h):
    if dxgi in DXGI_BC:
        return -(-w // 4) * DXGI_BC[dxgi], -(-h // 4)
    return w * DXGI_BPP[dxgi], h


def write_dds(path, dxgi, w, h, levels):
    """levels: bytes-like per mip (linear: block rows for BC, pixel rows for uncompressed)."""
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | (0x20000 if len(levels) > 1 else 0)
    hdr = struct.pack('<4s7I44x', b'DDS ', 124, flags, h, w, 0, 0, len(levels))
    pf = struct.pack('<2I4s5I', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
    caps = struct.pack('<5I', 0x1000 | (0x400008 if len(levels) > 1 else 0), 0, 0, 0, 0)
    dx10 = struct.pack('<5I', dxgi, 3, 0, 1, 0)
    with open(path, 'wb') as f:
        f.write(hdr + pf + caps + dx10)
        for lv in levels:
            f.write(bytes(np.ascontiguousarray(lv).tobytes() if isinstance(lv, np.ndarray) else lv))


def read_dds(path):
    """-> (dxgi, w, h, [level bytes]). DX10 header expected (texconv writes one for every format it outputs here)."""
    with open(path, 'rb') as f:
        b = f.read()
    if b[:4] != b'DDS ':
        raise NutexbError(f'{path}: not a DDS')
    h, w = struct.unpack_from('<2I', b, 12)
    mips = max(struct.unpack_from('<I', b, 28)[0], 1)
    fourcc = b[84:88]
    off = 128
    if fourcc == b'DX10':
        dxgi = struct.unpack_from('<I', b, 128)[0]
        off = 148
    else:
        bits, rmask = struct.unpack_from('<2I', b, 88)
        legacy = {b'DXT1': 71, b'DXT5': 77, b'ATI1': 80, b'BC4U': 80, b'ATI2': 83, b'BC5U': 83}
        if fourcc in legacy:
            dxgi = legacy[fourcc]
        elif bits == 32 and rmask == 0xff:
            dxgi = DXGI_RGBA8
        else:
            raise NutexbError(f'{path}: DDS pixel format not handled')
    levels = []
    for m in range(mips):
        mw, mh = max(w >> m, 1), max(h >> m, 1)
        pitch, rows = _pitch(dxgi, mw, mh)
        n = pitch * rows
        if off + n > len(b):
            break
        levels.append(b[off:off + n])
        off += n
    return dxgi, w, h, levels


def rows(level, dxgi, w, h):
    """bytes of one DDS level -> (rows, pitch) uint8 array."""
    pitch, nrows = _pitch(dxgi, w, h)
    return np.frombuffer(level, np.uint8, pitch * nrows).reshape(nrows, pitch)


def pixels(level, dxgi, w, h):
    """bytes of an uncompressed DDS level -> (h, w, c) array (uint8, float16 or float32)."""
    if dxgi in (DXGI_RGBA8, DXGI_RGBA8_SRGB):
        return np.frombuffer(level, np.uint8, w * h * 4).reshape(h, w, 4)
    if dxgi == DXGI_RGBA16F:
        return np.frombuffer(level, np.float16, w * h * 4).reshape(h, w, 4)
    if dxgi == DXGI_RGBA32F:
        return np.frombuffer(level, np.float32, w * h * 4).reshape(h, w, 4)
    if dxgi == DXGI_R8:
        return np.frombuffer(level, np.uint8, w * h).reshape(h, w, 1)
    raise NutexbError(f'DXGI {dxgi} is not an uncompressed format')
