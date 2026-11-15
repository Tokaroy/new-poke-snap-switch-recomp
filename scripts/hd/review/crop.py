# crop.py <shotBase> <k> <cx> <cy> <size> <outprefix>: crop pair (k, k+1) at full res around (cx,cy); HD = more fine detail; writes _orig/_hd/_sbs
import sys, numpy as np
from PIL import Image
base, k, cx, cy, s, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
A = Image.open('%s_%02d.png' % (base, k)).convert('RGB'); B = Image.open('%s_%02d.png' % (base, k + 1)).convert('RGB')
x0 = max(0, min(A.width - s, cx - s // 2)); y0 = max(0, min(A.height - s, cy - s // 2)); box = (x0, y0, x0 + s, y0 + s)
a, b = A.crop(box), B.crop(box)
na, nb = np.asarray(a).astype(float), np.asarray(b).astype(float)
d = np.abs(na - nb).max(2) > 6
hf = lambda n: (np.abs(np.diff(n.mean(2), axis=1))[d[:, 1:]]).sum()
o, h = (a, b) if hf(nb) >= hf(na) else (b, a)
o.save(out + '_orig.png'); h.save(out + '_hd.png')
dd = Image.fromarray(np.clip(np.abs(np.asarray(o).astype(int) - np.asarray(h).astype(int)) * 4, 0, 255).astype(np.uint8))
S = Image.new('RGB', (3 * s + 16, s), 'black'); S.paste(o, (0, 0)); S.paste(h, (s + 8, 0)); S.paste(dd, (2 * s + 16, 0))
if s > 600: S = S.resize(((3 * s + 16) * 600 // s, 600), Image.LANCZOS)
S.save(out + '_sbs.png'); print('diff px %.3f' % d.mean(), 'hf o/h', int(hf(np.asarray(o).astype(float))), int(hf(np.asarray(h).astype(float))))
