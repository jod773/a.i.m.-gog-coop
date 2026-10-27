"""A.I.M.: relabel the main-menu NETWORK button as MULTIPLAYER.

SUF_MAINMENU_BUTTONS.dds holds 10 columns (labels) x 18 rows (animation
frames) of 180x50 sprites. Row 17 is the settled label; the other rows are
the "glitch" frames played on hover. The new label is composed from glyphs of
the settled labels, then each glitch frame is rebuilt by estimating, per row,
how the NETWORK frame maps the settled NETWORK label (source row + horizontal
affine from the text extents) and applying the same mapping to the new label.
The text-free background comes from NEW GAME / CONTINUE, which share
NETWORK's background.

  aim1_mp_button.py <SUF_MAINMENU_BUTTONS.dds> <out.dds> [preview.png]
"""
import sys
from PIL import Image

W, H, ROWS = 180, 50, 18
NET, NEWG, CONT = 2, 0, 5
CLEAN = 17

# glyph sources in the settled row: letter -> (column, x0, x1, dy)
GLYPHS = {
    'M': (0, 124, 141, 0), 'U': (5, 123, 138, 0), 'L': (1, 22, 34, 0), 'T': (5, 80, 93, 0),
    'I': (5, 96, 101, 0), 'P': (3, 55, 69, 0), 'A': (0, 106, 121, 0), 'Y': (7, 107, 123, 1),
    'E': (0, 44, 57, 0), 'R': (2, 119, 133, 0),
}
LO, HI = 92.0, 175.0          # luminance of background (grid lines reach ~82) / text in the settled row
GAP = 1                       # with 1 px spacing MULTIPLAYER fits unscaled (scaling broke the strokes)
PERIOD = 20                   # the button's grid repeats every 20 px horizontally
GLOBAL = (0, 1)               # frames mapped with a single affine (no per-row warp)


def lum(p):
    return (p[0] + p[1] + p[2]) / 3.0


def cover_of(px, col, row, x, y):
    p = px[col * W + x, row * H + y]
    if p[3] < 128:
        return 0.0
    v = (lum(p) - LO) / (HI - LO) * 1.25     # solid strokes fully opaque: no grid showing through
    return 0.0 if v < 0 else 1.0 if v > 1 else v


def text_mask(px, col, row, thr=110):
    """Text pixels: the glitch backgrounds stay below ~100 luminance, the text above ~110."""
    return [[lum(px[col * W + x, row * H + y]) >= thr for x in range(W)] for y in range(H)]


def compose(px, word):
    """Coverage map (H x W floats) of `word` set in the settled-label style."""
    parts = [GLYPHS[ch] for ch in word]
    natural = sum(x1 - x0 for _, x0, x1, _ in parts) + GAP * (len(parts) - 1)
    target = 160.0
    s = min(1.0, target / natural)
    out = [[0.0] * W for _ in range(H)]
    x = (W - natural * s) / 2.0
    for col, x0, x1, dy in parts:
        gw = (x1 - x0) * s
        for ox in range(int(x), int(x + gw) + 2):
            if not 0 <= ox < W:
                continue
            # area-weighted horizontal resample of the glyph columns
            a, b = (ox - x) / s + x0, (ox + 1 - x) / s + x0
            for y in range(H):
                sy = y - dy
                if not 0 <= sy < H:
                    continue
                acc = wsum = 0.0
                c = int(a)
                while c < b:
                    if x0 <= c < x1:
                        w = min(b, c + 1) - max(a, c)
                        acc += cover_of(px, col, CLEAN, c, sy) * w
                        wsum += w
                    c += 1
                if wsum:
                    out[y][ox] = max(out[y][ox], acc / (b - a))
        x += gw + GAP * s
    return out


