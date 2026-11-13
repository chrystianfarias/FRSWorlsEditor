import struct, sys, collections
F="F:/Games/NFSU2/TRACKS/STREAML4RA.BUN"
buf=open(F,'rb').read()
print("filesize %d (0x%X)"%(len(buf),len(buf)))
off=0x3C
counts=collections.Counter(); order=[]
n=0
while off+8<=len(buf):
    cid,csz=struct.unpack_from("<II",buf,off)
    if cid==0 and csz==0:
        # skip zero-fill to next non-zero
        nxt=off
        while nxt+8<=len(buf) and buf[nxt:nxt+8]==b'\0'*8: nxt+=8
        if nxt+8>len(buf): break
        print("  [gap %d bytes @%08X]"%(nxt-off,off)); off=nxt; continue
    if off+8+csz>len(buf): print("truncated @%08X id=%08X sz=%d"%(off,cid,csz)); break
    counts[cid]+=1
    if n<40: print("%08X size=%-10d @%08X"%(cid,csz,off))
    n+=1
    off+=8+csz
print("... total top-level chunks:",n)
print("\n== chunk id histogram ==")
for k,v in counts.most_common(): print("  %08X : %d"%(k,v))
