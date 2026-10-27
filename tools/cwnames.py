"""Name functions from their exception-log strings ("Class::Method", pushed in the
catch handler placed right after the function body).

  cwnames.py <exe> <re_dir>      writes <re_dir>/names.tsv
"""
import sys, os, re, struct, bisect
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE

exe, d = sys.argv[1], sys.argv[2]
pe = PE(exe)
b = pe.b
starts = []
for line in open(os.path.join(d, 'decomp.c'), encoding='utf-8'):
    m = re.match(r'// ==== \S+ @ ([0-9a-f]{8})', line)
    if m:
        starts.append(int(m.group(1), 16))
starts.sort()
strs = []
for line in open(os.path.join(d, 'strings.txt'), encoding='utf-8'):
    p = line.split('\t')
    if len(p) < 2:
        continue
    s = p[1]
    m = re.match(r"^u'[^A-Za-z_~]?([A-Za-z_][A-Za-z0-9_]*::~?[A-Za-z_][A-Za-z0-9_]*)(\(\))?'$", s)
    if m:
        strs.append((int(p[0], 16), m.group(1)))
code = [(pe.imagebase + va, raw, rs) for n, va, vs, raw, rs, ch in pe.sections if ch & 0x20]
names = {}
for va, name in strs:
    # the string may start one byte later than the strings.txt entry (prefix byte)
    for v in (va, va + 1):
        pat = b'\x68' + struct.pack('<I', v)          # push imm32
        for cva, raw, rs in code:
            seg = b[raw:raw + rs]
            i = seg.find(pat)
            while i >= 0:
                site = cva + i
                k = bisect.bisect_right(starts, site) - 1
                # the catch funclet usually sits after the function; take the function before
                # the funclet if the funclet itself was made a function by the decompiler
                f = starts[k] if k >= 0 else None
                if f is not None and site - f < 0x40 and k > 0:
                    f = starts[k - 1]
                if f is not None and f not in names:
                    names[f] = name
                i = seg.find(pat, i + 1)
with open(os.path.join(d, 'names.tsv'), 'w', encoding='utf-8') as out:
    for f in sorted(names):
        out.write('%08x\t%s\n' % (f, names[f]))
print(len(names), 'names')
