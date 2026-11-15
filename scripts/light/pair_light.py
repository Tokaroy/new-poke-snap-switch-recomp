# Collects the same-view original / Enhanced pairs of a run_light_compare.ps1 run: python pair_light.py <glob of the run's route_*_NN.png> <tag> [crop x0,y0,x1,y1]
# route_*_NN.png come in order (dump label N, N+1). The effects of present count c show on the frame dumped as c+1, so with SNAP_LIGHT_AB=1 the EVEN label (first of a pair) is the Enhanced frame and the odd one the original. Writes analysis/lighting/compare/<tag>_<k>_orig.png, _enh.png and _sbs.png (left half original, right half Enhanced;
# the split line is drawn white), plus _crop_sbs.png for the optional crop box (full resolution pixels).
import pathlib, re, sys
from PIL import Image
import glob
files = sorted((pathlib.Path(f) for f in glob.glob(sys.argv[1])), key=lambda p: int(re.findall(r'_(\d+)\.png$', p.name)[-1])); tag = sys.argv[2]
crop = [int(v) for v in sys.argv[3].split(',')] if len(sys.argv) > 3 else None
out = pathlib.Path(__file__).resolve().parents[2] / 'analysis' / 'lighting' / 'compare'; out.mkdir(parents=True, exist_ok=True)
for k in range(0, len(files) - 1, 2):
    b = Image.open(files[k]).convert('RGB'); a = Image.open(files[k + 1]).convert('RGB')   # a original, b Enhanced
    n = k // 2
    a.save(out / f'{tag}_{n}_orig.png'); b.save(out / f'{tag}_{n}_enh.png')
    w, h = a.size; s = a.copy(); s.paste(b.crop((w // 2, 0, w, h)), (w // 2, 0))
    for y in range(h): s.putpixel((w // 2, y), (255, 255, 255))
    s.save(out / f'{tag}_{n}_sbs.png')
    if crop:
        x0, y0, x1, y1 = crop; ca = a.crop(crop); cb = b.crop(crop)
        m = Image.new('RGB', (ca.width * 2 + 8, ca.height), (255, 255, 255)); m.paste(ca, (0, 0)); m.paste(cb, (ca.width + 8, 0)); m.save(out / f'{tag}_{n}_crop_sbs.png')
    print(files[k].name, files[k + 1].name, '->', f'{tag}_{n}')
