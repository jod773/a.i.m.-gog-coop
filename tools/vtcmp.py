"""Compare two vtables slot by slot (function size, ret n, called functions count).

  vtcmp.py <exe_a> <vt_a> <exe_b> <vt_b> [n]
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from rtti import Rtti
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

md = Cs(CS_ARCH_X86, CS_MODE_32)


def info(r, f):
    o = r.v2o(f)
    if o is None: return '?'
    code = r.pe.b[o:o + 0x3000]
    n = calls = 0
    rets = None
    offs = set()
    for ins in md.disasm(code, f):
        n += 1
        if ins.mnemonic == 'call': calls += 1
        if ins.mnemonic == 'ret':
            rets = ins.op_str or '0'
            break
        if ins.mnemonic == 'jmp' and n == 1:
            return 'jmp %s' % ins.op_str
        if n > 3000: break
    return 'n=%-4d calls=%-3d ret %s' % (n, calls, rets)


def main():
    a, b = Rtti(sys.argv[1]), Rtti(sys.argv[3])
    va, vb = int(sys.argv[2], 16), int(sys.argv[4], 16)
    n = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    sa, sb = a.slots(va), b.slots(vb)
    for i in range(n or max(len(sa), len(sb))):
        fa = sa[i] if i < len(sa) else None
        fb = sb[i] if i < len(sb) else None
        print('0x%03x  %8s %-30s | %8s %s' % (i * 4, '%x' % fa if fa else '-', info(a, fa) if fa else '',
                                             '%x' % fb if fb else '-', info(b, fb) if fb else ''))


if __name__ == '__main__':
    main()
