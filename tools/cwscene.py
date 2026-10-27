"""Clan Wars graphics test scene (two co-op instances from cw2.py start): the guest's glider is
placed in front of the host's camera.

  cwscene.py ahead [dist] [up]     put the guest <dist> ahead of the host (forward = matrix row 2)
  cwscene.py host x y z            move the host
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import h, cw2


def pos(game):
    n = cw2.log_len(game)
    h.send(game, ['pos'])
    time.sleep(0.5)
    txt = open(os.path.join(game, h.log_of(game)), encoding='cp1251', errors='replace').read()[n:]
    m = re.search(r'position ([-\d.]+) ([-\d.]+) ([-\d.]+) .*r2 ([-\d.]+) ([-\d.]+) ([-\d.]+)', txt)
    return [float(v) for v in m.groups()] if m else None


def main():
    c = sys.argv[1]
    if c == 'ahead':
        d = float(sys.argv[2]) if len(sys.argv) > 2 else 150
        up = float(sys.argv[3]) if len(sys.argv) > 3 else 15
        p = pos(cw2.G1)
        x, y, z = p[0] + p[3] * d, p[1] + p[4] * d, p[2] + p[5] * d + up
        h.send(cw2.G2, ['goto %.0f %.0f %.0f' % (x, y, z)])
        print('host', p[:3], 'forward', p[3:], '-> guest', round(x), round(y), round(z))
    elif c == 'host':
        h.send(cw2.G1, ['goto ' + ' '.join(sys.argv[2:5])])


if __name__ == '__main__':
    main()
