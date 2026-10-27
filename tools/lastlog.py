"""Print what a crash report needs from a game install, compactly (saves reading whole logs).

  python tools/lastlog.py "<game folder>" [sessions]

Finds AIMCoop.log (A.I.M.) or ClanWarsCoop.log (Clan Wars) in the folder and prints the last
session(s) (from the `====` header), with long runs of repeated lines collapsed, then the tail of
the game's error.log with repeats collapsed, and the installed dinput8.dll's date.
"""
import datetime, os, re, sys

MAX_LINES = 250      # per session: keep the start and the end if it is longer


def collapse(lines):
    out, prev, n = [], None, 0
    for l in lines + [None]:
        key = re.sub(r'^[\d\-.: ]+', '', l) if l is not None else None   # ignore timestamps
        if key == prev:
            n += 1
            continue
        if n > 1:
            out.append('    (previous line repeated %d times)' % n)
        if l is not None:
            out.append(l)
        prev, n = key, 1
    return out


def read(p):
    with open(p, 'r', encoding='utf-8', errors='replace') as f:
        return f.read().splitlines()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    keep = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    dll = os.path.join(d, 'dinput8.dll')
    if os.path.exists(dll):
        print('installed dinput8.dll: %s' % datetime.datetime.fromtimestamp(os.path.getmtime(dll)))
    else:
        print('no dinput8.dll in %s - the mod is not installed there' % d)
    for name in ('AIMCoop.log', 'ClanWarsCoop.log'):
        p = os.path.join(d, name)
        if not os.path.exists(p):
            continue
        lines = read(p)
        starts = [i for i, l in enumerate(lines) if l.startswith('====')] or [0]
        for s in starts[-keep:]:
            e = next((i for i in starts if i > s), len(lines))
            sess = collapse(lines[s:e])
            print('\n--- %s, lines %d-%d of %d ---' % (name, s + 1, e, len(lines)))
            if len(sess) > MAX_LINES:
                crash = [l for l in sess[100:-(MAX_LINES - 100)] if 'CRASH' in l or 'EXCEPTION' in l]
                sess = sess[:100] + ['    ... %d lines cut ...' % (len(sess) - MAX_LINES)] + crash + sess[-(MAX_LINES - 100):]
            print('\n'.join(sess))
    p = os.path.join(d, 'error.log')
    if os.path.exists(p):
        err = collapse(read(p))
        print('\n--- error.log (last 40 distinct lines) ---')
        print('\n'.join(err[-40:]))


if __name__ == '__main__':
    main()
