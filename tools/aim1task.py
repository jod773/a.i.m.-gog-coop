"""A real task, played through the base dialogs by two players (aim1game = host, aim1game2 = guest).

  aim1task.py guest_takes    the guest takes base 1's delivery, the host walks into that base, the host delivers
  aim1task.py host_takes     the host takes it, the guest walks in, the guest delivers
  aim1task.py stall          the guest dies while the host's game stands still (an alt-tab), the host flying
  aim1task.py stall_panel    the same with the host in a panel
  aim1task.py kill_guest     the guest takes a "kill this pilot" job, the host delivers the mechmind
  aim1task.py kill_host      the host takes it, the guest delivers
  aim1task.py cargo_death_guest   the guest dies carrying a task's container, the host brings it back
  aim1task.py cargo_death_host    the host dies carrying it, the guest brings it back
  aim1task.py broken         a piece shot off a glider shows on the others' copy (players both ways, an NPC)
  aim1task.py stasis         stasis: nobody explodes; time runs faster only while every player is in stasis

Each one starts its own new game. It checks outcomes, not just that both logs agree: the task must not fail, the
cargo must reach the right hold, both players must see it completed, both must have their starter kit.
Prints PASS / FAIL for each check; exit code 1 if anything failed.
"""
import ctypes, os, re, sys, time, subprocess
sys.path.insert(0, os.path.dirname(__file__))
import h, coop1

G = {'host': coop1.G1, 'guest': coop1.G2}
# base 1's dialog, client coordinates at 1920x1080
NAME_LINK, CONTINUE, JOB, TAKE, FLY_OUT = (1054, 172), (542, 380), (838, 208), (574, 266), (230, 875)
BASE, PLANT, TASK, CARGO = 'B_L1_BASE1', 'B_L1_PLANT_PF', 'T_L1_B1_1', 'TOV_KONTAINER'
failed = []


def check(what, ok, detail=''):
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', what, ' - ' + detail if detail and not ok else ''), flush=True)
    if not ok:
        failed.append(what)


def log_len(who):
    return os.path.getsize(os.path.join(G[who], h.log_of(G[who])))


def log_since(who, n):
    with open(os.path.join(G[who], h.log_of(G[who])), 'rb') as f:
        f.seek(n)
        return f.read().decode('cp1251', 'replace')


def ask(who, *cmds):
    """harness commands that only work in flight, and what they logged"""
    n = log_len(who)
    h.send(G[who], list(cmds) + ['sleep 400'])
    time.sleep(0.6)
    return log_since(who, n)


def enter(who, building):
    h.send(G[who], ['tp ' + building, 'sleep 1500', 'enter ' + building, 'sleep 4000'], timeout=30)


def fly_out(who):
    """out of the gate and away: an idle glider drifts back in, and game commands wait for the next fly-out"""
    h.send(G[who], ['click %d %d' % FLY_OUT, 'sleep 8000', 'hold 0x11 2500'], timeout=30)


def click(who, xy, wait=2500):
    h.send(G[who], ['click %d %d' % xy, 'sleep %d' % wait], timeout=30)


def first_visit(who):
    """into base 1 for the first time: the name, the starter kit, the greeting"""
    enter(who, BASE)
    h.shot(G[who], 'task_%s_1' % who, 0.5)
    click(who, NAME_LINK)
    h.shot(G[who], 'task_%s_2' % who, 0.5)
    click(who, CONTINUE)


def take_job(who):
    click(who, JOB)
    h.shot(G[who], 'task_%s_3' % who, 0.5)
    click(who, TAKE)
    h.shot(G[who], 'task_%s_4' % who, 0.5)


def quest_state(text):
    for line in text.splitlines():
        if ('quest ' + TASK) in line:
            return 'failed' if '(failed)' in line else 'complete' if '(complete)' in line else 'running'
    return 'absent'


def kit(who, label):
    """the kit's 300 crystals, and the dialog of its items is in the screenshot task_<who>_2"""
    s = ask(who, 'cargostat')
    m = re.search(r'cargo: (\d+) crystals', s)
    check('%s: got the starter kit (%s)' % (who, label), bool(m) and int(m.group(1)) >= 300, s[-300:])


def own_hold(text):
    """the 'our hold:' part of the last cargostat in a log excerpt"""
    m = re.findall(r'our hold:([^\r\n]*)', text)
    return m[-1] if m else ''


