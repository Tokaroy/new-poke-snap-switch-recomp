# thumbs.py <shotBase> <out.png> [cols]: grid of one frame per pair (k even), labelled k
import sys
from PIL import Image, ImageDraw
import glob
base, out = sys.argv[1], sys.argv[2]; cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
fs = sorted(glob.glob(base + '_[0-9][0-9].png'))[0::2]
W, H = 384, 216
S = Image.new('RGB', (cols * W, ((len(fs) + cols - 1) // cols) * H)); d = ImageDraw.Draw(S)
for i, f in enumerate(fs):
    k = int(f[-6:-4]); im = Image.open(f).convert('RGB').resize((W, H), Image.BILINEAR)
    x, y = (i % cols) * W, (i // cols) * H; S.paste(im, (x, y)); d.rectangle((x, y, x + 34, y + 16), fill='black'); d.text((x + 3, y + 2), 'k%02d' % k, fill='yellow')
S.save(out); print(S.size)
