import sys
from PIL import Image
b = open(sys.argv[1], 'rb').read()
hdr = int(sys.argv[3], 0) if len(sys.argv) > 3 else 76
w = h = 1024
px = b[hdr:hdr + w * h * 2]
out = bytearray(w * h * 4)
for i in range(w * h):
    v = px[2 * i] | (px[2 * i + 1] << 8)
    a = (v >> 12) & 15; r = (v >> 8) & 15; g = (v >> 4) & 15; bl = v & 15
    out[4 * i:4 * i + 4] = bytes((r * 17, g * 17, bl * 17, a * 17))
im = Image.frombytes('RGBA', (w, h), bytes(out))
bg = Image.new('RGBA', im.size, (40, 40, 40, 255))
bg.alpha_composite(im)
bg.convert('RGB').save(sys.argv[2])
print('ok', len(b) - hdr - w * h * 2)
