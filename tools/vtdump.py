import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE
pe = PE(sys.argv[1])
va = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
o = pe.va2off(va)
for i in range(n):
    print('+%03x  %08x' % (i * 4, struct.unpack_from('<I', pe.b, o + 4 * i)[0]))
