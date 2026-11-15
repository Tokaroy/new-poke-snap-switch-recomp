# SPDX-License-Identifier: MIT
"""AI upscale engines for the HD texture builder. Both take and return float32 RGB in [0, 1] (sRGB-encoded, which is
what the models were trained on) and never see alpha.

  OrtEngine   ONNX Runtime with DirectML (any DirectX 12 GPU), in-process, tiled with an overlap so tile seams never show.
  NcnnEngine  realesrgan-ncnn-vulkan (MIT, Vulkan GPUs), a batch of PNGs per process start. It is the only way to run the
              author's Real-ESRGAN x4plus files, which are published for ncnn only.
"""
import os
import struct
import subprocess
import time
import zlib
from pathlib import Path

import numpy as np

from . import hdtex_nutexb as nb


class EngineError(RuntimeError):
    pass


class OrtEngine:
    name = 'ort-directml'

    def __init__(self, model_path, scale, fixed=None, device_id=0, tile=256, overlap=16, cpu=False):
        import onnxruntime as ort
        so = ort.SessionOptions()
        so.enable_mem_pattern = False   # DirectML requirement
        so.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        so.log_severity_level = 3
        prov = ['CPUExecutionProvider'] if cpu else [('DmlExecutionProvider', {'device_id': int(device_id)}), 'CPUExecutionProvider']
        t0 = time.perf_counter()
        self.sess = ort.InferenceSession(str(model_path), so, providers=prov)
        self.load_s = time.perf_counter() - t0
        inp = self.sess.get_inputs()[0]
        self.input = inp.name
        self.half = 'float16' in inp.type
        shp = inp.shape
        if fixed is None and isinstance(shp[2], int) and isinstance(shp[3], int):
            fixed = shp[2]
        self.fixed = fixed
        self.scale = int(scale)
        self.overlap = int(overlap)
        self.tile = int(fixed) if fixed else int(tile)
        self.providers = self.sess.get_providers()

    def _run(self, x):
        x = x.transpose(2, 0, 1)[None]
        x = x.astype(np.float16 if self.half else np.float32)
        y = self.sess.run(None, {self.input: np.ascontiguousarray(x)})[0][0]
        return np.clip(y.astype(np.float32).transpose(1, 2, 0), 0, 1)

    def upscale(self, rgb):
        """rgb (h, w, 3) float32 in [0, 1] -> (h*s, w*s, 3)."""
        rgb = np.asarray(rgb, np.float32)[..., :3]
        h, w = rgb.shape[:2]
        s, ov = self.scale, self.overlap
        core = self.tile - 2 * ov
        if not self.fixed and h <= self.tile and w <= self.tile:
            return self._run(rgb)
        src = np.pad(rgb, ((ov, ov + core), (ov, ov + core), (0, 0)), mode='edge')   # room for the last partial tile
        out = np.zeros((h * s, w * s, 3), np.float32)
        for y in range(0, h, core):
            for x in range(0, w, core):
                t = src[y:y + self.tile, x:x + self.tile]
                r = self._run(t)
                ch, cw = min(core, h - y), min(core, w - x)
                out[y * s:(y + ch) * s, x * s:(x + cw) * s] = r[ov * s:(ov + ch) * s, ov * s:(ov + cw) * s]
        return out

    def upscale_many(self, items):
        return {k: self.upscale(v) for k, v in items}


# ---- PNG (8-bit RGB, filter 0) for the ncnn exe; results come back through texconv as DDS -------------------------------

def write_png(path, rgb8):
    h, w, c = rgb8.shape
    raw = np.concatenate([np.zeros((h, 1), np.uint8), np.ascontiguousarray(rgb8).reshape(h, w * c)], 1).tobytes()

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    color = {3: 2, 4: 6}[c]
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, color, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 1)) + chunk(b'IEND', b'')
    Path(path).write_bytes(data)


def to8(x):
    return (np.clip(x, 0, 1) * 255 + 0.5).astype(np.uint8)


class NcnnEngine:
    name = 'ncnn-vulkan'

    def __init__(self, exe, model_dir, model_name, scale, texconv, work, gpu=0, tile=0):
        self.exe, self.model_dir, self.model_name = Path(exe), Path(model_dir), model_name
        self.scale, self.texconv, self.work, self.gpu, self.tile = int(scale), Path(texconv), Path(work), gpu, tile
        self.load_s = 0.0
        self.providers = ['vulkan']

    def upscale_many(self, items):
        items = list(items)
        if not items:
            return {}
        inp, outp, dds = self.work / 'ncnn_in', self.work / 'ncnn_out', self.work / 'ncnn_dds'
        for d in (inp, outp, dds):
            d.mkdir(parents=True, exist_ok=True)
            for f in d.iterdir():
                f.unlink()
        names = {}
        for i, (k, rgb) in enumerate(items):
            n = f'{i:05d}'
            names[n] = k
            write_png(inp / (n + '.png'), to8(rgb[..., :3]))
        cmd = [str(self.exe), '-i', str(inp), '-o', str(outp), '-n', self.model_name, '-s', str(self.scale), '-m', str(self.model_dir),
               '-f', 'png', '-g', str(self.gpu), '-t', str(self.tile)]
        r = subprocess.run(cmd, capture_output=True, text=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        made = sorted(outp.glob('*.png'))
        if r.returncode != 0 or len(made) != len(items):
            raise EngineError(f'realesrgan-ncnn-vulkan failed ({r.returncode}, {len(made)} of {len(items)} images): {(r.stderr or r.stdout)[-600:]}')
        lst = self.work / 'ncnn_list.txt'
        lst.write_text('\n'.join(str(p) for p in made) + '\n', encoding='utf-8')
        tc = subprocess.run([str(self.texconv), '-nologo', '-y', '-m', '1', '-dx10', '-f', 'R8G8B8A8_UNORM_SRGB', '-o', str(dds), '-flist', str(lst)],
                            capture_output=True, text=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if tc.returncode != 0:
            raise EngineError('texconv could not read the upscaled PNGs: ' + tc.stdout[-400:])
        out = {}
        for p in made:
            dx, w, h, lv = nb.read_dds(dds / (p.stem + '.dds'))
            out[names[p.stem]] = nb.pixels(lv[0], dx, w, h)[..., :3].astype(np.float32) / 255
        return out

    def upscale(self, rgb):
        return self.upscale_many([('x', rgb)])['x']
