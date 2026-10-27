"""Read memory of the running *test* game instance (never the player's game).

  mem.py [--game DIR] floats <addr|[ptr]+off> <count>
  mem.py [--game DIR] dwords <addr|[ptr]+off> <count>
  mem.py [--game DIR] watch  <addr|[ptr]+off> <count> <seconds>   (floats, prints changes)
Addresses: hex VA, "[63a1f4]+190" dereferences the pointer at 0x63a1f4 then adds 0x190,
nesting allowed: "[[63a180]+ac]+50".
"""
import ctypes, ctypes.wintypes as wt, os, re, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import h

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def open_game(game):
    pids = h.test_pids(game)
    if len(pids) != 1:
        sys.exit('need exactly one test instance in %s (found %d)' % (game, len(pids)))
    hp = k32.OpenProcess(0x0010 | 0x0400, False, pids[0])   # VM_READ | QUERY_INFORMATION
    if not hp:
        sys.exit('OpenProcess failed')
    return hp


def read(hp, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        raise OSError('read %08x failed' % addr)
    return buf.raw


def resolve(hp, expr):
    expr = expr.strip()
    m = re.fullmatch(r'\[(.*)\]\+?([0-9a-fA-F]*)', expr)
    if m and expr.startswith('['):
        # find matching bracket
        depth = 0
        for i, c in enumerate(expr):
            depth += c == '['
            depth -= c == ']'
            if depth == 0:
                break
        inner, rest = expr[1:i], expr[i + 1:]
        base = struct.unpack('<I', read(hp, resolve(hp, inner), 4))[0]
        off = int(rest.lstrip('+') or '0', 16)
        return base + off
    return int(expr, 16)


def main():
    a = sys.argv[1:]
    game = h.GAME
    if a and a[0] == '--game':
        game, a = a[1], a[2:]
    hp = open_game(game)
    cmd, addr, n = a[0], resolve(hp, a[1]), int(a[2])
    if cmd == 'floats':
        v = struct.unpack('<%df' % n, read(hp, addr, 4 * n))
        for i in range(0, n, 4):
            print('%08x +%03x  %s' % (addr + 4 * i, 4 * i, '  '.join('%12.4f' % x for x in v[i:i + 4])))
    elif cmd == 'dwords':
        v = struct.unpack('<%dI' % n, read(hp, addr, 4 * n))
        for i in range(0, n, 4):
            print('%08x +%03x  %s' % (addr + 4 * i, 4 * i, '  '.join('%08x' % x for x in v[i:i + 4])))
    elif cmd == 'watch':
        secs = float(a[3])
        prev = struct.unpack('<%df' % n, read(hp, addr, 4 * n))
        t0 = time.time()
        changed = set()
        while time.time() - t0 < secs:
            time.sleep(0.2)
            cur = struct.unpack('<%df' % n, read(hp, addr, 4 * n))
            for i in range(n):
                if cur[i] != prev[i]:
                    changed.add(i)
            prev = cur
        for i in sorted(changed):
            print('+%03x %12.4f' % (4 * i, prev[i]))


if __name__ == '__main__':
    main()
