"""Find where dword values occur in a PE image (vtables, pointer tables, pushes).

  dwfind.py <exe> <hex value> [...]
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE

pe = PE(sys.argv[1])
for a in sys.argv[2:]:
    pat = struct.pack('<I', int(a, 16))
    i = pe.b.find(pat)
    while i >= 0:
        va = pe.off2va(i)
        print('%s at va %s' % (a, hex(va) if va else 'file %x' % i))
        i = pe.b.find(pat, i + 1)
