# quad.py <shotBase> <out> k1 [k2 k3 k4]: 2x2 of frames at 1/4 scale (960x540 each), grid every 480 frame px (120 view px), labels
import sys
from PIL import Image, ImageDraw
base, out, ks = sys.argv[1], sys.argv[2], [int(k) for k in sys.argv[3:]]
S = Image.new('RGB', (1920, 1080)); d = ImageDraw.Draw(S)
for i, k in enumerate(ks):
    im = Image.open('%s_%02d.png' % (base, k)).convert('RGB').resize((960, 540), Image.BILINEAR)
    x, y = (i % 2) * 960, (i // 2) * 540; S.paste(im, (x, y))
    for g in range(120, 960, 120): d.line((x + g, y, x + g, y + 540), fill=(255, 255, 0))
    for g in range(120, 540, 120): d.line((x, y + g, x + 960, y + g), fill=(255, 255, 0))
    d.rectangle((x, y, x + 40, y + 16), fill='black'); d.text((x + 3, y + 2), 'k%02d' % k, fill='yellow')
S.save(out)
