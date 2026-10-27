"""MSVC RTTI: class name -> vtables (via CompleteObjectLocators), and vtable slot maps.

  rtti.py <exe>                         list classes with their vtables
  rtti.py <exe_a> <exe_b> [Class ...]   pair vtables/slots by class name (A -> B)
  rtti.py <exe_a> <exe_b> --func <addr> functions of B in the same vtable slots as A's <addr>
  rtti.py <exe_a> <exe_b> --vt <vtA>    the matching vtable of B
"""
import sys, os, struct, re
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE


class Rtti:
    def __init__(self, path):
        self.pe = pe = PE(path)
        b = pe.b
        self.base = pe.imagebase
        self.secs = [(self.base + va, self.base + va + max(vs, rs), raw, rs, ch) for n, va, vs, raw, rs, ch in pe.sections]
        self.text = [(lo, hi) for lo, hi, raw, rs, ch in self.secs if ch & 0x20]
        # type descriptors: vtable ptr, spare, ".?AV..."
        tds = {}
        for m in re.finditer(rb'\.\?A[VU][A-Za-z0-9_@?$]+\0', b):
            off = m.start() - 8
            va = self.o2v(off)
            if va: tds[va] = m.group(0)[4:-3].decode() if m.group(0).endswith(b'@@\0') else m.group(0)[4:-1].decode()
        self.tds = tds
        # COLs: sig 0, offset, cdOffset, pTD, pCHD
        cols = {}
        for lo, hi, raw, rs, ch in self.secs:
            if ch & 0x20: continue
            for o in range(raw, raw + rs - 20, 4):
                sig, off, cd, td = struct.unpack_from('<IIII', b, o)
                if sig == 0 and td in tds and off < 0x10000:
                    cols[self.o2v(o)] = (tds[td], off)
        # vtables: a dword pointing to a COL, followed by code pointers
        self.vts = {}   # vt -> (class, offset)
        for lo, hi, raw, rs, ch in self.secs:
            if ch & 0x20: continue
            for o in range(raw, raw + rs - 8, 4):
                p = struct.unpack_from('<I', b, o)[0]
                if p in cols:
                    f = struct.unpack_from('<I', b, o + 4)[0]
                    if self.is_code(f):
                        self.vts[self.o2v(o) + 4] = cols[p]
        self.by_class = {}
        for vt, (c, off) in self.vts.items():
            self.by_class.setdefault(c, []).append((off, vt))
        for c in self.by_class: self.by_class[c].sort()

    def o2v(self, off):
        for lo, hi, raw, rs, ch in self.secs:
            if raw <= off < raw + rs: return lo + off - raw
        return None

    def v2o(self, va):
        for lo, hi, raw, rs, ch in self.secs:
            if lo <= va < lo + rs: return raw + va - lo
        return None

    def is_code(self, v):
        return any(lo <= v < hi for lo, hi in self.text)

    def slots(self, vt):
        out = []
        o = self.v2o(vt)
        while o is not None and o + 4 <= len(self.pe.b):
            f = struct.unpack_from('<I', self.pe.b, o)[0]
            if not self.is_code(f) or (out and (vt + 4 * len(out)) in self.vts): break
            out.append(f)
            o += 4
        return out


def main():
    a = Rtti(sys.argv[1])
    if len(sys.argv) == 2:
        for c in sorted(a.by_class):
            print(c, ' '.join('%x@+%x(%d)' % (vt, off, len(a.slots(vt))) for off, vt in a.by_class[c]))
        return
    b = Rtti(sys.argv[2])
    args = sys.argv[3:]
    if args and args[0] == '--func':
        f = int(args[1], 16)
        for vt, (c, off) in a.vts.items():
            s = a.slots(vt)
            for i, x in enumerate(s):
                if x == f:
                    bv = dict((o, v) for o, v in b.by_class.get(c, []))
                    if off in bv:
                        bs = b.slots(bv[off])
                        print('%s+%x slot 0x%x: %x -> %s' % (c, off, i * 4, f, '%x' % bs[i] if i < len(bs) else '?'))
        return
    if args and args[0] == '--vt':
        v = int(args[1], 16)
        c, off = a.vts[v]
        bv = dict((o, x) for o, x in b.by_class.get(c, []))
        print(c, '+%x' % off, '%x -> %s' % (v, '%x' % bv[off] if off in bv else '?'))
        return
    classes = args or sorted(a.by_class)
    for c in classes:
        av = a.by_class.get(c, [])
        bv = dict(b.by_class.get(c, []))
        for off, vt in av:
            print('%s +%x  vt %x -> %s' % (c, off, vt, '%x' % bv[off] if off in bv else '?'))
            if args and off in bv:
                sa, sb = a.slots(vt), b.slots(bv[off])
                for i in range(max(len(sa), len(sb))):
                    print('   0x%03x  %s  %s' % (i * 4, '%x' % sa[i] if i < len(sa) else '-', '%x' % sb[i] if i < len(sb) else '-'))


if __name__ == '__main__':
    main()
