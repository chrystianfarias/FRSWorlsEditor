import struct, sys, collections

path = sys.argv[1]
data = open(path, 'rb').read()

def walk(buf, start, end):
    p = start
    while p + 8 <= end:
        cid, size = struct.unpack_from('<II', buf, p)
        if cid == 0 and size == 0:
            p += 8
            continue
        yield cid, p + 8, size
        p += 8 + size

def name(n):
    if n <= 0:
        return '--'
    return '%s%d' % (chr(0x40 + n // 100), n % 100)

top = {}
for cid, off, size in walk(data, 0, len(data)):
    top.setdefault(cid, []).append((off, size))
print('top chunks:', ', '.join('%08X x%d' % (k, len(v)) for k, v in top.items()))

off, size = top[0x34110][0]
n = size // 80
secs = []
for i in range(n):
    e = data[off + i * 80: off + i * 80 + 80]
    nm = e[0:8].split(b'\0')[0].decode('ascii', 'replace')
    num = struct.unpack_from('<h', e, 8)[0]
    u0a = e[0xA]; u0b = e[0xB]
    st, ft, fo, sz, sz2, perm = struct.unpack_from('<6I', e, 0xC)
    cx, cy, rad = struct.unpack_from('<3f', e, 0x24)
    h = struct.unpack_from('<I', e, 0x30)[0]
    rest = struct.unpack_from('<7I', e, 0x34)
    secs.append(dict(name=nm, num=num, st=st, ft=ft, off=fo, size=sz, size2=sz2, key=perm, cx=cx, cy=cy, rad=rad, h=h, rest=rest))
mode = sys.argv[2] if len(sys.argv) > 2 else 'summary'

for cid in (0x34111, 0x34112, 0x34113):
    if cid in top:
        o, s = top[cid][0]
        print('%08X size %d' % (cid, s))
        if cid == 0x34111:
            print('  words:', struct.unpack_from('<%dI' % min(s // 4, 32), data, o))
        if cid == 0x34112:
            for k in range(min(s // 16, 8)):
                print('  barrier', struct.unpack_from('<4f', data, o + k * 16))
            print('  barriers total', s // 16)
        if cid == 0x34113:
            p = o; cnt = 0
            while p < o + s:
                hdr = data[p:p + 0x14]
                c = hdr[0x13]
                items = [struct.unpack_from('<hhI', data, p + 0x14 + 8 * j) for j in range(c)]
                if cnt < 10:
                    print('  group', hdr[:0x13].hex(), 'size', struct.unpack_from('<I', hdr, 4)[0], [(name(a), b) for a, b, _ in items])
                cnt += 1
                p += 0x14 + 8 * c
            print('  groups total', cnt)

# preculler
o, s = top[0x80034150][0]
base = None
outlines = []; lists = []; c54 = []; c55 = []
for cid, so, ss in walk(data, o, o + s):
    if cid == 0x34151:
        base = struct.unpack_from('<I', data, so)[0]
        print('34151 size', ss, 'words', struct.unpack_from('<%dI' % (ss // 4), data, so))
    elif cid == 0x34152:
        p = so
        while p < so + ss:
            # list node header 8 bytes (prev/next) then fields; game reads +8 short id, +0xA short count, size count*8+0x24
            sid, cnt = struct.unpack_from('<hh', data, p + 8)
            outlines.append((sid, cnt))
            p += cnt * 8 + 0x24
    elif cid == 0x34153:
        for k in range(ss // 0xA4):
            p = so + k * 0xA4
            sid = struct.unpack_from('<h', data, p + 8)[0]
            cnt = struct.unpack_from('<i', data, p + 0x10)[0]
            lst = struct.unpack_from('<%dh' % cnt, data, p + 0x14)
            lists.append((sid, lst))
    elif cid == 0x34154:
        for k in range(ss // 32):
            p = so + k * 32
            c54.append(data[p:p + 32])
    elif cid == 0x34155:
        for k in range(ss // 76):
            p = so + k * 76
            c55.append(data[p:p + 76])
    else:
        print('other preculler chunk %08X size %d' % (cid, ss))
print('base', base, 'outlines', len(outlines), 'lists', len(lists), '34154', len(c54), '34155', len(c55))
for e in c54:
    tid, cnt = struct.unpack_from('<ii', e, 8)
    print('  34154 track', tid, [name(x) for x in struct.unpack_from('<%dh' % min(cnt, 8), e, 0x10)])
for e in c55:
    cnt = struct.unpack_from('<h', e, 0x18)[0]
    print('  34155', e[8:0x18].hex(), [name(x) for x in struct.unpack_from('<%dh' % min(cnt, 25), e, 0x1A)])

bynum = {s['num']: s for s in secs}
if mode == 'secs':
    for s in secs:
        print('%-5s %5d st=%d ft=%d off=%9d size=%9d size2=%9d key=%6d c=(%.0f,%.0f) r=%.0f rest=%s' % (
            s['name'], s['num'], s['st'], s['ft'], s['off'], s['size'], s['size2'], s['key'], s['cx'], s['cy'], s['rad'], s['rest']))
# classify
cls = collections.Counter()
for s in secs:
    n = s['num']; L = chr(0x40 + n // 100) if n > 0 else '-'; m = n % 100
    if 'A' <= L <= 'T':
        k = 'drv<base' if m < base else ('geom[base,2b)' if m < 2 * base else 'A-T>=2b')
    else:
        k = L + ('<50' if m < 50 else '>=50')
    cls[k] += 1
print('classes', dict(cls))

if mode == 'lists':
    pool = 60000 * 1024
    for sid, lst in lists:
        tot = sum(bynum[x]['size'] for x in lst if x in bynum)
        missing = [name(x) for x in lst if x not in bynum]
        print('zone %-4s n=%3d total=%6.1f MiB %s%s' % (name(sid), len(lst), tot / 1048576.0, ' '.join(name(x) for x in lst), (' MISSING ' + str(missing)) if missing else ''))
if mode == 'stats':
    always = [2400, 2500, 2600]
    pool = 60000 * 1024
    rows = []
    for sid, lst in lists:
        full = set(lst) | set(always)
        tot = sum(bynum[x]['size'] for x in full if x in bynum)
        rows.append((tot, name(sid), len(full)))
    rows.sort()
    print('min', rows[0], 'max', rows[-1])
    print('pool bytes', pool, 'zones over pool:', [(r[1], round(r[0] / 1048576.0, 1)) for r in rows if r[0] > pool])
    print('largest 8:', [(r[1], round(r[0] / 1048576.0, 1), r[2]) for r in rows[-8:]])
    # sections never listed
    listed = set()
    for sid, lst in lists:
        listed |= set(lst)
    never = [s['name'] for s in secs if s['num'] not in listed and s['num'] not in always and s['num'] > 0]
    print('never listed (%d):' % len(never), ' '.join(never))
    lib = [s for s in secs if s['num'] > 0 and chr(0x40 + s['num'] // 100) in 'XYZ']
    print('library total MiB', round(sum(s['size'] for s in lib) / 1048576.0, 1))
