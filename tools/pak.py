"""A.I.M. 2 .pak archives ("SGMF").

header : char magic[4]='SGMF'; u16 pad; u32 n_files; u32 n_blocks; u32 block_size; u32 unk
files  : n_files x { char name[0x50]; u32 pos; u32 len }   (pos/len in the decompressed stream)
blocks : n_blocks x { u32 crc; u32 algorithm; u32 offset }
at offset: u32 packed_size; data. algorithm 0 = stored, 1 = LZO1X; other bits are
legacy transforms not used by these archives (reported if seen).

pak.py list <pak> [filter]
pak.py extract <pak> <name> <out>
"""
import struct, sys, os


def lzo1x_decompress(src, out_len):
    dst = bytearray()
    ip = 0
    n = len(src)

    def get_len(ip, base):
        t = 0
        while src[ip] == 0:
            t += 255
            ip += 1
        return t + base + src[ip], ip + 1

    t = src[ip]
    state = 0
    if t > 17:
        t -= 17
        dst += src[ip + 1:ip + 1 + t]
        ip += 1 + t
        state = 4 if t >= 4 else t
        # after an initial literal run go to "match" handling
        first = True
    else:
        first = False
    while ip < n:
        t = src[ip]
        ip += 1
        if t < 16:
            if state == 0:
                # literal run
                if t == 0:
                    t, ip = get_len(ip, 15)
                t += 3
                dst += src[ip:ip + t]
                ip += t
                state = 4
                continue
            elif state < 4:
                # 2-byte match, distance from next byte
                m_pos = len(dst) - 1 - (t >> 2) - (src[ip] << 2)
                ip += 1
                for _ in range(2):
                    dst.append(dst[m_pos]); m_pos += 1
                state = t & 3
                dst += src[ip:ip + state]; ip += state
                continue
            else:
                # 3-byte match at distance 2049..3072
                m_pos = len(dst) - 1 - 0x800 - (t >> 2) - (src[ip] << 2)
                ip += 1
                for _ in range(3):
                    dst.append(dst[m_pos]); m_pos += 1
                state = t & 3
                dst += src[ip:ip + state]; ip += state
                continue
        if t >= 64:
            m_pos = len(dst) - 1 - ((t >> 2) & 7) - (src[ip] << 3)
            ip += 1
            ln = (t >> 5) - 1 + 2
        elif t >= 32:
            ln = t & 31
            if ln == 0:
                ln, ip = get_len(ip, 31)
            ln += 2
            d = src[ip] | (src[ip + 1] << 8)
            ip += 2
            m_pos = len(dst) - 1 - (d >> 2)
            t = d
        else:  # 16..31
            m_pos = len(dst) - ((t & 8) << 11)
            ln = t & 7
            if ln == 0:
                ln, ip = get_len(ip, 7)
            ln += 2
            d = src[ip] | (src[ip + 1] << 8)
            ip += 2
            m_pos -= d >> 2
            if m_pos == len(dst):
                break   # end of stream
            m_pos -= 0x4000
            t = d
        for _ in range(ln):
            dst.append(dst[m_pos]); m_pos += 1
        state = t & 3
        dst += src[ip:ip + state]
        ip += state
    return bytes(dst)


def rle16_decompress(src):
    """Algorithm 4 (engine FUN_0057d700): byte 0 = marker M; then 16-bit words.
    A word M<<8|n (n != 0xff) repeats the next word n+3 times; M<<8|0xff escapes
    the next word as a literal; anything else is a literal word."""
    m = src[0] << 8
    out = bytearray()
    n = len(src) >> 1
    i = 1
    while i < n:
        w = src[2 * i] | (src[2 * i + 1] << 8)
        if (w & 0xff00) == m:
            i += 1
            v = src[2 * i:2 * i + 2]
            if w == m | 0xff:
                out += v
            else:
                out += v * ((w & 0xff) + 3)
        else:
            out += src[2 * i:2 * i + 2]
        i += 1
    return bytes(out)


class Pak:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        h = self.f.read(0x16)
        assert h[:4] == b'SGMF', h[:4]
        self.n_files, self.n_blocks, self.block_size, self.unk = struct.unpack_from('<IIII', h, 6)
        raw = self.f.read(self.n_files * 0x58)
        self.files = []
        for i in range(self.n_files):
            e = raw[i * 0x58:(i + 1) * 0x58]
            name = e[:0x50].split(b'\0')[0].decode('cp1251')
            pos, ln = struct.unpack_from('<II', e, 0x50)
            self.files.append((name, pos, ln))
        raw = self.f.read(self.n_blocks * 12)
        self.blocks = [struct.unpack_from('<III', raw, i * 12) for i in range(self.n_blocks)]
        self.cache = {}

    def block(self, i):
        if i in self.cache:
            return self.cache[i]
        crc, alg, off = self.blocks[i]
        self.f.seek(off)
        size, = struct.unpack('<I', self.f.read(4))
        data = self.f.read(size)
        if alg == 0:
            out = data
        elif alg == 1:
            out = lzo1x_decompress(data, self.block_size)
        elif alg == 4:
            out = rle16_decompress(data)
        else:
            raise NotImplementedError('algorithm %x' % alg)
        if len(self.cache) > 64:
            self.cache.clear()
        self.cache[i] = out
        return out

    def read(self, name):
        for n, pos, ln in self.files:
            if n.lower() == name.lower():
                out = bytearray()
                bi = pos // self.block_size
                o = pos % self.block_size
                while len(out) < ln:
                    b = self.block(bi)
                    out += b[o:o + (ln - len(out))]
                    bi += 1
                    o = 0
                return bytes(out)
        raise KeyError(name)


if __name__ == '__main__':
    cmd = sys.argv[1]
    p = Pak(sys.argv[2])
    if cmd == 'list':
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        print('files', p.n_files, 'blocks', p.n_blocks, 'block_size', hex(p.block_size),
              'algs', sorted(set(b[1] for b in p.blocks)))
        for n, pos, ln in p.files:
            if flt in n.lower():
                print('%10d %10d %s' % (pos, ln, n))
    elif cmd == 'extract':
        data = p.read(sys.argv[3])
        open(sys.argv[4], 'wb').write(data)
        print('wrote', sys.argv[4], len(data))
