"""A.I.M. 2 .TM textures: tm.py info|png <file.tm> [out.png]"""
import sys, struct
from PIL import Image


def info(b):
    w, h, fmt, a, c, mips = struct.unpack_from('<6I', b, 0)
    return w, h, fmt, a, c, mips


def first_nonzero(b, start):
    for i in range(start, len(b)):
        if b[i]:
            return i
    return -1


def to_image(b):
    w, h, fmt, a, c, mips = info(b)
    # find header size: data size for level 0 ARGB is w*h*4; assume the image
    # occupies the tail of the file in mip order starting after the header
    lvl0 = w * h * 4
    total = sum(max(1, w >> k) * max(1, h >> k) * 4 for k in range(mips))
    hdr = len(b) - total
    return hdr, total


if __name__ == '__main__':
    b = open(sys.argv[2], 'rb').read()
    w, h, fmt, a, c, mips = info(b)
    print('size', len(b), 'w', w, 'h', h, 'fmt', fmt, 'a', a, 'c', c, 'mips', mips)
    print('first nonzero after 0x18:', hex(first_nonzero(b, 0x18)))
    for m in range(1, 12):
        tot = sum(max(1, w >> k) * max(1, h >> k) * 4 for k in range(m))
        print(' mips', m, 'argb total', tot, 'hdr would be', len(b) - tot)
    if sys.argv[1] == 'png':
        hdr = int(sys.argv[4], 0) if len(sys.argv) > 4 else len(b) - w * h * 4
        px = b[hdr:hdr + w * h * 4]
        im = Image.frombytes('RGBA', (w, h), px, 'raw', 'BGRA')
        im.save(sys.argv[3])
        print('saved', sys.argv[3], 'hdr', hdr)
