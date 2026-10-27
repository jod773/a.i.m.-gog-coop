"""Print decompiled function(s) from a Ghidra export: fn.py <decomp.c> <name-or-address> [...]"""
import sys, re

path = sys.argv[1]
src = open(path, encoding='utf-8').read().replace('\r\n', '\n')
parts = re.split(r'\n// ==== ', src)
idx = {}
for p in parts[1:]:
    head, _, body = p.partition('\n')
    name, _, addr = head.partition(' @ ')
    idx[name.lower()] = (addr, body)
    idx[addr.lower()] = (name, body)
for q in sys.argv[2:]:
    k = q.lower()
    if k.startswith('0x'):
        k = k[2:].rjust(8, '0')
    if k in idx:
        a, b = idx[k]
        print('// ====', q, a)
        print(b)
    else:
        # function containing address
        print('// not found:', q)
