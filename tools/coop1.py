"""Two-instance A.I.M. co-op test: aim1game (host) + aim1game2 (guest), 1920x1080.

  coop1.py start       start both (main menu, virtual input on)
  coop1.py newhost     host: Multiplayer > Host a new co-op game > Normal; fly out
  coop1.py join        guest: Multiplayer > first found game
  coop1.py stop        stop both test instances
  coop1.py shot NAME   both screenshots (NAME_h.png, NAME_g.png)
"""
import os, sys, time, subprocess
sys.path.insert(0, os.path.dirname(__file__))
import h

DEV = h.DEV
G1, G2 = os.path.join(DEV, 'aim1game'), os.path.join(DEV, 'aim1game2')
LOG = 'AIMCoop.log'

# main menu buttons (client coordinates at 1920x1080)
BTN_NEW, BTN_MP = (960, 786), (960, 908)
DIFF_NORMAL = (960, 847)
# list dialog: first row, OK
ROW = lambda i: (1080, 802 + 19 * i)
OK = (726, 1029)
FLY_OUT = (230, 875)


def wait_log(game, text, timeout=60, since=0):
    p = os.path.join(game, LOG)
    t = time.time()
    while time.time() - t < timeout:
        try:
            if text in open(p, encoding='cp1251', errors='replace').read()[since:]:
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def log_size(game):
    try:
        return len(open(os.path.join(game, LOG), encoding='cp1251', errors='replace').read())
    except OSError:
        return 0


def click(game, xy, wait=1.0):
    h.send(game, ['click %d %d' % xy])
    time.sleep(wait)


def start():
    for g in (G1, G2):
        if h.test_pids(g):
            sys.exit('already running: ' + g)
    for g in (G1, G2):
        subprocess.Popen([os.path.join(g, 'AIM.exe')], cwd=g)
    for g in (G1, G2):
        if not wait_log(g, 'CreateDevice hr=00000000'):
            sys.exit('no device in ' + g)
    time.sleep(5)
    for g in (G1, G2):
        h.send(g, ['virtual on'])
    print('started')


def newhost():
    click(G1, BTN_MP, 3)
    click(G1, ROW(1), 1)          # after the name entry
    click(G1, OK, 2)
    click(G1, DIFF_NORMAL, 1)
    wait_log(G1, 'coop: hosting', 60)
    time.sleep(8)
    click(G1, FLY_OUT, 8)
    print('host flying')


def join():
    n = log_size(G2)
    click(G2, BTN_MP, 3)
    click(G2, ROW(3), 1)          # first found game (after the name and the two host entries)
    click(G2, OK, 2)
    ok = wait_log(G2, 'world received', 90, n)
    print('world received' if ok else 'no world')


def main():
    c = sys.argv[1]
    if c == 'start':
        start()
    elif c == 'newhost':
        newhost()
    elif c == 'join':
        join()
    elif c == 'stop':
        for g in (G1, G2):
            sys.argv = ['h.py', '--game', g, 'stop']
            h.main()
    elif c == 'shot':
        h.shot(G1, sys.argv[2] + '_h', 0.5)
        h.shot(G2, sys.argv[2] + '_g', 0.5)


if __name__ == '__main__':
    main()
