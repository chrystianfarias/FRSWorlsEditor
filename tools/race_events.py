"""Race event triggers: where the player drives in free roam to start a race.

GLOBALB.BUN career chunk 0x34A11 lists the races (136 bytes each). Each race
names an EV_xx trigger by hash. PathsFreeRoam.bin (0x3414A) stores each trigger
as a zone record, so a race's start icon is the centre of its zone.
"""
import re
import struct

GLOBALB = 'F:/Games/NFSU2/GLOBAL/GLOBALB.BUN'
FREEROAM = 'F:/Games/NFSU2/TRACKS/ROUTESL4RA/PathsFreeRoam.bin'


def bin_hash(s):
    h = 0xFFFFFFFF
    for c in s:
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def chunks(buf, off, end):
    while off + 8 <= end:
        cid, csz = struct.unpack_from('<II', buf, off)
        yield cid, off + 8, csz
        off += 8 + csz


def career():
    g = open(GLOBALB, 'rb').read()
    for cid, d, s in chunks(g, 0, len(g)):
        if cid == 0x80034A10:
            sub = {c: (dd, ss) for c, dd, ss in chunks(g, d, d + s)}
            return g, sub      # the first one is the full career
    raise RuntimeError('no career chunk')


def races():
    g, sub = career()
    sd, ss = sub[0x34A1D]
    table = g[sd:sd + ss]
    text = lambda o: table[o:table.find(b'\0', o)].decode('latin1')
    names = {bin_hash(m): m.decode() for m in re.findall(rb'[\x20-\x7e]+', table)}
    rd, rs = sub[0x34A11]
    out = []
    for i in range(rs // 136):
        w = struct.unpack_from('<34I', g, rd + i * 136)
        out.append(dict(name=text(w[0]), track=w[6] & 0xFFFF,
                        event=names.get(w[10], '%08X' % w[10]), event_hash=w[10]))
    return out


def triggers(hashes):
    """Returns {hash: (x, y, radius)} for the trigger zones found in free roam."""
    b = open(FREEROAM, 'rb').read()
    out = {}
    for h in hashes:
        at = b.find(struct.pack('<I', h))
        if at < 0:
            continue
        kind, x, y = struct.unpack_from('<Iff', b, at - 0x30)
        x0, y0, x1, y1 = struct.unpack_from('<4f', b, at - 0x10)
        if kind == 0x0D and x0 <= x <= x1 and y0 <= y <= y1:
            out[h] = (x, y, struct.unpack_from('<f', b, at - 0x20)[0])
    return out


if __name__ == '__main__':
    rs = races()
    tr = triggers({r['event_hash'] for r in rs})
    for r in rs:
        t = tr.get(r['event_hash'])
        print('%-22s %5d %-7s %s' % (r['name'], r['track'], r['event'],
                                     '%.0f %.0f' % t[:2] if t else '-'))
