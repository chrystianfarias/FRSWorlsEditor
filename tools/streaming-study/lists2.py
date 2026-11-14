import struct, sys
exec(open('dump_stream.py').read().split("mode = sys.argv[2]")[0].replace("print(", "(lambda *a,**k: None)("))
# reparse preculler quickly
o, s = top[0x80034150][0]
lists = []; outl = {}
for cid, so, ss in walk(data, o, o + s):
    if cid == 0x34153:
        for k in range(ss // 0xA4):
            p = so + k * 0xA4
            sid = struct.unpack_from('<h', data, p + 8)[0]
            cnt = struct.unpack_from('<i', data, p + 0x10)[0]
            lists.append((sid, struct.unpack_from('<%dh' % cnt, data, p + 0x14)))
    if cid == 0x34152:
        p = so
        while p < so + ss:
            sid, cnt = struct.unpack_from('<hh', data, p + 8)
            outl[sid] = outl.get(sid, 0) + 1
            p += cnt * 8 + 0x24
bynum = {s['num']: s for s in secs}
selfmiss = [name(z) for z, l in lists if z + 20 not in l]
print('zones whose list lacks own geometry section z+20:', selfmiss)
idnot = 0; geomnot = 0; ex = []
for z, l in lists:
    for x in l:
        if x % 100 < 20 and x // 100 <= 20 and (x + 20) not in l: idnot += 1; ex.append((name(z), name(x)))
        if 20 <= x % 100 < 40 and x // 100 <= 20 and (x - 20) not in l: geomnot += 1
print('listed zone ids without their geometry section:', idnot, ex[:10])
print('listed geometry sections without their zone id:', geomnot)
print('zones with lists', len(lists), 'drivable outlines (A-T,1..19):', sum(1 for k in outl if k//100<=20 and k%100<20), 'others:', sorted(name(k) for k in outl if not (k//100<=20 and k%100<20)))
mx = max(len(l) for z,l in lists); print('max list length', mx)
# alt sections (n<20 with a table entry) per zone use
alts = [n for n in bynum if 0 < n and n // 100 <= 20 and n % 100 < 20]
for a in sorted(alts):
    zs = [name(z) for z, l in lists if a in l]
    print('alt', name(a), 'loaded in', len(zs), 'zones')
