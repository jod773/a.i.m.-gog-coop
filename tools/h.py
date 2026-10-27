"""Drive the in-game test harness.

  h.py start [game_dir]            launch the game (windowed, harness on)
  h.py cmd "<line>" ["<line>" ...]  send commands, wait until consumed
  h.py shot <name> [scale]         screenshot -> scratchpad/<name>.png
  h.py stop                        kill the test game
  h.py log [n]                     tail of the mod's log (ClanWarsCoop.log / AIMCoop.log)
"""
import os, sys, time, subprocess

DEV = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
GAME = os.path.join(DEV, 'cwgame')
# screenshots and other test output (override with the AIMCOOP_SHOTS environment variable)
SCRATCH = os.environ.get('AIMCOOP_SHOTS') or os.path.join(DEV, 'tmp', 'shots')
os.makedirs(SCRATCH, exist_ok=True)
EXE = 'AIMII.exe'
LOG = 'ClanWarsCoop.log'


def exe_of(game):
    """AIMII.exe (A.I.M. 2 test copies) or AIM.exe (A.I.M.)"""
    return EXE if os.path.exists(os.path.join(game, EXE)) else 'AIM.exe'


def log_of(game):
    if exe_of(game) != EXE:
        return 'AIMCoop.log'
    return LOG


def send(game, lines, timeout=30):
    p = os.path.join(game, 'rr_cmd.txt')
    t = time.time()
    while os.path.exists(p) and time.time() - t < timeout:
        time.sleep(0.05)
    with open(p + '.tmp', 'w') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(p + '.tmp', p)
    t = time.time()
    run = os.path.join(game, 'rr_cmd.run')
    while (os.path.exists(p) or os.path.exists(run)) and time.time() - t < timeout:
        time.sleep(0.05)


def shot(game, name, scale=0.6):
    from PIL import Image
    bmp = os.path.join(SCRATCH, name + '.bmp')
    if os.path.exists(bmp):
        os.remove(bmp)
    send(game, ['shot ' + bmp])
    t = time.time()
    while not os.path.exists(bmp) and time.time() - t < 15:
        time.sleep(0.1)
    time.sleep(0.3)
    if not os.path.exists(bmp):
        print('no screenshot (game not rendering?)')
        return
    im = Image.open(bmp)
    if scale != 1:
        im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
    out = os.path.join(SCRATCH, name + '.png')
    im.save(out)
    os.remove(bmp)
    print(out)


def burst(game, name, n=8, gap=0.15, scale=0.3):
    """n screenshots in quick succession, combined into one contact sheet scratchpad/<name>.png"""
    from PIL import Image
    frames = []
    for i in range(n):
        bmp = os.path.join(SCRATCH, '%s_%d.bmp' % (name, i))
        if os.path.exists(bmp):
            os.remove(bmp)
        send(game, ['shot ' + bmp])
        t = time.time()
        while not os.path.exists(bmp) and time.time() - t < 5:
            time.sleep(0.02)
        time.sleep(0.05)
        try:
            im = Image.open(bmp)
            frames.append(im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS))
            im.close()
            os.remove(bmp)
        except OSError:
            pass
        time.sleep(gap)
    if not frames:
        print('no frames')
        return
    cols = 2 if n <= 4 else 3
    rows = (len(frames) + cols - 1) // cols
    w, hh = frames[0].size
    sheet = Image.new('RGB', (w * cols, hh * rows))
    for i, f in enumerate(frames):
        sheet.paste(f, ((i % cols) * w, (i // cols) * hh))
    out = os.path.join(SCRATCH, name + '.png')
    sheet.save(out)
    print(out)


def test_pids(game):
    """PIDs of game processes whose executable lives in `game`."""
    name = exe_of(game)
    exe = os.path.normcase(os.path.abspath(os.path.join(game, name)))
    ps = ('Get-Process -Name %s -ErrorAction SilentlyContinue | ' % os.path.splitext(name)[0] +
          'ForEach-Object { "$($_.Id)|$($_.Path)" }')
    r = subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        pid, _, path = line.partition('|')
        if path and os.path.normcase(os.path.abspath(path)) == exe:
            out.append(int(pid))
    return out


def main():
    a = sys.argv[1:]
    game = GAME
    if a and a[0] == '--game':
        game = a[1]
        a = a[2:]
    c = a[0]
    if c == 'start':
        if test_pids(game):
            sys.exit('a test instance is already running (h.py stop first)')
        subprocess.Popen([os.path.join(game, exe_of(game))], cwd=game)
        print('started')
    elif c == 'stop':
        # only instances started from the test folder, never the player's game
        for _ in range(100):
            pids = test_pids(game)
            if not pids:
                break
            for pid in pids:
                subprocess.run(['taskkill', '/F', '/PID', str(pid)], capture_output=True)
            time.sleep(0.1)
        time.sleep(0.3)
        print('stopped')
    elif c == 'cmd':
        send(game, a[1:])
        print('ok')
    elif c == 'shot':
        shot(game, a[1], float(a[2]) if len(a) > 2 else 0.6)
    elif c == 'burst':
        burst(game, a[1], int(a[2]) if len(a) > 2 else 9, float(a[3]) if len(a) > 3 else 0.15)
    elif c == 'log':
        n = int(a[1]) if len(a) > 1 else 15
        print(''.join(open(os.path.join(game, log_of(game)), encoding='cp1251', errors='replace').readlines()[-n:]))


if __name__ == '__main__':
    main()
