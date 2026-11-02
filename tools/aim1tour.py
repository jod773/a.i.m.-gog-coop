"""Two-player A.I.M. trip through the sectors: the host goes through each passage tunnel (the guest
follows with the host's world), then both play the new sector for a while (aim1soak's random base
visits and flying). aim1game (host) + aim1game2 (guest) must already be in a session
(coop1.py start/newhost/join).

  aim1tour.py [last sector (7)] [minutes per sector (2)]

Stops at the first crash / hang / missed world and reports it. Screenshots: tmp/shots/tour_*.png
"""
import os, sys, time, random
sys.path.insert(0, os.path.dirname(__file__))
import h, aim1soak
from PIL import Image

S = aim1soak
HOST, GUEST = S.G['host'], S.G['guest']
JUMP, FLY_OUT = (260, 744), (230, 875)
SECTOR_X, SECTOR_Y0, SECTOR_DY = 1625, 195, 19       # the jump page's sector list (1920x1080)


def log_len(g):
    with open(os.path.join(g, h.log_of(g)), encoding='cp1251', errors='replace') as f:
        return len(f.read())


def wait_log(g, text, since, timeout):
    t = time.time()
    while time.time() - t < timeout:
        with open(os.path.join(g, h.log_of(g)), encoding='cp1251', errors='replace') as f:
            if text in f.read()[since:]:
                return True
        time.sleep(0.5)
    return False


def find_transfer(png):
    """the jump page's "make the transfer" link: the lowest red text line in the text panel"""
    im = Image.open(png).convert('RGB')
    s = 1920 / im.width
    px = im.load()
    rows = []
    for y in range(int(140 / s), int(900 / s)):
        n, x0 = 0, None
        for x in range(int(480 / s), int(1420 / s)):
            r, g, b = px[x, y]
            # red (192, 98, 91), or lighter once the link was hovered / used (218, 158, 153)
            if r > 140 and r - g > 40 and r - b > 40 and abs(g - b) < 30:
                n += 1
                if x0 is None:
                    x0 = x
        if n >= 3:
            rows.append((y, x0))
    if not rows:
        return None
    y, x0 = rows[-1]
    return (int((x0 + 20) * s), int(y * s))


def sector():
    """the host's sector, from its buildings' names (B_L<n>_...)"""
    for b in S.location_buildings('host'):
        if b.startswith('B_L') and '_' in b[3:]:
            return int(b[3:b.index('_', 3)])
    return 0


def problems(what):
    bad = S.check()
    if bad:
        print('PROBLEM after %s:' % what)
        for b in bad:
            print('  ', b)
        for w in S.G:
            h.shot(S.G[w], 'tour_fail_' + w, 0.5)
    return bool(bad)


NAME = {'host': 'Host', 'guest': 'Guest'}


def log_text(who):
    with open(os.path.join(S.G[who], h.log_of(S.G[who])), encoding='cp1251', errors='replace') as f:
        return f.read()


def down_since(since):
    """the player who went down (NPCs, turrets) since the log offsets and is not back yet"""
    for who in S.G:
        t = log_text(who)[since[who]:]
        i = t.rfind('respawn: we are down')
        if i >= 0 and 'spectating over' not in t[i:]:
            return who
    return None


def revive(dead, since):
    """the teammate carries the dead player's mechmind into a base; both fly out again"""
    other = 'guest' if dead == 'host' else 'host'
    base = next((b for b in S.BUILDINGS if 'BASE' in b), S.BUILDINGS[0])
    if not wait_log(S.G[other], "%s's mechmind is lying at" % NAME[dead], since[other], 40):
        print('  %s is down and %s found no mechmind' % (dead, other))
        return False
    h.send(S.G[other], ['pickmind', 'sleep 2500'], timeout=60)
    h.send(S.G[other], ['enter ' + base, 'sleep 5000'], timeout=60)
    if dead == 'host':
        ok = wait_log(S.G['host'], 'back in our own world', since['host'], 20)
        h.send(S.G['host'], ['sleep 2000', 'click %d %d' % FLY_OUT, 'sleep 4000'], timeout=60)
        h.send(S.G['guest'], ['click %d %d' % FLY_OUT, 'sleep 4000'], timeout=60)
    else:
        h.send(S.G['host'], ['click %d %d' % FLY_OUT, 'sleep 4000'], timeout=60)
        ok = wait_log(S.G['guest'], 'respawn: back in', since['guest'], 90)
        h.send(S.G['guest'], ['sleep 3000', 'click %d %d' % FLY_OUT, 'sleep 4000'], timeout=60)
    print('  %s was down - %s' % (dead, 'brought back in ' + base if ok else 'NOT brought back'), flush=True)
    return ok


