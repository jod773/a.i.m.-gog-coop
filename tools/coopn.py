"""Many-player A.I.M. co-op test: aim1game hosts, aim1nNN copies (NN = 02..) join it.

  coopn.py make N      make guest copies aim1n02..aim1nN from aim1game (light settings, names G02..)
  coopn.py run N       start the host, host a new game, then start and join the guests one by one
  coopn.py status      players the host's log has seen join / leave
  coopn.py stop        stop the host and every guest copy

Guests run with antialiasing, dynamic lights and VSync off and a 30 fps limit, so many fit on one PC.
"""
import os, sys, time, shutil, subprocess, glob
sys.path.insert(0, os.path.dirname(__file__))
import h, coop1, mk_testcopy

DEV = h.DEV
HOST = coop1.G1
DLL = os.path.join(DEV, 'build', 'aim1', 'dinput8.dll')

INI = """[Dev]
SkipVideos=1
Harness=1
WindowX={x}
WindowY={y}

[Video]
Renderer=9
DynamicLights=0
Antialiasing=0
Anisotropy=0
VSync=0
FrameLimit=30
IntroVideos=0
DisplayMode=windowed

[Coop]
Name=G{n:02d}
Loopback=1
"""


def guest(n):
    return os.path.join(DEV, 'aim1n%02d' % n)


def guests():
    return sorted(glob.glob(os.path.join(DEV, 'aim1n[0-9][0-9]')))


def make(count):
    for n in range(2, count + 1):
        g = guest(n)
        if not os.path.exists(g):
            mk_testcopy.main(g, HOST)
            for f in ('AIMCoop.log', 'error.log', 'rr_cmd.txt'):
                try:
                    os.remove(os.path.join(g, f))
                except OSError:
                    pass
        shutil.copy2(DLL, os.path.join(g, 'dinput8.dll'))
        with open(os.path.join(g, 'AIMCoop.ini'), 'w') as f:
            f.write(INI.format(n=n, x=40 * (n % 16), y=30 * (n % 16)))
    print('guests ready:', count - 1)


def start(game):
    if h.test_pids(game):
        sys.exit('already running: ' + game)
    try:
        os.remove(os.path.join(game, coop1.LOG))    # test copies start a new log anyway
    except OSError:
        pass
    subprocess.Popen([os.path.join(game, 'AIM.exe')], cwd=game)
    if not coop1.wait_log(game, 'CreateDevice hr=00000000', 90):
        sys.exit('no device in ' + game)
    time.sleep(5)
    h.send(game, ['virtual on'])


def join(game):
    n = coop1.log_size(game)
    coop1.click(game, coop1.BTN_MP, 3)
    coop1.click(game, coop1.ROW(3), 1)
    coop1.click(game, coop1.OK, 2)
    return coop1.wait_log(game, 'world received', 120, n)


def run(count):
    shutil.copy2(DLL, os.path.join(HOST, 'dinput8.dll'))
    start(HOST)
    coop1.newhost()
    ok = 0
    for n in range(2, count + 1):
        g = guest(n)
        start(g)
        r = join(g)
        ok += r
        print('G%02d' % n, 'joined' if r else 'NO WORLD', flush=True)
    print('%d of %d guests joined' % (ok, count - 1))
    status()


def status():
    log = open(os.path.join(HOST, coop1.LOG), encoding='cp1251', errors='replace').read()
    log = log[max(log.rfind('===='), 0):]
    conn = sum('net: peer' in l and 'connected' in l for l in log.splitlines())
    left = sum('net: peer' in l and 'left' in l for l in log.splitlines())
    print('host log: %d connected, %d left, CRASH lines: %d' % (conn, left, log.count('CRASH:')))


def stop():
    for g in [HOST] + guests():
        if h.test_pids(g):
            sys.argv = ['h.py', '--game', g, 'stop']
            h.main()


if __name__ == '__main__':
    c = sys.argv[1]
    if c == 'make':
        make(int(sys.argv[2]))
    elif c == 'run':
        run(int(sys.argv[2]))
    elif c == 'status':
        status()
    elif c == 'stop':
        stop()
