"""Redraw a text label inside a UI atlas region: detect the text pixels against a per-row background
estimate, erase them by interpolating the background across the text box, then draw the new label in
a matching style (used by ui1_mp.py for the "Multiplayer" menu buttons)."""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FONT_R = r'C:\Windows\Fonts\tahoma.ttf'
FONT_B = r'C:\Windows\Fonts\tahomabd.ttf'


def read_tga(path):
    """Uncompressed 32-bit TGA -> RGBA image (Pillow drops alpha when the
    descriptor declares 0 alpha bits, as this file does). Returns (img, header)."""
    import struct
    b = open(path, 'rb').read()
    idl = b[0]
    w, h = struct.unpack_from('<HH', b, 12)
    assert b[2] == 2 and b[16] == 32, 'expected uncompressed 32-bit TGA'
    desc = b[17]
    p = 18 + idl
    im = Image.frombytes('RGBA', (w, h), b[p:p + w * h * 4], 'raw', 'BGRA')
    if not (desc & 0x20):
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
    return im, b[:18 + idl]


def write_tga(path, im, header):
    desc = header[17]
    if not (desc & 0x20):
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
    data = im.tobytes('raw', 'BGRA')
    with open(path, 'wb') as f:
        f.write(header)
        f.write(data)



# style: 'flat' (crisp, sampled colour), 'glow' (white + blurred halo),
#        'plate' (navy on white), 'dim' (sampled colour, used on dark/disabled)
SIZE = {}   # (box) -> explicit font size in px


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(4))


def dist(a, b):
    return max(abs(a[i] - b[i]) for i in range(4))


CAPS = {}   # (style, width-bucket) -> list of detected cap heights (first pass)


def group_key(box, style, bold):
    return (style, bold, box[3] - box[1])


