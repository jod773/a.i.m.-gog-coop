"""Two-instance Clan Wars co-op test: cwgame (host) + cwgame2 (guest), 1920x1080 windowed.

  cw2.py newgame <dir>   start <dir>, new game, click through the intro dialogue, take off,
                         save as SAVES\test.sav
  cw2.py start           start both, load SAVES\test.sav from the LOAD menu in both, host + join
  cw2.py stop            stop both test instances
  cw2.py shot NAME       both screenshots (NAME_h.png, NAME_g.png)
"""
import os, sys, time, subprocess
sys.path.insert(0, os.path.dirname(__file__))
import h

DEV = h.DEV
G1, G2, G3 = os.path.join(DEV, 'cwgame'), os.path.join(DEV, 'cwgame2'), os.path.join(DEV, 'cwgame3')


def wait_log(game, text, timeout=60, after=0):
    p = os.path.join(game, h.log_of(game))
    t = time.time()
    while time.time() - t < timeout:
        try:
            lines = open(p, encoding='cp1251', errors='replace').read()
            if text in lines[after:]:
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def log_len(game):
    try:
        return len(open(os.path.join(game, h.log_of(game)), encoding='cp1251', errors='replace').read())
    except OSError:
        return 0


def launch(game):
    if h.test_pids(game):
        sys.exit('already running: ' + game)
    subprocess.Popen([os.path.join(game, h.exe_of(game))], cwd=game)
    if not wait_log(game, 'CreateDevice hr=00000000', after=0):
        sys.exit('no device in ' + game)
    time.sleep(5)


def newgame(game):
    launch(game)
    h.send(game, ['virtual on', 'click 408 398', 'sleep 5000',
                  'click 410 425', 'sleep 1200',      # "an identification code"
                  'click 374 504', 'sleep 1200',      # Warrior
                  'click 374 741', 'sleep 1500',      # Decline the skills test
                  'click 362 534', 'sleep 6000',      # Wait
                  'click 367 540', 'sleep 1500',      # More
                  'click 366 740', 'sleep 1500',      # More
                  'click 410 433', 'sleep 1500',      # Read ARIO's message
                  'click 372 733', 'sleep 1500',      # More
                  'click 1063 205', 'sleep 12000',    # LEAVE
                  'save test.sav'], timeout=90)
    print('in flight' if wait_log(game, 'coop: saved test.sav', 30) else 'no save')


def load_save(game):
    # main menu -> LOAD -> first (newest) save -> load; make test.sav the newest
    t = os.path.join(game, 'SAVES', 'test.sav')
    if os.path.exists(t):
        os.utime(t, None)
    h.send(game, ['virtual on', 'click 408 432', 'sleep 1500', 'click 398 306', 'sleep 300', 'click 608 610'])


def start(three=False):
    gs = (G1, G2, G3) if three else (G1, G2)
    for g in gs:
        launch(g)
    for g in gs:
        load_save(g)
    time.sleep(15)
    h.send(G1, ['host'])
    time.sleep(1)
    for g in gs[1:]:
        h.send(g, ['join 127.0.0.1'])
        time.sleep(2)
    time.sleep(3)
    print('started')


def main():
    c = sys.argv[1]
    if c == 'newgame':
        newgame(os.path.join(DEV, sys.argv[2]))
    elif c == 'start':
        start()
    elif c == 'start3':
        start(True)
    elif c == 'stop':
        for g in (G1, G2, G3):
            sys.argv = ['h.py', '--game', g, 'stop']
            h.main()
    elif c == 'shot':
        h.shot(G1, sys.argv[2] + '_h', 0.5)
        h.shot(G2, sys.argv[2] + '_g', 0.5)


if __name__ == '__main__':
    main()
