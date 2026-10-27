"""Find what holds a string in a running test instance: memfind, then ptrfind on each hit.

  memchase.py <game_dir> <text> [range_hex]
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import h

game, text = os.path.join(h.DEV, sys.argv[1]), sys.argv[2]
rng = sys.argv[3] if len(sys.argv) > 3 else '800'
log = os.path.join(game, h.log_of(game))


def run(cmds):
    n = len(open(log, encoding='cp1251', errors='replace').read())
    h.send(game, cmds + ['sleep 3000'], timeout=120)
    time.sleep(1)
    return open(log, encoding='cp1251', errors='replace').read()[n:]


out = run(['memfind ' + text])
hits = re.findall(r'memfind: ([0-9a-f]{8}) \((\w+)', out)
print(out)
for addr, kind in hits:
    print(run(['ptrfind %s %s' % (addr, rng)]))
