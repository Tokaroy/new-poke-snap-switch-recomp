import json, glob, sys, numpy as np
from PIL import Image
for f in sorted(glob.glob('scripts/hd/species/*.json')):
    s = json.load(open(f)); a = s['asset']; td = 'build/hd/cache/%s/textures/' % a
    for st in s['sets']:
        n = st.get(sys.argv[1] if len(sys.argv) > 1 else 'dif')
        if not n: continue
        pre = None; pre = td + 'CH_%s_00_0_A00_%s' % (s['num'], n)
        try:
            o = Image.open(pre + '.orig.png').convert('RGB'); m = Image.open(pre + '.x4_mat.png').convert('RGB')
        except Exception as e: print(f, 'missing', e); continue
        O = np.asarray(o).astype(float); M = np.asarray(m.resize(o.size, Image.BOX)).astype(float)
        M4 = np.asarray(m).astype(float); O4 = np.asarray(o.resize(m.size, Image.LANCZOS)).astype(float)
        dl = (M - O).mean(axis=(0, 1)); rms = np.sqrt(((M4 - O4) ** 2).mean())
        nm = f.replace(chr(92), '/').split('/')[-1][:-5]; print('%-11s %-9s mean dRGB %5.1f %5.1f %5.1f | rms@4096 %5.1f | luma %5.1f->%5.1f' % (nm, n, *dl, rms, O.mean(), M.mean()))
