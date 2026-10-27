"""DXT3 re-encoding of part of a texture, keeping the original blocks elsewhere.

Pillow's DXT3 encoder leaves visible stripes on sharp text; this one tries
every pair of block colours as endpoints and keeps the best.

  patch_dxt3(orig_dds, img, out_dds, x0, x1)   re-encode the blocks of columns x0..x1
"""
import struct


def _565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def _rgb(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _block(px):
    """px: 16 RGBA tuples -> 16 bytes of DXT3"""
    alpha = 0
    for i, p in enumerate(px):
        alpha |= (p[3] * 15 + 127) // 255 << (4 * i)
    cols = sorted(set(_565(p) for p in px))
    best = None
    for i in range(len(cols)):
        for j in range(i, len(cols)):
            a, b = max(cols[i], cols[j]), min(cols[i], cols[j])
            ca, cb = _rgb(a), _rgb(b)
            pal = [ca, cb, tuple((2 * ca[k] + cb[k]) // 3 for k in range(3)), tuple((ca[k] + 2 * cb[k]) // 3 for k in range(3))]
            err, idx = 0, 0
            for n, p in enumerate(px):
                w = p[3] / 255.0              # transparent pixels matter little
                e = [sum((p[k] - q[k]) ** 2 for k in range(3)) for q in pal]
                m = min(range(4), key=e.__getitem__)
                err += e[m] * (0.05 + w)
                idx |= m << (2 * n)
            if a == b:
                idx = 0
            if best is None or err < best[0]:
                best = (err, a, b, idx)
            if err == 0:
                break
    _, a, b, idx = best
    if a == b and a > 0:                      # keep 4-colour mode (c0 > c1)
        b = a - 1
    elif a == b:
        a = 1
    return struct.pack('<QHHI', alpha, a, b, idx)


def patch_dxt3(orig, img, out, x0, x1):
    data = bytearray(open(orig, 'rb').read())
    assert data[84:88] == b'DXT3', data[84:88]
    h, w = struct.unpack_from('<II', data, 12)
    bw = (w + 3) // 4
    px = img.load()
    for by in range(h // 4):
        for bx in range(x0 // 4, (x1 + 3) // 4):
            blk = [px[bx * 4 + i, by * 4 + j] for j in range(4) for i in range(4)]
            o = 128 + (by * bw + bx) * 16
            data[o:o + 16] = _block(blk)
    open(out, 'wb').write(data)
