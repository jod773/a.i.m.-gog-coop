"""cmp_crop.py <a.png|tga> <b.png|tga> x0 y0 x1 y1 out.png [zoom] : side-by-side zoomed crops on magenta"""
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGBA')
b = Image.open(sys.argv[2]).convert('RGBA')
x0, y0, x1, y1 = map(int, sys.argv[3:7])
z = int(sys.argv[8]) if len(sys.argv) > 8 else 3
def prep(im):
    c = im.crop((x0, y0, x1, y1))
    bg = Image.new('RGBA', c.size, (60, 0, 60, 255))
    bg.alpha_composite(c)
    return bg.resize((c.width * z, c.height * z), Image.NEAREST)
A, B = prep(a), prep(b)
out = Image.new('RGB', (A.width, A.height * 2 + 4), (255, 255, 0))
out.paste(A, (0, 0))
out.paste(B, (0, A.height + 4))
out.save(sys.argv[7])
print(sys.argv[7], out.size)
