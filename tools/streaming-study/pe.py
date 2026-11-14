import struct, sys
b = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]
nsec = struct.unpack_from('<H', b, pe + 6)[0]
opt = struct.unpack_from('<H', b, pe + 20)[0]
base = struct.unpack_from('<I', b, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt + 40 * i
    nm = b[o:o+8].rstrip(b'\0').decode(); vs, va, rs, ra = struct.unpack_from('<IIII', b, o + 8)
    secs.append((nm, va, vs, ra, rs))
def rd(va, fmt):
    r = va - base
    for nm, sva, vs, ra, rs in secs:
        if sva <= r < sva + max(vs, rs):
            return struct.unpack_from(fmt, b, ra + r - sva)
for va, f, label in [(0x79DC54, '<3I', 'TrackStreamingPoolSizes'), (0x7FA9D4, '<3I', 'CarLoaderPoolSizes'), (0x7A47A4, '<2f', 'predict 1.5/100'), (0x7A52F4, '<f', 'max speed')]:
    print(label, rd(va, f))