def quest_hash(who):
    since = log_len(S.G[who])
    h.send(S.G[who], ['qldump', 'sleep 300'], timeout=30)
    t = log_text(who)[since:]
    i = t.rfind(', set ')                 # order-independent: tasks, journal and flag names
    return t[i + 6:i + 14] if i >= 0 else None


def quests_same():
    """both players' quest logs (tasks, journal, flags, timers) are the same - give the sync a few s"""
    for _ in range(6):
        a, b = quest_hash('host'), quest_hash('guest')
        if a and a == b:
            return True
        time.sleep(2)
    print('  quest logs differ: host %s, guest %s' % (a, b))
    for who in S.G:
        h.send(S.G[who], ['qldump flags', 'sleep 300'], timeout=30)
    return False


def play(minutes):
    S.BUILDINGS[:] = S.location_buildings('host') or S.BUILDINGS
    t0, step = time.time(), 0
    while time.time() - t0 < minutes * 60:
        step += 1
        who = random.choice(list(S.G))
        since = {w: log_len(S.G[w]) for w in S.G}
        what = S.base_visit(who) if random.random() < 0.35 else S.fly(who)
        print('  [%4.0fs] %s' % (time.time() - t0, what), flush=True)
        if problems(what) or 'STILL IN THE BASE' in what:
            return False
        dead = down_since(since)
        if dead and (not revive(dead, since) or problems('the revival of ' + dead)):
            return False
    return True


def travel(cur, to):
    for who in S.G:
        if not S.leave_base(who):
            print('%s is stuck in a base' % who)
            return False
    h.send(HOST, ['qlflag SECTOR%d.ACCESS' % to, 'unlock', 'sleep 300', 'tp B_L%d_TUNNEL' % cur, 'sleep 2500',
                  'enter B_L%d_TUNNEL' % cur, 'sleep 5000', 'click %d %d' % JUMP, 'sleep 2500',
                  # the list holds every sector we have access to, the last one selected: pick ours
                  'click %d %d' % (SECTOR_X, SECTOR_Y0 + SECTOR_DY * (to - 1)), 'sleep 2000'], timeout=60)
    h.shot(HOST, 'tour_jump%d' % to, 1.0)
    link = find_transfer(os.path.join(h.SCRATCH, 'tour_jump%d.png' % to))
    if not link:
        print('no transfer link on the jump page (tour_jump%d.png)' % to)
        return False
    since = log_len(GUEST)
    h.send(HOST, ['click %d %d' % link, 'sleep 14000'], timeout=60)
    h.shot(HOST, 'tour_arrive%d' % to, 0.5)
    h.send(HOST, ['click %d %d' % FLY_OUT, 'sleep 3000'], timeout=60)
    if not wait_log(GUEST, 'world received', since, 90):
        print('the guest got no world after the host went to sector %d' % to)
        return False
    time.sleep(8)
    h.shot(HOST, 'tour_s%d_h' % to, 0.5)
    h.shot(GUEST, 'tour_s%d_g' % to, 0.5)
    return True


def main():
    last = int(sys.argv[1]) if len(sys.argv) > 1 else 7
    minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 2
    for who in S.G:
        S.log_new(who)
    cur = sector()
    print('host in sector', cur, flush=True)
    while cur < last:
        to = cur + 1
        print('-> sector %d' % to, flush=True)
        if not travel(cur, to) or problems('the trip to sector %d' % to):
            return 1
        now = sector()
        if now != to:
            print('the host is in sector %d, not %d' % (now, to))
            return 1
        cur = to
        print('sector %d: both there; playing %.0f min' % (cur, minutes), flush=True)
        if not play(minutes):
            return 1
        if not quests_same():
            return 1
        print('sector %d: quest logs the same on both' % cur, flush=True)
    print('tour finished in sector %d without problems' % cur)
    return 0


if __name__ == '__main__':
    sys.exit(main())
