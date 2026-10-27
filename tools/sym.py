"""Resolve module offsets (RVAs, e.g. "mod+ce8f" from the harness stack dump) with an ld map.

  sym.py <map> <rva> [...]      (also accepts a pasted "stack:" log line)
"""
import sys, re, bisect

syms = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.match(r'^([0-9a-f]{8}) [0-9a-f]{8}\s+0\s+(\S+)$', line.strip())
    if m:
        syms.append((int(m.group(1), 16), m.group(2)))
syms.sort()
addrs = [a for a, _ in syms]
args = ' '.join(sys.argv[2:])
for tok in re.findall(r'mod\+([0-9a-f]+)|\b([0-9a-f]{3,6})\b', args):
    v = int(tok[0] or tok[1], 16)
    i = bisect.bisect_right(addrs, v) - 1
    print('%6x  %s+%x' % (v, syms[i][1] if i >= 0 else '?', v - addrs[i] if i >= 0 else 0))
