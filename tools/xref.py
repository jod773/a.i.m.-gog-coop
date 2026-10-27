"""Lines of the decompilation matching a regex, with the containing function.

  xref.py <re_dir> <regex> [max]
"""
import os, re, sys

d, pat = sys.argv[1], re.compile(sys.argv[2])
mx = int(sys.argv[3]) if len(sys.argv) > 3 else 200
names = {}
for l in open(os.path.join(d, 'names.tsv'), encoding='utf-8'):
    a, n = l.split('\t')
    names[a] = n.strip()
cur = ('?', '')
n = 0
for line in open(os.path.join(d, 'decomp.c'), encoding='utf-8'):
    m = re.match(r'// ==== (\S+) @ ([0-9a-f]{8})', line)
    if m:
        cur = (m.group(1), names.get(m.group(2), ''))
        continue
    if pat.search(line):
        print('%-14s %-34s %s' % (cur[0], cur[1], line.strip()[:150]))
        n += 1
        if n >= mx:
            break
