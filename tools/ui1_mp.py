"""Add the main-menu "Multiplayer" button sprites to a TEX_INT_UI1 atlas.

The three states of the EXIT button (plain, orange/selected, blue/hover) are
copied to free atlas space and relabelled. The mod's menu button renderer
uses these origins (keep in sync with src/aim2/lobby.c):
  plain (450, 746)   selected (362, 555)   hover (540, 555)

  ui1_mp.py <in.tga> <out.tga> <label> [font size]
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import ui_text as U

# (source origin, destination origin, label box relative to the origin, style, bold)
STATES = [
    ((184, 522), (450, 746), (2, 0, 176, 31), 'flat', False),
    ((362, 522), (362, 555), (4, 0, 174, 31), 'flat', False),
    ((540, 522), (540, 555), (3, 0, 174, 31), 'glow', True),
]
W, H = 178, 33


def states_at(dests):
    """STATES with other destination origins (plain, selected, hover)"""
    return [(s, d, box, style, bold) for (s, _, box, style, bold), d in zip(STATES, dests)]


def add(im, label, size=None, states=None):
    states = states or STATES
    for (sx, sy), (dx, dy), (bx0, by0, bx1, by1), style, bold in states:
        im.paste(im.crop((sx, sy, sx + W, sy + H)), (dx, dy))
    for (sx, sy), (dx, dy), (bx0, by0, bx1, by1), style, bold in states:
        box = (dx + bx0, dy + by0, dx + bx1, dy + by1)
        if size:
            U.SIZE[box] = size
        U.process(im.copy(), box, style, label, bold, None, measure_only=True)
        # use the caps height of the existing menu rows for a consistent size
        src = (sx + bx0, sy + by0, sx + bx1, sy + by1)
        U.process(im.copy(), src, style, 'X', bold, None, measure_only=True)
        U.process(im, box, style, label, bold, None)


def main():
    im, header = U.read_tga(sys.argv[1])
    add(im, sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else None)
    U.write_tga(sys.argv[2], im, header)
    print('wrote', sys.argv[2])


if __name__ == '__main__':
    main()
