import struct,zlib,sys
def tga2png(src,dst,scale=1):
    b=open(src,'rb').read()
    idl,cmt,it=b[0],b[1],b[2]
    w,h=struct.unpack_from('<HH',b,12); bpp=b[16]; desc=b[17]
    p=18+idl
    print(src,'type',it,w,h,bpp,hex(desc))
    bp=bpp//8
    if it==2:
        px=b[p:p+w*h*bp]
    elif it==10:
        out=bytearray(); 
        while len(out)<w*h*bp:
            c=b[p]; p+=1
            if c&0x80:
                n=(c&0x7f)+1; out+=b[p:p+bp]*n; p+=bp
            else:
                n=c+1; out+=b[p:p+n*bp]; p+=n*bp
        px=bytes(out)
    rows=[px[y*w*bp:(y+1)*w*bp] for y in range(h)]
    if not (desc&0x20): rows=rows[::-1]
    raw=bytearray()
    for r in rows[::scale]:
        raw.append(0)
        for x in range(0,w,scale):
            q=x*bp; raw+=bytes((r[q+2],r[q+1],r[q]))+ (bytes((r[q+3],)) if bp==4 else b'')
    W=len(range(0,w,scale)); H=len(rows[::scale])
    def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',W,H,8,6 if bp==4 else 2,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(raw),6))+chunk(b'IEND',b'')
    open(dst,'wb').write(png)
tga2png(sys.argv[1],sys.argv[2],int(sys.argv[3]))
