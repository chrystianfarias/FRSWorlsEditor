import struct, collections
buf=open("F:/Games/NFSU2/TRACKS/STREAML4RA.BUN",'rb').read()
def children(off,end):
    while off+8<=end:
        cid,csz=struct.unpack_from("<II",buf,off)
        if off+8+csz>end: return
        yield cid,off+8,csz
        off+=8+csz
def runlen(off):
    n=0
    while buf[off+n]==0x11: n+=1
    return n
# For each chunk type, record the alignment of (payload_start + run_of_0x11)
align=collections.defaultdict(collections.Counter)
off=0
while off+8<=len(buf):
    cid,csz=struct.unpack_from("<II",buf,off)
    if cid==0 and csz==0: off+=8; continue
    if off+8+csz>len(buf): break
    if cid==0x80134000:
        for c1,o1,s1 in children(off+8,off+8+csz):
            if c1!=0x80134010: continue
            for c2,o2,s2 in children(o1,o1+s1):
                for cid2,o,s in ([(c2,o2,s2)] if c2!=0x80134100 else children(o2,o2+s2)):
                    d=o+runlen(o)
                    a=1
                    for cand in (256,128,64,32,16,8,4,1):
                        if d%cand==0: a=cand; break
                    align[cid2][a]+=1
    off+=8+csz
for cid,c in sorted(align.items()):
    print("%08X : %s"%(cid, dict(sorted(c.items(),reverse=True))))
