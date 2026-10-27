"""Disassemble a VA range of a PE: dis.py <exe> <start_va> <end_va|+len>"""
import sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

def strings_at(pe, v):
    off = pe.va2off(v)
    if off is None or off >= len(pe.b):
        return None
    s = pe.cstr(off)
    if 3 <= len(s) < 120 and all(32 <= c < 256 and c != 127 for c in s):
        return s.decode('cp1251', 'replace')
    return None

def main():
    pe = PE(sys.argv[1])
    a = int(sys.argv[2], 16)
    e = sys.argv[3]
    b = a + int(e[1:], 16) if e.startswith('+') else int(e, 16)
    off = pe.va2off(a)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for ins in md.disasm(pe.b[off:off + (b - a)], a):
        cm = ''
        import re
        for m in re.findall(r'0x[0-9a-f]{6,8}', ins.op_str):
            v = int(m, 16)
            s = strings_at(pe, v)
            if s:
                cm += '  ; "%s"' % s
        print('%08x  %-24s %s %s%s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, cm))

if __name__ == '__main__':
    main()
