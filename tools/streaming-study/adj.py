import struct, sys, math
exec(open('dump_stream.py').read().split("mode = sys.argv[2]")[0].replace("print(", "(lambda *a,**k: None)("))
o, s = top[0x80034150][0]
lists = {}; outl = {}
for cid, so, ss in walk(data, o, o + s):
    if cid == 0x34153:
        for k in range(ss // 0xA4):
            p = so + k * 0xA4
            sid = struct.unpack_from('<h', data, p + 8)[0]
            cnt = struct.unpack_from('<i', data, p + 0x10)[0]
            lists[sid] = set(struct.unpack_from('<%dh' % cnt, data, p + 0x14))
    if cid == 0x34152:
        p = so
        while p < so + ss:
            sid, cnt = struct.unpack_from('<hh', data, p + 8)
            pts = [struct.unpack_from('<ff', data, p + 0x24 + 8 * i) for i in range(cnt)]
            if sid // 100 <= 20 and 1 <= sid % 100 < 20:
                outl[sid] = pts
            p += cnt * 8 + 0x24
def segd(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay; L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)
def dist(A, B):
    m = 1e9
    for (px, py) in A:
        for i in range(len(B)):
            ax, ay = B[i]; bx, by = B[(i + 1) % len(B)]
            m = min(m, segd(px, py, ax, ay, bx, by))
    return m
bb = {k: (min(x for x, y in v), min(y for x, y in v), max(x for x, y in v), max(y for x, y in v)) for k, v in outl.items()}
pairs = 0; missing = []
for a in outl:
    for b in outl:
        if a == b: continue
        A = bb[a]; B = bb[b]
        if A[0] > B[2] + 2 or B[0] > A[2] + 2 or A[1] > B[3] + 2 or B[1] > A[3] + 2: continue
        if min(dist(outl[a], outl[b]), dist(outl[b], outl[a])) < 1.0:
            pairs += 1
            if (b + 20) not in lists.get(a, ()):
                missing.append((name(a), name(b)))
print('adjacent pairs', pairs, 'neighbour geometry not preloaded:', len(missing), missing[:30])
# CC groups (0x34155)
cc = []
for cid, so, ss in walk(data, o, o + s):
    if cid == 0x34155:
        for k in range(ss // 76):
            p = so + k * 76
            n1 = struct.unpack_from('<h', data, p + 0x18)[0]
            l1 = struct.unpack_from('<%dh' % n1, data, p + 0x1A)
            n2 = struct.unpack_from('<h', data, p + 0x3A)[0]
            l2 = struct.unpack_from('<%dh' % n2, data, p + 0x3C)
            nm = data[p + 8:p + 0x18].split(b'\0')[0].decode()
            cc.append((nm, l1, l2))
print('CC entries', len(cc), 'with list2:', sum(1 for c in cc if c[2]), 'max n1', max(len(c[1]) for c in cc))
print('examples', cc[:3], [c for c in cc if c[2]][:5])
eff = {}
for a in lists:
    e = None
    for nm, l1, l2 in cc:
        if a in l1:
            e = set()
            for z in l1: e |= lists.get(z, set())
            for z in l2:
                e.add(z)
                if z // 100 <= 20 and 1 <= z % 100 < 20: e.add(z + 20)
            break
    eff[a] = e if e is not None else lists[a]
miss2 = [(x, y) for x, y in missing if True]
still = []
for a in outl:
    pass
cnt = 0; still = []
for (an, bn) in missing:
    pass
# recompute with eff
st = []
for a in outl:
    for b in outl:
        if a == b: continue
        A = bb[a]; B = bb[b]
        if A[0] > B[2] + 2 or B[0] > A[2] + 2 or A[1] > B[3] + 2 or B[1] > A[3] + 2: continue
        if (b + 20) in lists.get(a, ()): continue
        if min(dist(outl[a], outl[b]), dist(outl[b], outl[a])) < 1.0 and (b + 20) not in eff.get(a, ()):
            st.append((name(a), name(b)))
print('with CC unions, still not preloaded:', len(st))
inCC = set(z for nm, l1, l2 in cc for z in l1)
print('zones covered by a CC entry:', len(inCC))
