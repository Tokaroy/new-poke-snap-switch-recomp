# Joins the same-moment Off/On frames of a run_lens_compare.ps1 run into strips: python -I strip_lens.py <glob of the run's route_*_NN.png> <tag> [crop x0,y0,x1,y1]
# Frames come in pairs (dump label N, N+1). The effects of present count c show on the frame dumped as c+1, so with SNAP_LIGHT_AB=1 the EVEN label (first of a pair)
# is the effect-on frame and the odd one the original. Writes analysis/lens/<tag>_<k>_off.png, _on.png, _strip.png (Off left, On right, labelled, scaled to 2560 wide)
# and, with a crop box (full resolution pixels), _crop_strip.png. <tag>_all.png stacks every strip. Our own images only; analysis/lens/ is local.
import pathlib, re, sys, glob
from PIL import Image, ImageDraw

files = sorted((pathlib.Path(f) for f in glob.glob(sys.argv[1])), key=lambda p: int(re.findall(r'_(\d+)\.png$', p.name)[-1]))
tag = sys.argv[2]
crop = [int(v) for v in sys.argv[3].split(',')] if len(sys.argv) > 3 else None
out = pathlib.Path(__file__).resolve().parents[2] / 'analysis' / 'lens'
out.mkdir(parents=True, exist_ok=True)


def label(im, text):
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 8 * len(text) + 12, 22), fill=(0, 0, 0))
    d.text((6, 5), text, fill=(255, 255, 255))
    return im


def join(a, b, width):
    w, h = a.size
    s = width / (2 * w + 8)
    a2 = label(a.resize((int(w * s), int(h * s)), Image.LANCZOS), 'Off')
    b2 = label(b.resize((int(w * s), int(h * s)), Image.LANCZOS), 'On')
    m = Image.new('RGB', (a2.width * 2 + 8, a2.height), (255, 255, 255))
    m.paste(a2, (0, 0))
    m.paste(b2, (a2.width + 8, 0))
    return m


strips = []
for k in range(0, len(files) - 1, 2):
    on = Image.open(files[k]).convert('RGB')
    off = Image.open(files[k + 1]).convert('RGB')
    n = k // 2
    off.save(out / f'{tag}_{n}_off.png')
    on.save(out / f'{tag}_{n}_on.png')
    s = join(off, on, 2560)
    s.save(out / f'{tag}_{n}_strip.png')
    strips.append(s)
    if crop:
        join(off.crop(crop), on.crop(crop), 2560).save(out / f'{tag}_{n}_crop_strip.png')
    print(files[k].name, files[k + 1].name, '->', f'{tag}_{n}')
if strips:
    allim = Image.new('RGB', (strips[0].width, sum(s.height for s in strips) + 8 * (len(strips) - 1)), (255, 255, 255))
    y = 0
    for s in strips:
        allim.paste(s, (0, y))
        y += s.height + 8
    allim.save(out / f'{tag}_all.png')
