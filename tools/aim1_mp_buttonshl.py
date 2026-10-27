"""A.I.M.: relabel NETWORK as MULTIPLAYER in the main menu's hover sheet.

SUF_MAINMENU_BUTSHL.dds: 10 labels x 10 frames (180x50) of the highlighted
button, all settled labels. Each frame is rebuilt like aim1_mp_button.py's
settled row: the new label is composed from glyphs of the same frame and
painted over NETWORK's text-free background (NEW GAME / CONTINUE share it).

  aim1_mp_buttonshl.py <SUF_MAINMENU_BUTSHL.dds> <out.dds> [preview.png]
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from PIL import Image
import aim1_mp_button as B

ROWS = 10
SAME_BG = (2, 0, 5, 4)      # NETWORK, NEW GAME, CONTINUE, EXIT share the background
GRID = 13                   # two periods of the background grid, in pixels


def background(px, row):
    """NETWORK's text-free background: from a column without text there, else the same
    grid pixel 13*k pixels to the side. The alpha (button shape) stays NETWORK's."""
    W, H = B.W, B.H
    masks = {c: B.text_mask(px, c, row, 150) for c in SAME_BG}

    def free(c, x, y):
        return not any(masks[c][yy][xx] for yy in range(max(0, y - 1), min(H, y + 2))
                       for xx in range(max(0, x - 1), min(W, x + 2)))

    out = [[None] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            a = px[B.NET * W + x, row * H + y][3]
            p = None
            for k in range(0, 8):
                for xs in ((x,) if k == 0 else (x - GRID * k, x + GRID * k)):
                    if not 0 <= xs < W:
                        continue
                    for c in SAME_BG:
                        q = px[c * W + xs, row * H + y]
                        if q[3] and free(c, xs, y):
                            p = q
                            break
                    if p:
                        break
                if p:
                    break
            p = p or px[B.NET * W + x, row * H + y]
            out[y][x] = (p[0], p[1], p[2], a)
    return out


def main():
    src = Image.open(sys.argv[1]).convert('RGBA')
    px = src.load()
    dst = src.copy()
    dp = dst.load()
    W, H, NET = B.W, B.H, B.NET
    for row in range(ROWS):
        # this frame's text brightness
        lums = sorted(B.lum(px[NET * W + x, row * H + y]) for y in range(H) for x in range(W)
                      if px[NET * W + x, row * H + y][3])
        B.HI = lums[int(len(lums) * 0.97)]
        B.LO = lums[int(len(lums) * 0.5)]
        B.CLEAN = row
        cov = B.compose(px, 'MULTIPLAYER')
        bg = background(px, row)
        net = [[B.cover_of(px, NET, row, x, y) for x in range(W)] for y in range(H)]
        solid = [px[NET * W + x, row * H + y] for y in range(H) for x in range(W) if net[y][x] > 0.9]
        tc = tuple(sum(p[i] for p in solid) / len(solid) for i in range(4))
        for y in range(H):
            for x in range(W):
                b = bg[y][x]
                c = cov[y][x] if b[3] else 0.0
                dp[NET * W + x, row * H + y] = tuple(int(b[k] + (tc[k] - b[k]) * c + 0.5) for k in range(3)) + (b[3],)
    from dxt import patch_dxt3
    patch_dxt3(sys.argv[1], dst, sys.argv[2], NET * W, NET * W + W)   # other labels keep their original blocks
    if len(sys.argv) > 3:
        orig = src.crop((NET * W, 0, NET * W + W, H * ROWS))
        new = dst.crop((NET * W, 0, NET * W + W, H * ROWS))
        pv = Image.new('RGBA', (W * 2, H * ROWS), (255, 0, 255, 255))
        pv.alpha_composite(orig, (0, 0))
        pv.alpha_composite(new, (W, 0))
        pv.resize((W * 4, H * ROWS * 2)).save(sys.argv[3])
    print('wrote', sys.argv[2])


if __name__ == '__main__':
    main()