def own_only(who, label):
    """a new game: no cargo and no glider from an earlier game (#85)"""
    s = ask(who, 'cargostat')
    print('      %s %s: %s' % (who, label, ' '.join(s.split('cargo: ', 1)[-1].split())[:160]), flush=True)
    check('%s starts with an empty hold (%s)' % (who, label), CARGO not in s, s[-300:])
    return s


def run(taker, other):
    subprocess.run([sys.executable, coop1.__file__, 'start'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'newhost'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'join'], check=True)
    time.sleep(3)
    own_only('guest', 'new game')
    own_only('host', 'new game')

    first_visit(taker)
    take_job(taker)
    fly_out(taker)
    s = ask(taker, 'cargostat')
    check('%s has the container after taking the job' % taker, CARGO in s, s[-300:])
    kit(taker, 'first player into the base')
    time.sleep(2)
    s = ask(other, 'qldump', 'cargostat')
    check('%s sees the task' % other, quest_state(s) == 'running', quest_state(s))
    check('%s knows %s carries it' % (other, taker), CARGO in s, s[-300:])

    # the bug: walking into the base without the cargo failed the task for everybody
    first_visit(other)
    h.shot(G[other], 'task_%s_visit' % other, 0.5)
    fly_out(other)
    kit(other, 'second player into the base')
    time.sleep(2)
    for who in G:
        s = ask(who, 'qldump')
        check('task still running on %s after %s visited the base' % (who, other), quest_state(s) == 'running', quest_state(s))

    # the other one delivers from the taker's hold
    n = log_len(taker)
    enter(other, PLANT)
    h.shot(G[other], 'task_%s_plant' % other, 0.5)
    fly_out(other)
    time.sleep(3)
    s = ask(taker, 'cargostat')
    check('the container left %s\'s hold when %s delivered it' % (taker, other), CARGO not in s, s[-300:])
    check('%s was told' % taker, 'from our hold' in log_since(taker, n), log_since(taker, n)[-300:])
    for who in G:
        t = ask(who, 'qldump')
        check('the task\'s timer stopped on %s after the delivery' % who, ('timer ' + TASK) not in t, t[-300:])

    # back to the base: done
    enter(other, BASE)
    h.shot(G[other], 'task_%s_done' % other, 0.5)
    fly_out(other)
    time.sleep(3)
    for who in G:
        s = ask(who, 'qldump')
        check('task complete on %s' % who, quest_state(s) == 'complete', quest_state(s))

    subprocess.run([sys.executable, coop1.__file__, 'stop'], check=False)


def freeze(who, on):
    """stands in for an alt-tab of the fullscreen host: its game stops, the session stays up"""
    ntdll = ctypes.WinDLL('ntdll')
    kernel32 = ctypes.WinDLL('kernel32')
    kernel32.OpenProcess.restype = ctypes.c_void_p
    pid = h.test_pids(G[who])[0]
    p = kernel32.OpenProcess(0x0800, False, pid)         # PROCESS_SUSPEND_RESUME
    (ntdll.NtSuspendProcess if on else ntdll.NtResumeProcess)(ctypes.c_void_p(p))
    kernel32.CloseHandle(ctypes.c_void_p(p))


