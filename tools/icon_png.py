import struct, zlib
def read_png(path):
    d = open(path,'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
    i=8; idat=b''; plte=None; trns=None
    while i < len(d):
        n,=struct.unpack('>I',d[i:i+4]); t=d[i+4:i+8]; c=d[i+8:i+8+n]; i+=12+n
        if t==b'IHDR': w,h,bd,ct,_,_,il=struct.unpack('>IIBBBBB',c)
        elif t==b'IDAT': idat+=c
        elif t==b'PLTE': plte=c
        elif t==b'tRNS': trns=c
    assert il==0
    raw=zlib.decompress(idat)
    ch={0:1,2:3,3:1,4:2,6:4}[ct]
    bpp=max(1,ch*bd//8); stride=(w*ch*bd+7)//8
    rows=[]; prev=bytearray(stride); p=0
    for y in range(h):
        f=raw[p]; line=bytearray(raw[p+1:p+1+stride]); p+=1+stride
        for x in range(stride):
            a=line[x-bpp] if x>=bpp else 0; b=prev[x]; c=prev[x-bpp] if x>=bpp else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+b)&255
            elif f==3: line[x]=(line[x]+(a+b)//2)&255
            elif f==4:
                pa=abs(b-c); pb=abs(a-c); pc=abs(a+b-2*c)
                pr=a if pa<=pb and pa<=pc else (b if pb<=pc else c)
                line[x]=(line[x]+pr)&255
        rows.append(line); prev=line
    out=[]
    for line in rows:
        r=[]
        for x in range(w):
            if bd==8: v=line[x*ch:(x+1)*ch]
            else:
                bits=[(line[(x*bd*ch)//8]>>(8-bd-((x*bd*ch)%8)))&((1<<bd)-1)]; v=bits
            if ct==0: g=v[0]*(255//((1<<bd)-1)); r.append((g,g,g,255))
            elif ct==4: r.append((v[0],v[0],v[0],v[1]))
            elif ct==2: r.append((v[0],v[1],v[2],255))
            elif ct==6: r.append(tuple(v))
            elif ct==3:
                k=v[0]; a=trns[k] if trns and k<len(trns) else 255
                r.append((plte[3*k],plte[3*k+1],plte[3*k+2],a))
        out.append(r)
    return w,h,out
def write_rgba(path, px):
    h=len(px); w=len(px[0])
    raw=b''.join(b'\0'+bytes(c for p in r for c in p) for r in px)
    def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b''))
