"""Clan Wars (GOG): add the main-menu "Multiplayer" sprites to DATA\\TM\\TEX_INT_UI1.TM.

The .TM is a 76-byte header followed by the 1024x1024 ARGB4444 image (then
data the menu does not use, kept as is). The sprites go to the atlas
origins src/aim2/lobby.c expects (the same as ui1_mp.py uses).

  cw_mp_atlas.py <in.tm> <out.tm> [label] [preview.png]
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from PIL import Image
import ui1_mp

HDR, W, H = 76, 1024, 1024


def decode(b):
    px = b[HDR:HDR + W * H * 2]
    out = bytearray(W * H * 4)
    for i in range(W * H):
        v = px[2 * i] | (px[2 * i + 1] << 8)
        out[4 * i:4 * i + 4] = bytes((((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 15) * 17))
    return Image.frombytes('RGBA', (W, H), bytes(out))


def encode(im, b, region):
    """re-encode only the changed region (keeps the original 4-bit values elsewhere)"""
    b = bytearray(b)
    px = im.load()
    x0, y0, x1, y1 = region
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, bl, a = px[x, y]
            v = ((a * 15 + 127) // 255) << 12 | ((r * 15 + 127) // 255) << 8 | ((g * 15 + 127) // 255) << 4 | ((bl * 15 + 127) // 255)
            o = HDR + 2 * (y * W + x)
            b[o] = v & 255
            b[o + 1] = v >> 8
    return bytes(b)


# "Mod settings" (plain, selected, hover); keep in sync with src/aim2/lobby.c
MODSET = [(444, 0), (286, 98), (466, 98)]


def main():
    src = open(sys.argv[1], 'rb').read()
    label = sys.argv[3] if len(sys.argv) > 3 else 'MULTIPLAYER'
    im = decode(src)
    ui1_mp.add(im, label)
    modset = ui1_mp.states_at(MODSET)
    ui1_mp.add(im, 'MOD SETTINGS', states=modset)
    out = src
    for (sx, sy), (dx, dy), box, style, bold in ui1_mp.STATES + modset:
        out = encode(im, out, (dx, dy, dx + ui1_mp.W, dy + ui1_mp.H))
    open(sys.argv[2], 'wb').write(out)
    if len(sys.argv) > 4:
        bg = Image.new('RGBA', im.size, (40, 40, 40, 255))
        bg.alpha_composite(decode(out))
        bg.crop((180, 350, 720, 800)).save(sys.argv[4])
    print('wrote', sys.argv[2])


if __name__ == '__main__':
    main()