def stall(panel):
    """the guest dies while the host's game stands still (#84, #86): the guest must see the picture, not black;
    the host must see the glider destroyed and a mechmind, and be able to bring the guest back"""
    subprocess.run([sys.executable, coop1.__file__, 'start'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'newhost'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'join'], check=True)
    time.sleep(3)
    h.send(G['guest'], ['hold 0x11 4000'], timeout=30)    # not next to the host: the blast hits it too
    if panel:
        h.send(G['host'], ['key 0x24', 'sleep 2500'])     # the journal panel, as in the players' session
    n_host = log_len('host')
    freeze('host', True)
    try:
        time.sleep(3)                                     # the guest's copy of the host goes stale
        n_guest = log_len('guest')
        h.send(G['guest'], ['selfdestruct'])
        time.sleep(4)
        h.send(G['guest'], ['fadestat'])
        time.sleep(1)
        s = log_since('guest', n_guest)
        print('      ' + ' '.join(l for l in s.splitlines() if 'fadestat:' in l), flush=True)
        check('guest is spectating with the host away', 'we are down' in s, s[-300:])
        m = re.search(r'last ramp (\d+)%', s)
        check('the guest\'s picture is not dark, the host away', bool(m) and int(m.group(1)) >= 90, s[-300:])
        time.sleep(4)
    finally:
        freeze('host', False)
    time.sleep(6)
    s = log_since('host', n_host)
    print('      ' + ' | '.join(l[15:] for l in s.splitlines() if 'destroyed' in l or 'mechmind' in l)[:400], flush=True)
    check('the host saw the guest destroyed after its stall', 'player Guest destroyed' in s, s[-400:])
    check('the guest\'s mechmind lies in the host\'s world', 'mechmind is lying at' in s, s[-400:])
    if panel:
        h.send(G['host'], ['key 0x24', 'sleep 1500'])
    time.sleep(1)
    n_guest = log_len('guest')
    n_host = log_len('host')
    h.send(G['host'], ['pickmind', 'sleep 800'], timeout=30)
    time.sleep(2)
    if 'in a base' not in log_since('host', n_host) and 'delivered to' not in log_since('host', n_host):
        h.send(G['host'], ['enter ' + BASE, 'sleep 4000'], timeout=30)
    time.sleep(2)
    click('host', FLY_OUT, 8000)
    h.send(G['host'], ['hold 0x11 2500'], timeout=30)
    time.sleep(20)
    s = log_since('guest', n_guest)
    check('the guest is back after the mechmind was delivered', 'we are back' in s, s[-400:])

    # the normal case too: watching a teammate who is there, never dark (the guest came back in the base)
    fly_out('guest')
    time.sleep(3)
    h.send(G['guest'], ['hold 0x11 4000'], timeout=30)
    n_guest = log_len('guest')
    h.send(G['guest'], ['selfdestruct'])
    time.sleep(6)
    h.send(G['guest'], ['fadestat'])
    time.sleep(1)
    s = log_since('guest', n_guest)
    print('      ' + ' '.join(l for l in s.splitlines() if 'fadestat:' in l or 'camera on' in l), flush=True)
    m = re.search(r'last ramp (\d+)%', s)
    check('the guest watches the host', 'camera on' in s, s[-300:])
    check('the guest\'s picture is not dark, watching the host', bool(m) and int(m.group(1)) >= 90, s[-300:])
    subprocess.run([sys.executable, coop1.__file__, 'stop'], check=False)


def kill_task(taker, other):
    """base 1's third job: kill a pilot and bring its mechmind. Its target comes into the world by the taker's
    script, and the NPCs are the host's: the taker's teammate must meet it too. The other player delivers."""
    target = 'NASP-M013'
    subprocess.run([sys.executable, coop1.__file__, 'start'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'newhost'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'join'], check=True)
    time.sleep(3)
    first_visit(taker)
    fly_out(taker)
    first_visit(other)              # each player's name dialog comes first on their first visit
    fly_out(other)
    # the first two jobs are behind us
    h.send(G[taker], ['qlflag T_L1_B1_1.GET', 'qlflag T_L1_B1_1.COMPLETE', 'qlflag T_L1_B1_2.GET', 'qlflag T_L1_B1_2.COMPLETE',
                      'unlock', 'sleep 1500'], timeout=30)
    time.sleep(2)
    enter(taker, BASE)
    take_job(taker)                 # the same three greeting lines: the same places on the screen
    h.shot(G[taker], 'kill_%s_3' % taker, 0.5)
    fly_out(taker)
    time.sleep(3)
    s = ask(taker, 'qldump')
    check('%s has the job' % taker, 'quest T_L1_B1_3' in s, s[-400:])
    time.sleep(2)

    s = ask('host', 'group ' + target)
    check('the target\'s group is shown in the host\'s world', 'shown 1' in s, s[-300:])

    # both fly to it (the job's text says where); the NPCs are the host's, made around whoever comes near
    for who in (taker, other):
        h.send(G[who], ['togroup ' + target, 'sleep 500'], timeout=30)
    for who, what in ((taker, '%s meets the target' % taker), (other, '%s meets it too' % other)):
        for i in range(8):          # the group makes its glider within a few seconds
            time.sleep(2)
            s = ask(who, 'npcs')
            if target in s:
                break
        check(what, target in s, s[-600:])

    # the taker shoots it down, the mechmind drops, the taker picks it up. The hit is the host's: its own
    # projectile, or the replay of the guest's shot by the guest's glider there (the starter gun needs minutes
    # for this glider, and other NPCs join in)
    n = log_len('host')
    h.send(G[taker], ['face 1', 'tonpc 150 ' + target, 'sleep 500'], timeout=30)
    time.sleep(1)
    h.send(G['host'], [('npchit 100000 ' if taker == 'host' else 'puppethit 100000 ') + target, 'sleep 500'], timeout=30)
    time.sleep(3)
    s = ask('host', 'qldump flags')
    check('%s shot the target down' % taker, (target + '.KILLED') in s, s[-300:])
    # quickly, as a player would: NPC transports gather what lies around (a TRANTR took it after 12 s once)
    for i in range(20):
        h.send(G[taker], ['pickmind ' + target, 'sleep 700'], timeout=30)
        if target in ask(taker, 'cargostat'):
            break
    time.sleep(2)
    s = ask(taker, 'cargostat')
    check('%s carries the mechmind' % taker, target in s, s[-400:])
    time.sleep(2)
    s = ask(other, 'cargostat')
    check('%s knows' % other, target in s, s[-400:])

    # the other player walks in and delivers it
    enter(other, BASE)
    h.shot(G[other], 'kill_%s_deliver' % other, 0.5)
    fly_out(other)
    time.sleep(3)
    for who in G:
        s = ask(who, 'qldump')
        line = [l for l in s.splitlines() if 'quest T_L1_B1_3' in l]
        check('task complete on %s' % who, bool(line) and '(complete)' in line[0] and '(failed)' not in line[0], s[-400:])
    s = ask(taker, 'cargostat')
    check('the mechmind left %s\'s hold' % taker, target not in s, s[-400:])
    subprocess.run([sys.executable, coop1.__file__, 'stop'], check=False)


