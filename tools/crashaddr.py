"""Resolve the module offsets of a crash log ("mod+4376") to functions of the mod.

  python tools/crashaddr.py <aim1|aim2cw> <log file> [line filter]
  python tools/crashaddr.py <aim1|aim2cw> 4376 1cab ...

Uses build/<game>/dinput8.map, so it is only right for the build that crashed: compare the
"mod built ..." stamp in the log's session header with the build (rebuild that commit if needed).
Static functions are not in the map: an offset may belong to a static function that follows the
named one in the same file. Stack lists in crash logs are a scan of the stack, not a true call chain
- old return addresses can appear in them.
"""
import os, re, sys

DEV = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def load(game):
    syms, objs = [], []
    for line in open(os.path.join(DEV, 'build', game, 'dinput8.map'), encoding='latin-1'):
        m = re.match(r'^([0-9a-f]{8}) 00000000\s+0\s+(\S+)$', line)
        if m:
            syms.append((int(m.group(1), 16), m.group(2)))
            continue
        m = re.match(r'^([0-9a-f]{8}) ([0-9a-f]{8})\s+16\s+.*?([\w-]+)\.o:\(\.text\)', line)
        if m:
            objs.append((int(m.group(1), 16), int(m.group(2), 16), re.sub(r'-[0-9a-f]+$', '', m.group(3)) + '.c'))
    syms.sort()
    return syms, objs


def name(off, syms, objs):
    s = [x for x in syms if x[0] <= off]
    o = [x for x in objs if x[0] <= off < x[0] + x[1]]
    fn = '%s+%x' % (s[-1][1], off - s[-1][0]) if s else '?'
    return '%s (%s)' % (fn, o[-1][2] if o else '?')


def main():
    game = sys.argv[1]
    syms, objs = load(game)
    args = sys.argv[2:]
    if args and os.path.exists(args[0]):
        flt = args[1] if len(args) > 1 else 'CRASH'
        for line in open(args[0], encoding='cp1251', errors='replace'):
            if flt not in line:
                continue
            offs = re.findall(r'mod\+([0-9a-f]+)', line)
            print(line.rstrip()[:160])
            for o in offs:
                print('    mod+%s = %s' % (o, name(int(o, 16), syms, objs)))
    else:
        for a in args:
            print('mod+%s = %s' % (a, name(int(a, 16), syms, objs)))


if __name__ == '__main__':
    main()
