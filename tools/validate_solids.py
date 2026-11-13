import struct, collections, sys

def align16(buf, off):
    """Chunk payloads are preceded by a run of 0x11 filler bytes."""
    while off < len(buf) and buf[off] == 0x11:
        off += 1
    return off

def children(buf, off, end):
    while off + 8 <= end:
        cid, csz = struct.unpack_from("<II", buf, off)
        if off + 8 + csz > end: return
        yield cid, off + 8, csz
        off += 8 + csz

path = sys.argv[1] if len(sys.argv) > 1 else "F:/Games/NFSU2/TRACKS/STREAML4RA.BUN"
buf = open(path, 'rb').read()

strides = collections.Counter()
bad = 0; objs = 0; packs = 0
tri_ok = 0; tri_bad = 0
names = []

off = 0
while off + 8 <= len(buf):
    cid, csz = struct.unpack_from("<II", buf, off)
    if cid == 0 and csz == 0:
        off += 8; continue
    if off + 8 + csz > len(buf): break
    if cid == 0x80134000:
        packs += 1
        for c1, o1, s1 in children(buf, off + 8, off + 8 + csz):
            if c1 != 0x80134010: continue
            objs += 1
            nverts = ntris = None; vb = ib = None; nmat = 0; name = None
            for c2, o2, s2 in children(buf, o1, o1 + s1):
                p2 = align16(buf, o2); s2e = o2 + s2
                if c2 == 0x00134011:
                    ntris = struct.unpack_from("<I", buf, p2 + 0x14)[0]
                    z = buf.find(b'\0', p2 + 0xA4)
                    name = buf[p2+0xA4:z].decode('latin1', 'replace')
                elif c2 == 0x80134100:
                    for c3, o3, s3 in children(buf, o2, s2e):
                        p3 = align16(buf, o3)
                        if   c3 == 0x00134900: nverts = struct.unpack_from("<I", buf, p3 + 0x34)[0]
                        elif c3 == 0x00134B01: vb = s3 - (p3 - o3)
                        elif c3 == 0x00134B03: ib = s3 - (p3 - o3)
            if nverts and vb is not None:
                if vb % nverts == 0: strides[vb // nverts] += 1
                else: bad += 1; names.append((name, nverts, vb))
            if ntris and ib is not None:
                if ib >= ntris * 6: tri_ok += 1
                else: tri_bad += 1
    off += 8 + csz

print("solid packs: %d   solid objects: %d" % (packs, objs))
print("vertex-size divisible: %d   NOT divisible: %d" % (sum(strides.values()), bad))
print("index buffer == numTris*6: ok=%d  mismatch=%d" % (tri_ok, tri_bad))
print("\nstride histogram:")
for s, n in sorted(strides.items()): print("  %3d bytes : %6d objects" % (s, n))
if names: print("\nfailures (first 5):", names[:5])
