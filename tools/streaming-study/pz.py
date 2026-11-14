import struct, sys, collections
b = open(sys.argv[1], 'rb').read()
def chunks(s, e):
    p = s
    while p + 8 <= e:
        i, n = struct.unpack_from('<II', b, p); yield i, p + 8, n; p += 8 + n
types = collections.Counter(); t12 = []
for i, d, n in chunks(0, len(b)):
    if i != 0x80034147: continue
    for j, dd, nn in chunks(d, d + n):
        if j != 0x3414A: continue
        at = dd
        while at + 0x44 <= dd + nn:
            t = struct.unpack_from('<I', b, at)[0]; cnt, size = struct.unpack_from('<HH', b, at + 0x40)
            types[t] += 1
            if t == 12:
                secs = struct.unpack_from('<4i', b, at + 0x30)
                bmin = struct.unpack_from('<2f', b, at + 0x20); bmax = struct.unpack_from('<2f', b, at + 0x28)
                t12.append((secs, bmin, bmax, cnt))
            at += size
print('zone types', dict(types))
nm = lambda n: '%s%d' % (chr(0x40 + n // 100), n % 100) if n > 0 else '-'
print('type 12 zones', len(t12))
for s in t12[:12]: print('  sections', [nm(x) for x in s[0]], 'box', [round(v) for v in s[1] + s[2]], 'pts', s[3])
