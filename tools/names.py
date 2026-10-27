"""Name functions from the exception-handler strings ("Class::Method").

Each function with a try/catch has a Catch@ handler that passes its own
name to the logger; map that name to the function containing the handler.
  names.py <re_dir>   -> <re_dir>/names.tsv  (address<TAB>name)
"""
import os, re, sys, bisect

d = sys.argv[1]
heads = []
for line in open(os.path.join(d, 'decomp.c'), encoding='utf-8'):
    m = re.match(r'// ==== (\S+) @ ([0-9a-f]{8})', line)
    if m and not m.group(1).startswith('Catch'):
        heads.append(int(m.group(2), 16))
heads.sort()
names = {}
for line in open(os.path.join(d, 'strings.txt'), encoding='utf-8'):
    parts = line.rstrip('\n').split('\t')
    if len(parts) < 3:
        continue
    m = re.match(r"u'([A-Za-z_]\w*::~?\w+)(\(\))?", parts[1])
    if not m:
        continue
    for ref in parts[2].split():
        r = re.match(r'Catch@([0-9a-f]{8})', ref)
        if not r:
            continue
        a = int(r.group(1), 16)
        i = bisect.bisect_right(heads, a) - 1
        if i >= 0:
            names.setdefault(heads[i], m.group(1))
with open(os.path.join(d, 'names.tsv'), 'w', encoding='utf-8') as f:
    for a in sorted(names):
        f.write('%08x\t%s\n' % (a, names[a]))
print(len(names), 'names')
