"""crop.py <image> <x0> <y0> <x1> <y1> <out.png> [zoom] [--grid]"""
import sys
from PIL import Image, ImageDraw
im = Image.open(sys.argv[1]).convert('RGBA')
x0, y0, x1, y1 = map(int, sys.argv[2:6])
z = float(sys.argv[7]) if len(sys.argv) > 7 and not sys.argv[7].startswith('--') else 2
c = im.crop((x0, y0, x1, y1))
bg = Image.new('RGBA', c.size, (255, 0, 255, 255))
bg.alpha_composite(c)
c = bg.resize((int(c.width * z), int(c.height * z)), Image.NEAREST)
if '--grid' in sys.argv:
    d = ImageDraw.Draw(c)
    step = 10
    for gx in range((x0 // step + 1) * step, x1, step):
        X = int((gx - x0) * z)
        d.line([(X, 0), (X, 4 if gx % 50 else 10)], fill=(255, 255, 0, 255))
    for gy in range((y0 // step + 1) * step, y1, step):
        Y = int((gy - y0) * z)
        d.line([(0, Y), (4 if gy % 50 else 10, Y)], fill=(255, 255, 0, 255))
c.convert('RGB').save(sys.argv[6])
print(sys.argv[6], c.size)
