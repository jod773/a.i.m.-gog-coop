"""A.I.M.: dialog captions for the mod's list dialogs.

SUF_CAPTIONS.dds (160x80, DXT3) holds the list dialog's titles in rows of 24 px, chosen by
the dialog type: LOAD GAME, SAVE GAME, NET GAME. The texture grows to 160x128 with two more
rows (src/aim1/lobby.c picks them): 3 MULTIPLAYER, 4 SETTINGS. The new titles use Windows'
Bahnschrift (bold condensed) in the captions' single colour, at their cap height.

  aim1_captions.py <SUF_CAPTIONS.dds> <out.dds> [preview.png]
"""
import struct, sys, os
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.dirname(__file__))
from dxt import _block

W, ROW, H = 160, 24, 128
COLOR = (206, 215, 231)
TITLES = ['MULTIPLAYER', 'SETTINGS']
CAP_TOP, CAP_H = 3, 18          # the original letters fill y 3..20 of their row


def title(text):
    s = 4
    font = ImageFont.truetype(r'C:\Windows\Fonts\bahnschrift.ttf', CAP_H * s * 1.4)
    try:
        font.set_variation_by_name('Bold Condensed')
    except Exception:
        pass
    x0, y0, x1, y1 = font.getbbox(text)
    k = CAP_H * s / (y1 - y0)                      # exact cap height
    font = ImageFont.truetype(r'C:\Windows\Fonts\bahnschrift.ttf', CAP_H * s * 1.4 * k)
    try:
        font.set_variation_by_name('Bold Condensed')
    except Exception:
        pass
    x0, y0, x1, y1 = font.getbbox(text)
    tw = x1 - x0
    big = Image.new('L', (W * s, ROW * s), 0)
    d = ImageDraw.Draw(big)
    d.text(((W * s - tw) / 2 - x0, CAP_TOP * s - y0), text, fill=255, font=font)
    mask = big.resize((W, ROW), Image.LANCZOS)
    out = Image.new('RGBA', (W, ROW), COLOR + (0,))
    out.putalpha(mask)
    return out


def main():
    src = open(sys.argv[1], 'rb').read()
    assert src[84:88] == b'DXT3', src[84:88]
    orig = Image.open(sys.argv[1]).convert('RGBA')
    im = Image.new('RGBA', (W, H), COLOR + (0,))
    im.paste(orig.crop((0, 0, W, 3 * ROW)), (0, 0))
    for i, t in enumerate(TITLES):
        im.paste(title(t), (0, (3 + i) * ROW))
    hdr = bytearray(src[:128])
    struct.pack_into('<I', hdr, 12, H)
    struct.pack_into('<I', hdr, 20, (W // 4) * (H // 4) * 16)
    struct.pack_into('<I', hdr, 28, 0)             # no mipmaps
    px = im.load()
    body = bytearray()
    for by in range(H // 4):
        for bx in range(W // 4):
            body += _block([px[bx * 4 + i, by * 4 + j] for j in range(4) for i in range(4)])
    open(sys.argv[2], 'wb').write(bytes(hdr) + body)
    if len(sys.argv) > 3:
        bg = Image.new('RGBA', (W, H), (20, 20, 30, 255))
        bg.alpha_composite(Image.open(sys.argv[2]).convert('RGBA'))
        bg.resize((W * 4, H * 4), Image.NEAREST).save(sys.argv[3])
    print('wrote', sys.argv[2])


if __name__ == '__main__':
    main()