def background(px, row):
    """Text-free background of the NETWORK sprite in `row`, plus a hole mask."""
    masks = {c: text_mask(px, c, row, 100) for c in (NET, NEWG, CONT)}
    bg = [[None] * W for _ in range(H)]

    def free(c, x, y):
        return not any(masks[c][yy][xx] for yy in range(max(0, y - 1), min(H, y + 2))
                       for xx in range(max(0, x - 1), min(W, x + 2)))
    for y in range(H):
        for x in range(W):
            # NETWORK's own pixel, else the same grid cell PERIOD px away in the same row
            for d in [0] + [s * k * PERIOD for k in range(1, 8) for s in (1, -1)]:
                if 0 <= x + d < W and free(NET, x + d, y):
                    bg[y][x] = px[NET * W + x + d, row * H + y]
                    break
            if bg[y][x] is not None:
                continue
            for c in (NEWG, CONT):
                near = any(masks[c][yy][xx] for yy in range(max(0, y - 1), min(H, y + 2))
                           for xx in range(max(0, x - 1), min(W, x + 2)))
                if not near:
                    bg[y][x] = px[c * W + x, row * H + y]
                    break
    # fill the remaining holes from the nearest known pixels in the row / column
    for _ in range(40):
        todo = [(x, y) for y in range(H) for x in range(W) if bg[y][x] is None]
        if not todo:
            break
        fills = []
        for x, y in todo:
            nb = [bg[yy][xx] for xx, yy in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))
                  if 0 <= xx < W and 0 <= yy < H and bg[yy][xx] is not None]
            if nb:
                fills.append((x, y, tuple(sum(p[i] for p in nb) // len(nb) for i in range(4))))
        for x, y, p in fills:
            bg[y][x] = p
    return bg


def extent(row):
    xs = [x for x, v in enumerate(row) if v]
    return (xs[0], xs[-1] + 1) if xs else None


def frame(px, row, clean_net, clean_new):
    """Rebuild the NETWORK sprite of `row` with the new label."""
    bg = background(px, row)
    # the glitch text is bright (~200); 150 keeps the background noise out
    # (the fading-in frames 14..16 are dimmer and have a clean background)
    tmask = text_mask(px, NET, row, 150 if row < 14 else 110)
    cov = [[1.0 if v else 0.0 for v in r] for r in tmask]
    cmask = text_mask(px, NET, CLEAN)
    cbin = [[1.0 if v else 0.0 for v in r] for r in cmask]
    out = [[bg[y][x] for x in range(W)] for y in range(H)]
    for y in range(H):
        te = extent(tmask[y])
        if not te:
            continue
        # text colour of this row: mean of the solid text pixels
        solid = sorted((px[NET * W + x, row * H + y] for x in range(W) if tmask[y][x]), key=lum)
        solid = solid[len(solid) // 3:]
        tc = tuple(sum(p[i] for p in solid) / len(solid) for i in range(4))
        best = None
        if row in GLOBAL:
            # undistorted (only blurred / squashed) frames: one mapping for the whole sprite
            ty = [yy for yy in range(H) if extent(tmask[yy])]
            cy = [yy for yy in range(H) if extent(cmask[yy])]
            sy = cy[0] + int((y - ty[0]) * (cy[-1] - cy[0]) / float(max(1, ty[-1] - ty[0])) + 0.5)
            tx = [extent(tmask[yy]) for yy in ty]
            cx = [extent(cmask[yy]) for yy in cy]
            te = (min(e[0] for e in tx), max(e[1] for e in tx))
            se = (min(e[0] for e in cx), max(e[1] for e in cx))
            best = (0, sy, (te[1] - te[0]) / float(se[1] - se[0]), se)
        for sy in range(H) if best is None else ():
            se = extent(cmask[sy])
            if not se:
                continue
            s = (te[1] - te[0]) / float(se[1] - se[0])
            err = 0.0
            for x in range(W):
                sx = int((x - te[0]) / s + se[0])
                pv = cbin[sy][sx] if 0 <= sx < W else 0.0
                err += abs(pv - cov[y][x])
            if best is None or err < best[0]:
                best = (err, sy, s, se)
        _, sy, s, se = best
        for x in range(W):
            sx = (x - te[0]) / s + se[0]
            i = int(sx)
            if not 0 <= i < W - 1:
                continue
            f = sx - i
            c = clean_new[sy][i] * (1 - f) + clean_new[sy][i + 1] * f
            if c > 0.02:
                # keep the frame's text noise: colour of the nearest original text pixel in the row
                for d in range(W):
                    if x - d >= 0 and tmask[y][x - d]:
                        tc = px[NET * W + x - d, row * H + y]
                        break
                    if x + d < W and tmask[y][x + d]:
                        tc = px[NET * W + x + d, row * H + y]
                        break
                b = out[y][x]
                out[y][x] = tuple(int(b[k] + (tc[k] - b[k]) * c + 0.5) for k in range(4))
    return out


def main():
    src = Image.open(sys.argv[1]).convert('RGBA')
    px = src.load()
    clean_net = [[cover_of(px, NET, CLEAN, x, y) for x in range(W)] for y in range(H)]
    clean_new = compose(px, 'MULTIPLAYER')
    dst = src.copy()
    dp = dst.load()
    for row in range(ROWS):
        if row == CLEAN:
            bg = background(px, row)
            # settled text colour: mean of the solid NETWORK pixels
            solid = [px[NET * W + x, row * H + y] for y in range(H) for x in range(W)
                     if clean_net[y][x] > 0.9]
            tc = tuple(sum(p[i] for p in solid) / len(solid) for i in range(4))
            img = [[tuple(int(bg[y][x][k] + (tc[k] - bg[y][x][k]) * clean_new[y][x] + 0.5)
                          for k in range(4)) for x in range(W)] for y in range(H)]
        else:
            img = frame(px, row, clean_net, clean_new)
        for y in range(H):
            for x in range(W):
                dp[NET * W + x, row * H + y] = img[y][x]
        print('row', row, file=sys.stderr)
    from dxt import patch_dxt3
    patch_dxt3(sys.argv[1], dst, sys.argv[2], NET * W, NET * W + W)   # other labels keep their original blocks
    if len(sys.argv) > 3:
        col = dst.crop((NET * W, 0, NET * W + W, H * ROWS))
        orig = src.crop((NET * W, 0, NET * W + W, H * ROWS))
        pv = Image.new('RGBA', (W * 2, H * ROWS), (255, 0, 255, 255))
        pv.alpha_composite(orig, (0, 0))
        pv.alpha_composite(col, (W, 0))
        pv.resize((W * 4, H * ROWS * 2)).save(sys.argv[3])
    print('wrote', sys.argv[2])


if __name__ == '__main__':
    main()