def cargo_death(who):
    """a player dies carrying the container: the task must survive, and the cargo must not be lost for good"""
    other = 'host' if who == 'guest' else 'guest'
    subprocess.run([sys.executable, coop1.__file__, 'start'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'newhost'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'join'], check=True)
    time.sleep(3)
    first_visit(who)
    take_job(who)
    fly_out(who)
    s = ask(who, 'cargostat')
    check('%s has the container' % who, CARGO in s, s[-300:])
    n = log_len(other)
    h.send(G[who], ['selfdestruct'])
    time.sleep(10)
    s = log_since(other, n)
    check('%s saw it destroyed' % other, 'destroyed' in s, s[-300:])
    s = ask(other, 'qldump', 'cargostat', 'mindstat')
    print('      ' + ' | '.join(l[15:] for l in s.splitlines() if 'cargo:' in l or 'mindstat:' in l)[:500], flush=True)
    check('the container stays in %s\'s hold, not on the ground' % who, ("'%s'" % CARGO) not in s,
          ' '.join(l for l in s.splitlines() if CARGO in l))
    # the other one walks into the base meanwhile: this failed the task
    first_visit(other)
    fly_out(other)
    time.sleep(2)
    for w in G:
        s = ask(w, 'qldump')
        check('task not failed on %s with the carrier down' % w, quest_state(s) != 'failed', quest_state(s))
    # bring them back
    n = log_len(who)
    h.send(G[other], ['pickmind', 'sleep 800'], timeout=30)
    time.sleep(2)
    enter(other, BASE)
    fly_out(other)
    time.sleep(20)
    s = log_since(who, n)
    check('%s is back' % who, 'we are back' in s or 'back in our own world' in s, s[-300:])
    fly_out(who)
    time.sleep(3)
    s = ask(who, 'cargostat', 'qldump')
    print('      ' + ' | '.join(l[15:] for l in s.splitlines() if 'cargo:' in l)[:300], flush=True)
    check('task not failed after %s came back' % who, quest_state(s) != 'failed', quest_state(s))
    s2 = ask(other, 'cargostat', 'qldump')
    check('%s still carries the container' % who, CARGO in own_hold(s), own_hold(s))
    check('no second container (%s picked up the wreck)' % other, CARGO not in own_hold(s2), own_hold(s2))
    check('task not failed on %s either' % other, quest_state(s2) != 'failed', quest_state(s2))
    subprocess.run([sys.executable, coop1.__file__, 'stop'], check=False)


