"""Find opaque rectangles (buttons/plates) in a UI atlas and draw them numbered."""
import sys
from PIL import Image, ImageDraw, ImageFont
from collections import deque

src, out = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGBA')
W, H = im.size
a = im.getchannel('A').load()
seen = [[False] * W for _ in range(H)]
boxes = []
for y in range(H):
    for x in range(W):
        if seen[y][x] or a[x, y] < 40:
            continue
        q = deque([(x, y)])
        seen[y][x] = True
        x0 = x1 = x; y0 = y1 = y; n = 0
        while q:
            cx, cy = q.popleft(); n += 1
            x0 = min(x0, cx); x1 = max(x1, cx); y0 = min(y0, cy); y1 = max(y1, cy)
            for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                if 0 <= nx < W and 0 <= ny < H and not seen[ny][nx] and a[nx, ny] >= 40:
                    seen[ny][nx] = True
                    q.append((nx, ny))
        w, h = x1 - x0 + 1, y1 - y0 + 1
        if 40 <= w <= 400 and 12 <= h <= 60 and n > 0.6 * w * h:
            boxes.append((x0, y0, x1 + 1, y1 + 1))
boxes.sort(key=lambda b: (b[1] // 8, b[0]))
bg = Image.new('RGBA', im.size, (255, 0, 255, 255))
bg.alpha_composite(im)
d = ImageDraw.Draw(bg)
for i, b in enumerate(boxes):
    d.rectangle(b, outline=(255, 255, 0, 255))
    d.text((b[0] + 2, b[1] + 1), str(i), fill=(255, 0, 0, 255))
    print(i, b)
bg.convert('RGB').save(out)
