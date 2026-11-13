import struct, collections, sys
buf=None
ALIGN={0x00134B01:128, 0x00134012:8}
def data(cid,off):
    a=ALIGN.get(cid,16); d=(off+a-1)//a*a
    return d, all(b==0x11 for b in buf[off:d])
def children(off,end):
    while off+8<=end:
        cid,csz=struct.unpack_from("<II",buf,off)
        if off+8+csz>end: return
        yield cid,off+8,csz
        off+=8+csz

for path in ["F:/Games/NFSU2/TRACKS/STREAML4RA.BUN","F:/Games/NFSU2/TRACKS/STREAML4RD.BUN","F:/Games/NFSU2/TRACKS/STREAML4RB.BUN"]:
    buf=open(path,'rb').read()
    strides=collections.Counter(); objs=0; vbad=0; ibad=0; mbad=0; padbad=0; mats=0
    off=0
    while off+8<=len(buf):
        cid,csz=struct.unpack_from("<II",buf,off)
        if cid==0 and csz==0: off+=8; continue
        if off+8+csz>len(buf): break
        if cid==0x80134000:
            for c1,o1,s1 in children(off+8,off+8+csz):
                if c1!=0x80134010: continue
                objs+=1; nv=None; vb=ib=None; ml=[]
                for c2,o2,s2 in children(o1,o1+s1):
                    for c,o,s in ([(c2,o2,s2)] if c2!=0x80134100 else children(o2,o2+s2)):
                        d,ok=data(c,o)
                        if not ok: padbad+=1
                        sz=s-(d-o)
                        if   c==0x00134900: nv=struct.unpack_from("<I",buf,d+0x34)[0]
                        elif c==0x00134B01: vb=sz
                        elif c==0x00134B03: ib=sz
                        elif c==0x00134B02:
                            n=sz//60
                            ml=[struct.unpack_from("<II",buf,d+i*60+0x0C)[0] for i in range(n)]
                            ml=[(struct.unpack_from("<I",buf,d+i*60+0x0C)[0],
                                 struct.unpack_from("<I",buf,d+i*60+0x34)[0]) for i in range(n)]
                if nv and vb is not None:
                    if vb%nv==0: strides[vb//nv]+=1
                    else: vbad+=1
                if ml and ib is not None:
                    mats+=len(ml)
                    tot=sum(c for c,_ in ml)
                    if tot*2>ib: ibad+=1
                    # material ranges must tile the index buffer contiguously
                    exp=0
                    for c,o_ in ml:
                        if o_!=exp: mbad+=1; break
                        exp+=c
    print("%-22s objects=%-6d materials=%-6d | stride fail=%-3d idx overflow=%-3d range gap=%-3d pad!=0x11=%d"
          %(path.split('/')[-1],objs,mats,vbad,ibad,mbad,padbad))
    print("    strides:",dict(sorted(strides.items())))