def broken():
    """a piece shot off a glider (a wing) shows on the other players' copy too: players both ways, NPCs host -> guest"""
    subprocess.run([sys.executable, coop1.__file__, 'start'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'newhost'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'join'], check=True)
    time.sleep(3)
    for who, piece in (('host', 0), ('guest', 1)):
        other = 'guest' if who == 'host' else 'host'
        s = ask(who, 'breakpiece %d' % piece)
        print('      ' + ' '.join(l[15:] for l in s.splitlines() if 'breakpiece:' in l), flush=True)
        time.sleep(1.5)
        s = ask(other, 'playerhp')
        line = [l for l in s.splitlines() if 'playerhp: %s ' % who.capitalize() in l]
        m = re.search(r'broken ([0-9a-f]+)', line[-1]) if line else None
        check('%s\'s copy of %s lost piece %d too' % (other, who, piece), bool(m) and int(m.group(1), 16) & (1 << piece),
              line[-1] if line else s[-300:])
    # an NPC near both: the host breaks it, the guest's copy follows
    names = lambda t: set(m.group(1) for l in t.splitlines() if '(' not in l
                          for m in [re.search(r'npcs: \S+ \S+\s+(\S+)\s+-?\d', l)] if m)
    both = sorted(names(ask('host', 'npcs')) & names(ask('guest', 'npcs')))
    check('an NPC both players see', bool(both), '')
    if both:
        s = ask('host', 'breakpiece 0 ' + both[0])
        print('      ' + ' '.join(l[15:] for l in s.splitlines() if 'breakpiece:' in l), flush=True)
        time.sleep(1.5)
        s = ask('guest', 'mirrorstat')
        line = [l for l in s.splitlines() if ' %s ' % both[0] in l]
        m = re.search(r'broken ([0-9a-f]+)', line[-1]) if line else None
        check('the guest\'s copy of %s lost the piece too' % both[0], bool(m) and int(m.group(1), 16) & 1, line[-1] if line else s[-300:])
    subprocess.run([sys.executable, coop1.__file__, 'stop'], check=False)


def stasis():
    """stasis (the STASIS key) in co-op: nobody explodes; the world runs faster only while everyone is in stasis"""
    subprocess.run([sys.executable, coop1.__file__, 'start'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'newhost'], check=True)
    subprocess.run([sys.executable, coop1.__file__, 'join'], check=True)
    time.sleep(3)
    ft = lambda t: [float(x) for x in re.findall(r'world speed x([0-9.]+)', t)]
    team = lambda t: re.findall(r'team in stasis (\d)', t)
    base_host = ft(ask('host', 'stasis'))[-1]
    check('the world runs at normal speed to begin with', base_host == 1.0, str(base_host))
    n_host, n_guest = log_len('host'), log_len('guest')
    ask('guest', 'stasis in')
    time.sleep(12)
    s = ask('guest', 'stasis')
    check('the guest is in stasis', 'stasis: ours 2' in s, s[-300:])
    s = ask('host', 'stasis', 'playerhp')
    check('the host sees the guest in stasis', re.search(r'stasis: Guest 2 ', s) is not None, s[-300:])
    check('the guest\'s glider is still there on the host', 'playerhp: Guest' in s, s[-300:])
    check('the host\'s time is not faster (only the guest waits)', ft(s)[-1] < base_host * 2.5, 'frame time %s vs %.4f' % (ft(s), base_host))
    ask('host', 'stasis in')
    time.sleep(12)
    s = ask('host', 'stasis')
    check('everyone in stasis: the host\'s world runs faster', team(s) and team(s)[-1] == '1' and ft(s)[-1] > base_host * 2.5,
          s[-300:] + ' / before %.4f' % base_host)
    ask('guest', 'stasis out')
    time.sleep(2)
    s = ask('host', 'stasis')
    check('one comes out: normal time again', ft(s)[-1] < base_host * 2.5, s[-300:] + ' / before %.4f' % base_host)
    ask('host', 'stasis out')
    time.sleep(12)
    for who in G:
        s = log_since(who, n_host if who == 'host' else n_guest)
        bad = [l for l in s.splitlines() if re.search(r'player \S+ destroyed|we were destroyed|went down|we are down', l)]
        check('no player was destroyed on %s' % who, not bad, ' '.join(bad)[:300])
    s = ask('host', 'playerhp')
    check('the guest is still flying on the host', 'playerhp: Guest' in s, s[-300:])
    subprocess.run([sys.executable, coop1.__file__, 'stop'], check=False)


def main():
    c = sys.argv[1]
    if c in ('broken', 'stasis'):
        (broken if c == 'broken' else stasis)()
        print('\n%s' % ('ALL PASSED' if not failed else 'FAILED: ' + '; '.join(failed)))
        return 1 if failed else 0
    if c == 'cargo_death_guest':
        cargo_death('guest')
    elif c == 'cargo_death_host':
        cargo_death('host')
    elif c == 'kill_guest':
        kill_task('guest', 'host')
    elif c == 'kill_host':
        kill_task('host', 'guest')
    elif c == 'guest_takes':
        run('guest', 'host')
    elif c == 'host_takes':
        run('host', 'guest')
    elif c == 'stall':
        stall(False)
    elif c == 'stall_panel':
        stall(True)
    else:
        sys.exit(__doc__)
    print('\n%s' % ('ALL PASSED' if not failed else 'FAILED: ' + '; '.join(failed)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
