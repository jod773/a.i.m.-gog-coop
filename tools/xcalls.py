"""Side-by-side calls and global references of two matched functions.

  xcalls.py <exe_a> <dir_a> <fa> <exe_b> <dir_b> <fb> [filter]

Lists, in order, every call (target) and absolute memory reference of each
function, so callees, call sites and globals can be paired up by position.
Function extents come from the Ghidra export (next function start).
"""
import sys, os, re
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

def starts(d):
    s = []
    for line in open(os.path.join(d, 'decomp.c'), encoding='utf-8'):
        m = re.match(r'// ==== \S+ @ ([0-9a-f]{8})', line)
        if m:
            s.append(int(m.group(1), 16))
    return sorted(s)

def events(exe, d, fa):
    pe = PE(exe)
    s = starts(d)
    import bisect
    i = bisect.bisect_right(s, fa)
    end = s[i] if i < len(s) else fa + 0x2000
    off = pe.va2off(fa)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = True
    out = []
    for ins in md.disasm(pe.b[off:off + (end - fa)], fa):
        t = '%s %s' % (ins.mnemonic, ins.op_str)
        if ins.mnemonic == 'call':
            out.append((ins.address, 'call', t))
        else:
            for m in re.findall(r'0x[0-9a-f]{6,8}', ins.op_str):
                v = int(m, 16)
                if 0x400000 <= v < 0x800000:
                    out.append((ins.address, 'ref', t))
                    break
    return out

def main():
    a = events(sys.argv[1], sys.argv[2], int(sys.argv[3], 16))
    b = events(sys.argv[4], sys.argv[5], int(sys.argv[6], 16))
    flt = sys.argv[7] if len(sys.argv) > 7 else None
    if flt:
        a = [e for e in a if e[1] == flt]
        b = [e for e in b if e[1] == flt]
    n = max(len(a), len(b))
    for i in range(n):
        x = a[i] if i < len(a) else (0, '', '')
        y = b[i] if i < len(b) else (0, '', '')
        print('%3d %08x %-38s | %08x %s' % (i, x[0], x[2][:38], y[0], y[2][:50]))

if __name__ == '__main__':
    main()
