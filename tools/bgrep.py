"""bgrep.py <file> <regex-bytes> : print offsets/VAs and context of matches"""
import sys, re, os
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE
pe = PE(sys.argv[1])
for m in re.finditer(sys.argv[2].encode('latin1'), pe.b, re.I):
    s = max(0, m.start() - 24)
    va = pe.off2va(m.start())
    ctx = pe.b[s:m.end() + 40]
    print('%08x va=%s  %r' % (m.start(), hex(va) if va else '-', ctx))
