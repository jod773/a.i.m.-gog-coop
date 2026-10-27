"""Minimal PE32 parser (sections, imports, va<->offset)."""
import struct


class PE:
    def __init__(self, path):
        self.path = path
        self.b = b = open(path, 'rb').read()
        e = struct.unpack_from('<I', b, 0x3c)[0]
        assert b[e:e + 4] == b'PE\0\0'
        self.machine, nsec, self.timestamp = struct.unpack_from('<HHI', b, e + 4)
        optsz = struct.unpack_from('<H', b, e + 20)[0]
        o = e + 24
        self.entry = struct.unpack_from('<I', b, o + 16)[0]
        self.imagebase = struct.unpack_from('<I', b, o + 28)[0]
        ndd = struct.unpack_from('<I', b, o + 92)[0]
        self.dd = [struct.unpack_from('<II', b, o + 96 + 8 * i) for i in range(ndd)]
        s = o + optsz
        self.sections = []
        for i in range(nsec):
            name = b[s:s + 8].rstrip(b'\0').decode('latin1')
            vsize, va, rsize, raw = struct.unpack_from('<IIII', b, s + 8)
            ch = struct.unpack_from('<I', b, s + 36)[0]
            self.sections.append((name, va, vsize, raw, rsize, ch))
            s += 40

    def rva2off(self, rva):
        for name, va, vs, raw, rs, ch in self.sections:
            if va <= rva < va + max(vs, rs):
                return raw + rva - va if rva - va < rs else None
        return None

    def va2off(self, va):
        return self.rva2off(va - self.imagebase)

    def off2va(self, off):
        for name, va, vs, raw, rs, ch in self.sections:
            if raw <= off < raw + rs:
                return self.imagebase + va + off - raw
        return None

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s

    def cstr(self, off):
        e = self.b.find(b'\0', off)
        return self.b[off:e]

    def imports(self):
        rva, size = self.dd[1]
        res = {}
        if not rva:
            return res
        p = self.rva2off(rva)
        while True:
            oft, ts, fc, name, ft = struct.unpack_from('<IIIII', self.b, p)
            if not name:
                break
            dll = self.cstr(self.rva2off(name)).decode('latin1')
            thunk = oft or ft
            q = self.rva2off(thunk)
            i = 0
            lst = []
            while True:
                v = struct.unpack_from('<I', self.b, q + 4 * i)[0]
                if not v:
                    break
                iat = self.imagebase + ft + 4 * i
                if v & 0x80000000:
                    lst.append(('#%d' % (v & 0xffff), iat))
                else:
                    lst.append((self.cstr(self.rva2off(v) + 2).decode('latin1'), iat))
                i += 1
            res[dll] = lst
            p += 20
        return res
