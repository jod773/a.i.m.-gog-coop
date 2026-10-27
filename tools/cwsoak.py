"""Long co-op soak for Clan Wars: cwgame (host) + cwgame2 (guest) must already be in a session
(cw2.py start). Both players fly, teleport around the sector, enter bases, accept missions and
leave, in random order. Stops at the first crash / hang and reports it.

  cwsoak.py <minutes>
"""
import os, sys, time, random
sys.path.insert(0, os.path.dirname(__file__))
import h
from PIL import Image

DEV = h.DEV
G = {'host': os.path.join(DEV, 'cwgame'), 'guest': os.path.join(DEV, 'cwgame2')}
BUILDINGS = ['B_L5_BASE1', 'B_L5_BASE2', 'B_L5_BASE3', 'B_L5_BASE4', 'B_L5_BASE5', 'B_L5_FEEDSTOCK',
             'B_L5_PLANT_RECYCLE', 'B_L5_PLANT_RM', 'B_L5_MINE1', 'B_L5_PLANT_PF', 'B_L5_KA']
SCR = h.SCRATCH
pos = {k: 0 for k in G}


def log_new(who):
    p = os.path.join(G[who], h.log_of(G[who]))
    with open(p, encoding='cp1251', errors='replace') as f:
        f.seek(pos[who])
        s = f.read()
        pos[who] = f.tell()
    return s


def alive(who):
    import subprocess
    pids = h.test_pids(G[who])
    if not pids:
        return 'gone'
    r = subprocess.run(['powershell', '-NoProfile', '-Command', '(Get-Process -Id %d).Responding' % pids[0]],
                       capture_output=True, text=True)
    return 'ok' if 'True' in r.stdout else 'hung'


def check():
    bad = []
    for who in G:
        s = log_new(who)
        for line in s.splitlines():
            if 'CRASH' in line or 'error' in line.lower():
                bad.append('%s: %s' % (who, line))
        st = alive(who)
        if st == 'hung':
            time.sleep(10)
            st = alive(who)
        if st != 'ok':
            bad.append('%s: process %s' % (who, st))
            if st == 'hung':
                h.send(G[who], ['stack'], timeout=5)
                time.sleep(1)
                bad.append(log_new(who))
    return bad


def find_link(png):
    """lowest orange text line in the base dialog's text panel (the mission's accept link)"""
    im = Image.open(png).convert('RGB')
    s = 1920 / im.width
    px = im.load()
    rows = []
    for y in range(int(280 / s), int(780 / s)):
        n, x0 = 0, None
        for x in range(int(340 / s), int(840 / s)):
            r, g, b = px[x, y]
            if r > 125 and r - b > 55 and r - g > 45:
                n += 1
                if x0 is None:
                    x0 = x
        if n >= 2:
            rows.append((y, x0))
    # group rows into text lines
    lines = []
    for y, x0 in rows:
        if lines and y - lines[-1][-1][0] <= 2:
            lines[-1].append((y, x0))
        else:
            lines.append([(y, x0)])
    if not lines:
        return None
    pick = lines[-1]
    if len(lines) >= 2 and (lines[-1][0][0] - lines[-2][0][0]) * s < 40:
        pick = lines[-2]                  # "Accept" above "Decline"
    y = pick[len(pick) // 2][0]
    return (int((min(x for _, x in pick) + 15) * s), int(y * s))


def base_visit(who):
    g = G[who]
    b = random.choice(BUILDINGS)
    h.send(g, ['enter ' + b, 'sleep 4000', 'click 960 296', 'sleep 1500'])
    h.shot(g, 'soak_' + who, 1.0)
    link = find_link(os.path.join(SCR, 'soak_' + who + '.png'))
    acts = []
    if link:
        acts = ['click %d %d' % link, 'sleep 2500']
    h.send(g, acts + ['click 1063 205', 'sleep 9000'])
    return '%s visited %s%s' % (who, b, ' (accepted a mission)' if link else '')


def fly(who):
    g = G[who]
    cmds = []
    if random.random() < 0.5:
        cmds += ['tp %s %d %d %d' % (random.choice(BUILDINGS), random.randint(-600, 600), random.randint(-600, 600),
                                     random.randint(80, 250))]
    for _ in range(random.randint(2, 5)):
        k = random.choice([0x11, 0x11, 0x1e, 0x20, 0x1f])   # DirectInput scan codes: W W A D S
        cmds += ['hold %d %d' % (k, random.randint(500, 3000))]
        if random.random() < 0.4:
            cmds += ['click 960 540']
    cmds += ['npcs']
    h.send(g, cmds, timeout=60)
    return '%s flew' % who


def main():
    minutes = float(sys.argv[1])
    if len(sys.argv) > 2:                 # solo <who>: base visits only, one instance, no session
        for k in list(G):
            if k != sys.argv[2]:
                del G[k]
    for who in G:
        log_new(who)
    t0 = time.time()
    step = 0
    while time.time() - t0 < minutes * 60:
        step += 1
        who = random.choice(list(G))
        what = base_visit(who) if len(G) == 1 or random.random() < 0.35 else fly(who)
        bad = check()
        print('[%4.0fs] %d %s' % (time.time() - t0, step, what), flush=True)
        if bad:
            print('PROBLEM:')
            for b in bad:
                print('  ', b)
            for w in G:
                h.shot(G[w], 'soak_fail_' + w, 0.5)
            return 1
    print('soak finished without problems: %d steps' % step)
    return 0


if __name__ == '__main__':
    sys.exit(main())