def process(im, box, style, label, bold, dbg, measure_only=False):
    x0, y0, x1, y1 = box
    px = im.load()
    pad = 3
    ix0, ix1 = x0 + pad, x1 - pad

    from collections import Counter

    def mode(cs):
        return Counter(cs).most_common(1)[0][0]
    # background per row: most frequent colour of the interior row (these are
    # clean rendered graphics; even text rows are mostly background)
    bgL, bgR = {}, {}
    for y in range(y0, y1):
        m = mode([px[x, y] for x in range(ix0, ix1)])
        bgL[y] = bgR[y] = m
    # text pixels (plain labels sit on a faint grid, glow text has a halo:
    # use higher thresholds so only the glyph cores count)
    plain = bgL[(y0 + y1) // 2][3] < 30
    thr = 110 if plain else 90 if style == 'glow' else 40
    tx0, ty0, tx1, ty1 = 10 ** 9, 10 ** 9, -1, -1
    col = {}
    for y in range(y0 + 2, y1 - 2):
        for x in range(ix0 + 1, ix1 - 1):
            t = (x - ix0) / max(1, ix1 - ix0)
            bg = lerp(bgL[y], bgR[y], t)
            if dist(px[x, y], bg) > thr:
                tx0 = min(tx0, x); tx1 = max(tx1, x)
                ty0 = min(ty0, y); ty1 = max(ty1, y)
                c = px[x, y]
                col[c] = col.get(c, 0) + 1
    if tx1 < 0:
        print('no text found in', box, label)
        return
    if measure_only:
        CAPS.setdefault(group_key(box, style, bold), []).append(ty1 - ty0 + 1)
        return
    g = 3 if style == 'glow' else 1
    ex0, ex1 = max(ix0, tx0 - g), min(ix1, tx1 + g + 1)
    ey0, ey1 = max(y0 + 1, ty0 - g), min(y1 - 1, ty1 + g + 1)
    # text mask inside the text box (lower threshold than detection so the
    # glyph anti-aliasing is included), dilated by g
    mthr = 25 if not plain else 30
    raw = set()
    for y in range(ey0, ey1):
        for x in range(ex0, ex1):
            if dist(px[x, y], bgL[y]) > mthr:
                raw.add((x, y))
    mask = set()
    for (x, y) in raw:
        for dy in range(-g, g + 1):
            for dx in range(-g, g + 1):
                if ex0 <= x + dx < ex1 and ey0 <= y + dy < ey1:
                    mask.add((x + dx, y + dy))
    orig = {p: px[p] for p in mask}
    if label == 'FIELD DEACTIVATED':
        # sits on plain transparency at the atlas edge: just clear it
        for y in range(y0, y1 + 1):
            for x in range(ix0 - 2, ix1 + 2):
                px[x, y] = (255, 255, 255, 0)
    elif plain:
        # transparent background with a grid: interpolate vertically between
        # the nearest unmasked pixels, which keeps vertical grid lines
        for x in range(ex0, ex1):
            ys = [y for y in range(ey0, ey1) if (x, y) in mask]
            for y in ys:
                ya = y - 1
                while (x, ya) in mask: ya -= 1
                yb = y + 1
                while (x, yb) in mask: yb += 1
                a = px[x, ya] if (x, ya) not in orig else orig[(x, ya)]
                b = px[x, yb] if yb < im.height else a
                px[x, y] = lerp(a, b, (y - ya) / (yb - ya))
    else:
        for y in range(ey0, ey1):
            xs = [x for x in range(ex0, ex1) if (x, y) in mask]
            for x in xs:
                xa = x - 1
                while (xa, y) in mask: xa -= 1
                xb = x + 1
                while (xb, y) in mask: xb += 1
                a = px[xa, y] if xa >= ix0 - 1 else bgL[y]
                b = px[xb, y] if xb <= ix1 else bgL[y]
                px[x, y] = lerp(a, b, (x - xa) / (xb - xa))
    # text colour: the most saturated/brightest frequent text colour
    if style == 'plate':
        color = (31, 56, 84, 255)
    elif style == 'glow':
        color = (255, 255, 255, 255)
    else:
        best = sorted(col.items(), key=lambda kv: -(sum(kv[0][:3]) * (kv[0][3] / 255.0)) * min(kv[1], 6))
        color = best[0][0]
        color = (color[0], color[1], color[2], 255)
    caps = sorted(CAPS.get(group_key(box, style, bold), [ty1 - ty0 + 1]))
    cap = caps[len(caps) // 2] if len(caps) > 2 else caps[0]
    if style == 'glow':
        cap -= 1          # the measured box includes the brightest halo pixels
    font_path = FONT_B if bold else FONT_R
    if plain:
        color = (255, 255, 255, 235)
    size = SIZE.get(box) or max(8, int(round(cap / 0.727)))
    avail = (ix1 - ix0) - 6
    while True:
        f = ImageFont.truetype(font_path, size)
        bb = f.getbbox(label)
        if bb[2] - bb[0] <= avail or size <= 8:
            break
        size -= 1
    bb = f.getbbox(label)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    cx = (tx0 + tx1 + 1) / 2 if (tx1 - tx0) < (ix1 - ix0) * 0.9 else (ix0 + ix1) / 2
    cx = (ix0 + ix1) / 2 if abs(cx - (ix0 + ix1) / 2) < 6 else cx
    left = int(round(cx - tw / 2)) - bb[0]
    left = max(ix0 - bb[0], min(left, ix1 - tw - bb[0]))
    top = int(round((ty0 + ty1 + 1) / 2 - th / 2)) - bb[1]
    layer = Image.new('RGBA', im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    if style == 'glow':
        halo = Image.new('RGBA', im.size, (0, 0, 0, 0))
        ImageDraw.Draw(halo).text((left, top), label, font=f, fill=(255, 255, 255, 255))
        halo = halo.filter(ImageFilter.GaussianBlur(1.7))
        a = halo.getchannel('A').point(lambda v: min(255, int(v * 2.2)))
        halo.putalpha(a)
        layer.alpha_composite(halo)
    # regular-weight labels get a faint second pass to match the original's
    # slightly heavier hinting
    if not bold:
        d.text((left + 0.4, top), label, font=f, fill=color[:3] + (110,))
    d.text((left, top), label, font=f, fill=color)
    clip = Image.new('L', im.size, 0)
    ImageDraw.Draw(clip).rectangle((ix0 - 2, y0, ix1 + 2, y1 - 1), fill=255)
    layer.putalpha(Image.composite(layer.getchannel('A'), Image.new('L', im.size, 0), clip))
    im.alpha_composite(layer)
    if dbg is not None:
        dd = ImageDraw.Draw(dbg)
        dd.rectangle((x0, y0, x1 - 1, y1 - 1), outline=(255, 255, 0, 255))
        dd.rectangle((ex0, ey0, ex1 - 1, ey1 - 1), outline=(255, 0, 0, 255))
