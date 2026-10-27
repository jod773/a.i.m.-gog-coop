"""Find code referencing a 32-bit value (push/mov imm, [abs] operands, call/jmp rel targets).

  immref.py <exe> <value> [...]
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE

pe = PE(sys.argv[1])
b = pe.b
code = [(va, raw, rs) for name, va, vs, raw, rs, ch in pe.sections if ch & 0x20]
for a in sys.argv[2:]:
    v = int(a, 16)
    print('==', hex(v))
    pat = struct.pack('<I', v)
    for va, raw, rs in code:
        seg = b[raw:raw + rs]
        i = seg.find(pat)
        while i >= 0:
            print('  abs  %08x' % (pe.imagebase + va + i))
            i = seg.find(pat, i + 1)
        # rel32 call/jmp
        for j in range(len(seg) - 5):
            if seg[j] in (0xe8, 0xe9):
                t = pe.imagebase + va + j + 5 + struct.unpack_from('<i', seg, j + 1)[0]
                if t == v:
                    print('  %s %08x' % ('call' if seg[j] == 0xe8 else 'jmp ', pe.imagebase + va + j))
