"""NFSU2 TRACKS/ROUTES<region>/Paths*.bin: the 0x80034147 "track path" container.

SPEED2.EXE loader 0x5D9A90 takes these children (any order, by id):

    0x34148  race path nodes        24 B each   (race files only)
    0x34149  race path segments    220 B each   (race files only)
    0x3414A  zones                 variable     (0x44 + 8 * corners)
    0x3414C  race outlines         272 B each   (race files only)
    0x3414D  barrier lines          36 B each

PathsFreeRoam.bin has only 0x3414A and 0x3414D: it is not a road graph. The
roads that traffic, AI and the GPS use are in Routes*.bin (nfsroutes.py).
`PathFile.to_bytes()` rebuilds the file byte for byte.
"""
import struct

CONTAINER = 0x80034147
ZONES, BARRIERS, RACE_NODES, RACE_SEGMENTS, RACE_OUTLINES = 0x3414A, 0x3414D, 0x34148, 0x34149, 0x3414C
RECORD = {RACE_NODES: 24, RACE_SEGMENTS: 220, RACE_OUTLINES: 272, BARRIERS: 36}

ZONE_TYPES = ['NO_WRONG_WAY', 'WRONG_WAY', 'RESET', 'RESET_TO_POINT', 'GUIDED_RESET', 'TUNNEL', 'OVERPASS',
              'OVERPASS_SMALL', 'DRIFT_GATE', 'DRIFT_HOT_SPOT', 'TUTORIAL', 'SHORTCUT', 'STREAMER_PREDICTION',
              'RACE_START', 'SHOP', 'SHOWCASE', 'TRAFFIC_TYPE', 'NEIGHBOURHOOD', 'SMS', 'BANK']   # SPEED2 0x800D80


class Zone:
    FMT = '<I2f2ff2I4fI3iHH'          # 0x44 bytes, then `corners` x float2

    def __init__(self, raw):
        (self.kind, self.x, self.y, self.fx, self.fy, self.height, self.u18, self.u1C,
         bx0, by0, bx1, by1, self.hash, gx, gy, gz, self.corners, self.size) = struct.unpack_from(self.FMT, raw)
        self.bbox = [bx0, by0, bx1, by1]
        self.gps = [gx, gy, gz]
        self.points = [struct.unpack_from('<2f', raw, 0x44 + 8 * i) for i in range(self.corners)]

    def to_bytes(self):
        return struct.pack(self.FMT, self.kind, self.x, self.y, self.fx, self.fy, self.height, self.u18, self.u1C,
                           *self.bbox, self.hash, *self.gps, self.corners, self.size) + \
            b''.join(struct.pack('<2f', *p) for p in self.points)

    @property
    def type_name(self):
        return ZONE_TYPES[self.kind] if self.kind < len(ZONE_TYPES) else str(self.kind)


class Barrier:
    """A 2D wall segment switched on by race barrier groups (L4RA.BUN 0x3412F hashes)."""
    FMT = '<4fBBBB4I'

    def __init__(self, raw):
        x0, y0, x1, y1, self.u10, self.count, self.u12, self.u13, *h = struct.unpack(self.FMT, raw)
        self.p0, self.p1 = (x0, y0), (x1, y1)
        self.hashes = list(h)

    def to_bytes(self):
        return struct.pack(self.FMT, *self.p0, *self.p1, self.u10, self.count, self.u12, self.u13, *self.hashes)


class PathFile:
    def __init__(self, buf):
        cid, size = struct.unpack_from('<II', buf, 0)
        if cid != CONTAINER or 8 + size != len(buf):
            raise ValueError('not a 0x80034147 file')
        self.order, self.raw, self.zones, self.barriers = [], {}, [], []
        off = 8
        while off < len(buf):
            cid, size = struct.unpack_from('<II', buf, off)
            data = buf[off + 8:off + 8 + size]
            if len(data) != size:
                raise ValueError('child %X truncated' % cid)
            self.order.append(cid)
            if cid == ZONES:
                p = 0
                while p < size:
                    corners, rec = struct.unpack_from('<HH', data, p + 0x40)
                    if rec != 0x44 + 8 * corners:
                        raise ValueError('zone at %X: size %d for %d corners' % (p, rec, corners))
                    self.zones.append(Zone(data[p:p + rec]))
                    p += rec
                if p != size:
                    raise ValueError('zones do not fill 0x3414A')
            elif cid == BARRIERS:
                if size % 36:
                    raise ValueError('0x3414D size')
                self.barriers = [Barrier(data[36 * i:36 * (i + 1)]) for i in range(size // 36)]
            else:
                if cid in RECORD and size % RECORD[cid]:
                    raise ValueError('child %X size %d not a multiple of %d' % (cid, size, RECORD[cid]))
                self.raw[cid] = data
            off += 8 + size

    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            return cls(f.read())

    def to_bytes(self):
        body = bytearray()
        for cid in self.order:
            if cid == ZONES:
                data = b''.join(z.to_bytes() for z in self.zones)
            elif cid == BARRIERS:
                data = b''.join(b.to_bytes() for b in self.barriers)
            else:
                data = self.raw[cid]
            body += struct.pack('<II', cid, len(data)) + data
        return struct.pack('<II', CONTAINER, len(body)) + bytes(body)

    def check(self):
        bad = []
        for i, b in enumerate(self.barriers):
            if not 1 <= b.count <= 4 or any(b.hashes[b.count:]):
                bad.append('barrier %d hash count %d' % (i, b.count))
        for i, z in enumerate(self.zones):
            xs = [p[0] for p in z.points]
            ys = [p[1] for p in z.points]
            if xs and max(abs(a - b) for a, b in zip(z.bbox, (min(xs), min(ys), max(xs), max(ys)))) > 0.01:
                bad.append('zone %d bbox' % i)
        return bad
