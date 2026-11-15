# sheet.py <list.json> <out.png>: list of {"species","orig","hd","grade","note","crop":[x,y,s] optional}
import sys, json
from PIL import Image, ImageDraw, ImageFont
L = json.load(open(sys.argv[1])); T = 480; cols = 2
try: font = ImageFont.truetype('arial.ttf', 22); small = ImageFont.truetype('arial.ttf', 16)
except Exception: font = small = ImageFont.load_default()
cw, ch = 2 * T + 12, T + 56
rows = (len(L) + cols - 1) // cols
S = Image.new('RGB', (cols * cw + 10, rows * ch + 40), (24, 24, 24)); d = ImageDraw.Draw(S)
d.text((10, 8), 'HD Pokemon in game, 4K (render scale 2), consecutive frames: left original, right HD pack (SNAP_MODS_AB)', fill='white', font=font)
for i, e in enumerate(L):
    x, y = 10 + (i % cols) * cw, 40 + (i // cols) * ch
    for j, k in enumerate(('orig', 'hd')):
        im = Image.open(e[k]).convert('RGB')
        if e.get('crop'):
            cx, cy, s = e['crop']; im = im.crop((cx, cy, cx + s, cy + s))
        im = im.resize((T, T), Image.LANCZOS if im.width > T else Image.NEAREST)
        S.paste(im, (x + j * (T + 4), y + 30))
    col = {'good': (120, 230, 120), 'too strong': (255, 140, 80), 'no visible change': (200, 200, 120), 'broken': (255, 80, 80)}.get(e['grade'], 'white')
    d.text((x, y + 4), '%s: %s' % (e['species'], e['grade']), fill=col, font=font)
    if e.get('note'): d.text((x + 2 * T + 4 - d.textlength(e['note'][:70], font=small), y + 8), e['note'][:70], fill=(200, 200, 200), font=small)
S.save(sys.argv[2]); print(S.size)
