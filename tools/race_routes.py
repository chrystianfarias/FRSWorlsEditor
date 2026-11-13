import struct, glob, os, re
R = 'F:/Games/NFSU2/TRACKS/ROUTESL4RA'
def chunks(buf, off, end):
    while off + 8 <= end:
        cid, csz = struct.unpack_from('<II', buf, off)
        yield cid, off + 8, csz
        off += 8 + csz
def load(num):
    b = open('%s/Paths%s.bin' % (R, num), 'rb').read()
    out = {}
    for cid, d, s in chunks(b, 8, len(b)):
        out[cid] = b[d:d + s]
    return out
def nodes(num):
    c = load(num)[0x34148]
    res = []
    for i in range(len(c) // 24):
        x, y, fl, a, bb, n1, n2, n3, pad, dist = struct.unpack_from('<ffHBBHHHHf', c, i * 24)
        res.append(dict(x=x, y=y, fl=fl, a=a, b=bb, n=(n1, n2, n3), dist=dist))
    return res
def races():
    return sorted(re.findall(r'Paths(\d+)\.bin', ' '.join(os.listdir(R))))
if __name__ == '__main__':
    ns = nodes('4001')
    for i, n in enumerate(ns[:30]): print(i, n)
    print(len(ns), min(n['x'] for n in ns), max(n['x'] for n in ns), min(n['y'] for n in ns), max(n['y'] for n in ns))
    import collections; print(collections.Counter(n['fl'] for n in ns))
