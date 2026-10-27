"""Search .text for instructions matching a regex over capstone output.
findpat.py <exe> <regex> [start end]"""
import sys, re, os
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
pe = PE(sys.argv[1])
rx = re.compile(sys.argv[2])
t = pe.section('.text')
s = int(sys.argv[3], 16) if len(sys.argv) > 3 else pe.imagebase + t[1]
e = int(sys.argv[4], 16) if len(sys.argv) > 4 else pe.imagebase + t[1] + t[2]
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.skipdata = True
off = pe.va2off(s)
for ins in md.disasm(pe.b[off:off + (e - s)], s):
    txt = '%s %s' % (ins.mnemonic, ins.op_str)
    if rx.search(txt):
        print('%08x  %s' % (ins.address, txt))
