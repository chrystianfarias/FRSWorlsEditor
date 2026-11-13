"""NFSU2 road network: TRACKS/ROUTES<region>/Routes*.bin.

This is the graph that traffic, AI racers and the free-roam GPS drive on
(SPEED2.EXE loader 0x5DEB80). It is NOT PathsFreeRoam.bin: that file only
holds zones (0x3414A) and race barrier lines (0x3414D), see nfspaths.py.

A Routes file is three top-level chunks, back to back, no padding:

    0x00034121  routes        n x (0x8C header + count x 0x38 node)
    0x00034122  signposts     u32 0, then n x 0x20   (links between routes)
    0x00034123  intersections n x 0x114              (traffic-light crossings)

Every byte of each record is mapped to a named field below, and
`Network.to_bytes()` rebuilds the file byte for byte (see validate_paths.py).
Layout and meaning: docs/research/road-paths.md.

The writer helpers at the bottom (make_route, add_signpost, insert_node,
connect_end_to_route) build new roads that follow the rules found in the
shipped data. They have not been tried in game yet.
"""
import math
import struct

ROUTES, SIGNPOSTS, INTERSECTIONS, EMITTERS = 0x34121, 0x34122, 0x34123, 0x34124
HDR, NODE, SIGN, INTER = 0x8C, 0x38, 0x20, 0x114

NO_SLOT = 0xFFFF
NO_INTERSECTION = 0x3F
MAX_ROUTE_ID = 1499          # route table at 0x88A334 has 1500 slots (loops to 0x5DC)
MAX_SIGNPOSTS = 4000         # load-time remap array on the stack (0x5E2BD0, 0x1F48 frame)
MAX_INTERSECTIONS = 63       # 6-bit field in the node flags, 0x3F = none

# node flags (+0x0C): permission bits per AI mode and travel direction (0x5DD770)
#   mode 0 (default: player/GPS, A nibble)  fwd bit 11, back bit 18
#   mode 1 (traffic, B nibble, needs lanes) fwd bit 25, back bit 26
#   mode 2 (A nibble, only A in {2, 8})     fwd bit 27, back bit 28
PERM = {(0, 0): 1 << 11, (0, 1): 1 << 18, (1, 0): 1 << 25, (1, 1): 1 << 26, (2, 0): 1 << 27, (2, 1): 1 << 28}
PERM_ALL = sum(PERM.values())
PERM_NO_TRAFFIC = PERM_ALL & ~(PERM[(1, 0)] | PERM[(1, 1)])
BIT12 = 1 << 12              # set on 95% of nodes, no reader found


def chunks(buf):
    """Top-level chunks: [(id, data_offset, size)]."""
    out, off = [], 0
    while off + 8 <= len(buf):
        cid, size = struct.unpack_from('<II', buf, off)
        if off + 8 + size > len(buf):
            raise ValueError('chunk %08X at %X runs past the end' % (cid, off))
        out.append((cid, off + 8, size))
        off += 8 + size
    if off != len(buf):
        raise ValueError('%d stray bytes at the end' % (len(buf) - off))
    return out


class Node:
    """0x38 bytes. Units: world metres, Z up; lateral offsets relative to the
    node's own direction, right = (dy, -dx)."""
    FMT = '<3fIHhHHhhHHbbbb8s10s2s'

    def __init__(self, raw=None):
        if raw is None:
            raw = bytes(NODE)
        (self.x, self.y, self.race_dist, self.flags, self.seg_len256, self.height8,
         self.half_left256, self.half_right256, self.dir_x, self.dir_y,
         slot0, slot1, self.cross_slope, self.slope, line_f, line_b,
         lanes, speeds, self.pad) = struct.unpack(self.FMT, raw)
        self.slots = [slot0, slot1]
        self.racing_line = [line_f, line_b]
        self.lanes = list(struct.unpack('8b', lanes))      # -1 = no lane
        self.speeds = list(speeds)                          # [0],[1] racing line, [2..9] lanes

    def to_bytes(self):
        return struct.pack(self.FMT, self.x, self.y, self.race_dist, self.flags, self.seg_len256,
                           self.height8, self.half_left256, self.half_right256, self.dir_x, self.dir_y,
                           self.slots[0], self.slots[1], self.cross_slope, self.slope,
                           self.racing_line[0], self.racing_line[1],
                           struct.pack('8b', *self.lanes), bytes(self.speeds), self.pad)

    # decoded views
    route_id = property(lambda s: s.flags & 0x7FF)
    intersection = property(lambda s: s.flags >> 19 & 0x3F)
    z = property(lambda s: s.height8 / 8.0)
    half_left = property(lambda s: s.half_left256 / 256.0)
    half_right = property(lambda s: s.half_right256 / 256.0)
    seg_len = property(lambda s: s.seg_len256 / 256.0)

    def allowed(self, mode, backward):
        return bool(self.flags & PERM[(mode, int(backward))])

    def lane_offset(self, slot):
        """Lateral offset in metres of lane slot 0..7 (0..3 forward, 4..7 backward), or None.
        Positive = right of the direction of travel for that slot (SPEED2 0x401C40)."""
        v = self.lanes[slot]
        if v == -1:
            return None
        return v / 128.0 * max(self.half_left, self.half_right)

    def lane_world(self, slot):
        off = self.lane_offset(slot)
        if off is None:
            return None
        dx, dy = self.dir_x / 32767.0, self.dir_y / 32767.0
        if slot >= 4:                       # backward lanes: right of the reversed direction
            dx, dy = -dx, -dy
        return (self.x + dy * off, self.y - dx * off)


class Route:
    """0x8C-byte header followed by its nodes."""
    FMT = '<IIHHI32sfhhIIf4H8sf8s4fIIfIf8s'

    def __init__(self, raw=None):
        if raw is None:
            raw = default_route_header()
        (self.link_next, self.link_prev, self.one, self.id, self.kind, name, self.unk30,
         self.count, self.count2, self.rt38, self.rt3C, self.length,
         e0, e1, e2, e3, self.unk4C, self.layer_z, self.zero58,
         bx0, by0, bx1, by1, self.flags, self.unk74, self.unk78, self.unk7C, self.unk80,
         lane_present) = struct.unpack(self.FMT, raw[:HDR])
        self.name = name
        self.excluded_events = [e0, e1, e2, e3]
        self.bbox = [bx0, by0, bx1, by1]
        self.lane_present = list(lane_present)
        self.nodes = []

    def to_bytes(self):
        hdr = struct.pack(self.FMT, self.link_next, self.link_prev, self.one, self.id, self.kind,
                          self.name, self.unk30, self.count, self.count2, self.rt38, self.rt3C,
                          self.length, *self.excluded_events, self.unk4C, self.layer_z, self.zero58,
                          *self.bbox, self.flags, self.unk74, self.unk78, self.unk7C, self.unk80,
                          bytes(self.lane_present))
        return hdr + b''.join(n.to_bytes() for n in self.nodes)

    @property
    def name_str(self):
        return self.name.split(b'\0')[0].decode('latin1')


class Signpost:
    """0x20 bytes: a one-way link from (src route, src node) to (dst route, dst node)."""
    FMT = '<fHHHHIiI4H'

    def __init__(self, raw=None):
        if raw is None:
            raw = struct.pack(self.FMT, 0, 0, 0, 0, 0, 0, -1, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF)
        (self.runtime_dist, self.dst_node, self.src_node, self.dst_route, self.src_route,
         self.flags, self.unk10, self.unk14, *ext) = struct.unpack(self.FMT, raw)
        self.ext = list(ext)        # A=4 signposts: (route a, route b, node on a, node on b)

    def to_bytes(self):
        return struct.pack(self.FMT, self.runtime_dist, self.dst_node, self.src_node, self.dst_route,
                           self.src_route, self.flags, self.unk10, self.unk14, *self.ext)

    a = property(lambda s: s.flags & 0xF)                 # used by modes 0 and 2
    b = property(lambda s: s.flags >> 4 & 0xF)            # used by mode 1 (traffic)
    a_authored = property(lambda s: s.flags >> 8 & 0xF)   # copied to A at load (0x5D98C0)
    b_authored = property(lambda s: s.flags >> 12 & 0xF)
    backward = property(lambda s: bool(s.flags >> 18 & 1))  # valid when travelling the src route backwards
    flip = property(lambda s: bool(s.flags >> 23 & 1))      # travel direction toggles on the dst route
    nav_bits = property(lambda s: s.flags >> 24 & 0xF)       # read by the navigation code, meaning unknown


def signpost_flags(a, b, backward, flip, nav=0):
    return (a | b << 4 | a << 8 | b << 12 | int(backward) << 18 | int(flip) << 23 | nav << 24)


class Intersection:
    """0x114 bytes: a crossing of two routes (node flags bits 19..24 point here)."""
    FMT = '<I3H3Hi'

    def __init__(self, raw=None):
        if raw is None:
            raw = struct.pack(self.FMT, 1, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, -1) + \
                  bytes.fromhex('00000000ffffffff000080bf000080bf') * 16
        (self.kind, r0, r1, r2, n0, n1, n2, self.unk10) = struct.unpack_from(self.FMT, raw)
        self.routes = [r0, r1, r2]
        self.nodes = [n0, n1, n2]
        self.state = raw[0x14:INTER]       # 16 x {u32 0, i32 -1, f32 -1, f32 -1}: runtime

    def to_bytes(self):
        return struct.pack(self.FMT, self.kind, *self.routes, *self.nodes, self.unk10) + self.state


class Network:
    def __init__(self, buf):
        self.order = []
        self.routes, self.signposts, self.intersections = [], [], []
        self.sign_header = b'\0\0\0\0'
        self.extra = {}
        for cid, off, size in chunks(buf):
            self.order.append(cid)
            data = buf[off:off + size]
            if cid == ROUTES:
                p = 0
                while p < size:
                    r = Route(data[p:p + HDR])
                    p += HDR
                    for k in range(r.count):
                        r.nodes.append(Node(data[p:p + NODE]))
                        p += NODE
                    self.routes.append(r)
                if p != size:
                    raise ValueError('0x34121 does not end on a record')
            elif cid == SIGNPOSTS:
                if (size - 4) % SIGN:
                    raise ValueError('0x34122 size %d is not 4 + 32n' % size)
                self.sign_header = data[:4]
                self.signposts = [Signpost(data[4 + SIGN * i:4 + SIGN * (i + 1)]) for i in range((size - 4) // SIGN)]
            elif cid == INTERSECTIONS:
                if size % INTER:
                    raise ValueError('0x34123 size %d is not 276n' % size)
                self.intersections = [Intersection(data[INTER * i:INTER * (i + 1)]) for i in range(size // INTER)]
            else:
                self.extra[cid] = data
        self.by_id = {r.id: r for r in self.routes}

    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            return cls(f.read())

    def to_bytes(self):
        out = bytearray()
        for cid in self.order:
            if cid == ROUTES:
                data = b''.join(r.to_bytes() for r in self.routes)
            elif cid == SIGNPOSTS:
                data = self.sign_header + b''.join(s.to_bytes() for s in self.signposts)
            elif cid == INTERSECTIONS:
                data = b''.join(i.to_bytes() for i in self.intersections)
            else:
                data = self.extra[cid]
            out += struct.pack('<II', cid, len(data)) + data
        return bytes(out)

    def save(self, path):
        with open(path, 'wb') as f:
            f.write(self.to_bytes())

    def node(self, route_id, index):
        return self.by_id[route_id].nodes[index]

    # ---------------------------------------------------------------- checks
    def check(self, strict=True):
        """Every cross-reference the shipped files satisfy. Returns a list of problems.

        strict=False skips what the race files (Routes<n>F/B.bin) break and the game
        tolerates: signposts and intersections that name routes the file leaves out
        (the race files keep the full free-roam signpost/intersection tables), and a
        working A/B nibble that differs from the authored copy (rewritten at load)."""
        bad = []
        ids = [r.id for r in self.routes]
        if ids != sorted(ids) or len(set(ids)) != len(ids):
            bad.append('route ids not sorted/unique')
        nsp = len(self.signposts)
        if nsp > MAX_SIGNPOSTS:
            bad.append('%d signposts > %d' % (nsp, MAX_SIGNPOSTS))
        if len(self.intersections) > MAX_INTERSECTIONS - 1:
            bad.append('too many intersections')
        for r in self.routes:
            if not 0 < r.id <= MAX_ROUTE_ID:
                bad.append('route id %d out of range' % r.id)
            if r.count != len(r.nodes) or r.count2 != r.count or r.count < 2:
                bad.append('route %d count %d/%d' % (r.id, r.count, r.count2))
            total = 0.0
            for k, n in enumerate(r.nodes):
                if n.route_id != r.id:
                    bad.append('route %d node %d has route id %d' % (r.id, k, n.route_id))
                total += n.seg_len256
                if k + 1 < len(r.nodes):
                    m = r.nodes[k + 1]
                    if abs(n.seg_len256 / 256 - math.hypot(m.x - n.x, m.y - n.y)) > 0.01:
                        bad.append('route %d node %d segment length' % (r.id, k))
                elif n.seg_len256 != 1280:
                    bad.append('route %d last node length %d != 1280' % (r.id, n.seg_len256))
                ix = n.intersection
                if ix != NO_INTERSECTION and ix >= len(self.intersections):
                    bad.append('route %d node %d intersection %d' % (r.id, k, ix))
                if n.slots[0] == NO_SLOT and n.slots[1] != NO_SLOT:
                    bad.append('route %d node %d uses slot 2 without slot 1' % (r.id, k))
                for s in n.slots:
                    if s != NO_SLOT and s < nsp:
                        sp = self.signposts[s]
                        if (sp.src_route, sp.src_node) != (r.id, k):
                            bad.append('route %d node %d slot -> signpost %d from %d/%d' % (r.id, k, s, sp.src_route, sp.src_node))
            if abs(total / 256 - 5.0 - r.length) > 0.05:
                bad.append('route %d length %.3f != %.3f' % (r.id, r.length, total / 256 - 5))
            bb = route_bbox(r)
            if max(abs(a - b) for a, b in zip(bb, r.bbox)) > 0.01:
                bad.append('route %d bbox' % r.id)
            for j in range(8):
                if bool(r.lane_present[j]) != any(n.lanes[j] != -1 for n in r.nodes):
                    bad.append('route %d lane flag %d' % (r.id, j))
        owner = {}
        for r in self.routes:
            for k, n in enumerate(r.nodes):
                for s in n.slots:
                    if s != NO_SLOT and s < nsp:
                        owner.setdefault(s, []).append((r.id, k))
        for i, sp in enumerate(self.signposts):
            for rid, nd in ((sp.src_route, sp.src_node), (sp.dst_route, sp.dst_node)):
                if rid not in self.by_id:
                    if strict:
                        bad.append('signpost %d points at missing route %d' % (i, rid))
                elif nd >= self.by_id[rid].count:
                    bad.append('signpost %d points at %d/%d' % (i, rid, nd))
            if len(owner.get(i, [])) != (1 if sp.src_route in self.by_id else 0):
                bad.append('signpost %d is in %d node slots' % (i, len(owner.get(i, []))))
            if strict and (sp.a != sp.a_authored or sp.b != sp.b_authored):
                bad.append('signpost %d A/B differ from the authored copy' % i)
            if sp.ext[0] != 0xFFFF:
                ra, rb, na, nb = sp.ext
                if ra not in self.by_id or rb not in self.by_id:
                    if strict:
                        bad.append('signpost %d extra link to missing route %s' % (i, sp.ext))
                elif na >= self.by_id[ra].count or nb >= self.by_id[rb].count:
                    bad.append('signpost %d extra link %s' % (i, sp.ext))
        for i, it in enumerate(self.intersections):
            for rid, nd in zip(it.routes, it.nodes):
                if rid == 0xFFFF or (not strict and rid not in self.by_id):
                    continue
                if rid not in self.by_id or nd >= self.by_id[rid].count or self.by_id[rid].nodes[nd].intersection != i:
                    bad.append('intersection %d -> %d/%d not marked' % (i, rid, nd))
        return bad


# ------------------------------------------------------------------ geometry
def route_bbox(r):
    """Header +0x60: the box of both road edges (exact on all shipped routes)."""
    xs, ys = [], []
    for n in r.nodes:
        dx, dy = n.dir_x / 32767.0, n.dir_y / 32767.0
        xs += [n.x + dy * n.half_right, n.x - dy * n.half_left]
        ys += [n.y - dx * n.half_right, n.y + dx * n.half_left]
    return [min(xs), min(ys), max(xs), max(ys)]


def default_route_header():
    """A header with the constants every shipped route carries."""
    return struct.pack(Route.FMT, 0xB, 0xB, 1, 0, 0, b'TrackRoutesA32'.ljust(32, b'\0'), 1.0, 0, 0, 0, 0,
                       0.0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, b'\xff' * 8, 0.0, bytes(8),
                       0.0, 0.0, 0.0, 0.0, 0x007FE141, 0x100, 999.0, 0x100, 999.0, bytes(8))


def _i8(v):
    v = int(round(v))
    return max(-128, min(127, v))


def finish_route(r):
    """Recompute everything derived from node positions/heights/widths/lanes: segment
    lengths, directions, slopes, header length, count, bbox and lane-present flags."""
    ns = r.nodes
    r.count = r.count2 = len(ns)
    for k, n in enumerate(ns):
        n.flags = (n.flags & ~0x7FF) | r.id
        if k + 1 < len(ns):
            m = ns[k + 1]
            dx, dy = m.x - n.x, m.y - n.y
            d = math.hypot(dx, dy)
            if d < 0.05:
                raise ValueError('route %d nodes %d/%d coincide' % (r.id, k, k + 1))
            n.seg_len256 = int(round(d * 256))
            n.dir_x, n.dir_y = int(round(dx / d * 32767)), int(round(dy / d * 32767))
            n.slope = _i8(128.0 * (m.height8 - n.height8) / 8.0 / d)
        else:
            n.seg_len256 = 1280                       # every shipped route ends with 5.0 m
            n.dir_x, n.dir_y = ns[k - 1].dir_x, ns[k - 1].dir_y
            n.slope = ns[k - 1].slope
    r.length = sum(n.seg_len256 for n in ns[:-1]) / 256.0
    r.bbox = route_bbox(r)
    r.lane_present = [int(any(n.lanes[j] != -1 for n in ns)) for j in range(8)]


def lane_byte(offset, max_half):
    """Lane offset (metres, positive = right of travel) to the i8 the node stores."""
    v = _i8(offset / max_half * 128.0)
    return -2 if v == -1 else v          # -1 means "no lane"


def make_route(route_id, points, half_left, half_right, fwd_lanes=(), back_lanes=(),
               speed=20, traffic=True, name=b'TrackRoutesA32', layer_z=0.0):
    """A new route along `points` [(x, y, z)], node 0 first.

    fwd_lanes / back_lanes: up to 4 lateral offsets each (metres, positive = right of
    the direction of travel; back lanes are measured in the reversed direction, so a
    normal two-way road has positive values in both lists). speed: lane speed byte
    (m/s, 6..20 in the shipped data; it is lowered on bends here like the shipped data).
    """
    if len(points) < 2:
        raise ValueError('a route needs 2+ nodes')
    if len(fwd_lanes) > 4 or len(back_lanes) > 4:
        raise ValueError('4 lanes per direction at most')
    r = Route()
    r.id = route_id
    r.name = name.ljust(32, b'\0')
    r.layer_z = layer_z
    m = max(half_left, half_right)
    perm = PERM_ALL if traffic and (fwd_lanes or back_lanes) else PERM_NO_TRAFFIC
    for x, y, z in points:
        n = Node()
        n.x, n.y, n.race_dist = float(x), float(y), 0.0
        n.height8 = int(round(z * 8))
        n.half_left256, n.half_right256 = int(round(half_left * 256)), int(round(half_right * 256))
        n.flags = route_id | BIT12 | NO_INTERSECTION << 19 | perm
        n.slots = [NO_SLOT, NO_SLOT]
        n.lanes = [-1] * 8
        for j, off in enumerate(fwd_lanes):
            n.lanes[j] = lane_byte(off, m)
        for j, off in enumerate(back_lanes):
            n.lanes[4 + j] = lane_byte(off, m)
        n.speeds = [100, 100] + [6] * 8
        r.nodes.append(n)
    finish_route(r)
    # lane speed: 20 m/s on straights, lower on bends (fit of the shipped data: ~-4.5 per deg/m)
    for k, n in enumerate(r.nodes):
        turn = 0.0
        if 0 < k < len(r.nodes) - 1:
            a0 = math.atan2(r.nodes[k - 1].dir_y, r.nodes[k - 1].dir_x)
            a1 = math.atan2(n.dir_y, n.dir_x)
            turn = abs((a1 - a0 + math.pi) % (2 * math.pi) - math.pi) * 180 / math.pi / max(n.seg_len, 1.0)
        v = max(6, min(speed, int(round(speed - 4.5 * turn))))
        for j in range(8):
            if n.lanes[j] != -1:
                n.speeds[2 + j] = v
    return r


def free_route_id(net, start=101):
    used = set(net.by_id)
    for i in range(start, MAX_ROUTE_ID + 1):
        if i not in used:
            return i
    raise ValueError('no free route id below 1500')


def add_route(net, r):
    if r.id in net.by_id:
        raise ValueError('route id %d already used' % r.id)
    net.routes.append(r)
    net.routes.sort(key=lambda q: q.id)
    net.by_id[r.id] = r
    return r


def drop_dangling_slots(net):
    """Clear node slots that hold an index past the end of the signpost table.

    RoutesFreeRoam.bin (L4RA) has 232 of them (values 3290..3752 with 3289
    signposts). The game skips them only because they are >= the count; as soon
    as signposts are appended they would start pointing at the new ones. Valid
    slots are kept first, as the game counts slots from +0x1C."""
    count = len(net.signposts)
    fixed = 0
    for r in net.routes:
        for n in r.nodes:
            keep = [s for s in n.slots if s != NO_SLOT and s < count]
            new = keep + [NO_SLOT] * (2 - len(keep))
            if new != n.slots:
                n.slots = new
                fixed += 1
    return fixed


def add_signpost(net, src_route, src_node, dst_route, dst_node, backward, flip, a=2, b=2):
    """Append a signpost and hook it into the source node's first free slot.
    backward: valid while travelling the source route towards lower node indices.
    flip: travel direction on the destination is the opposite of `backward`."""
    drop_dangling_slots(net)
    n = net.node(src_route, src_node)
    if NO_SLOT not in n.slots:
        raise ValueError('route %d node %d already has 2 signposts' % (src_route, src_node))
    if len(net.signposts) >= MAX_SIGNPOSTS:
        raise ValueError('signpost limit reached')
    sp = Signpost()
    sp.src_route, sp.src_node, sp.dst_route, sp.dst_node = src_route, src_node, dst_route, dst_node
    sp.flags = signpost_flags(a, b, backward, flip)
    net.signposts.append(sp)
    n.slots[n.slots.index(NO_SLOT)] = len(net.signposts) - 1
    return sp


def insert_node(net, route_id, index, x, y, z=None):
    """Insert a node before `index` (1..count-1) of a route and renumber every
    reference to that route's later nodes. Width, lanes, speeds and flags are copied
    from the previous node; the slots start empty."""
    r = net.by_id[route_id]
    if not 0 < index < r.count:
        raise ValueError('insert inside the route only')
    prev, nxt = r.nodes[index - 1], r.nodes[index]
    n = Node(prev.to_bytes())
    n.x, n.y = float(x), float(y)
    if z is None:
        t = math.hypot(x - prev.x, y - prev.y) / max(prev.seg_len, 1e-6)
        n.height8 = int(round(prev.height8 + (nxt.height8 - prev.height8) * t))
    else:
        n.height8 = int(round(z * 8))
    n.slots = [NO_SLOT, NO_SLOT]
    n.flags = (n.flags & ~(0x3F << 19)) | NO_INTERSECTION << 19
    r.nodes.insert(index, n)
    for sp in net.signposts:
        if sp.src_route == route_id and sp.src_node >= index:
            sp.src_node += 1
        if sp.dst_route == route_id and sp.dst_node >= index:
            sp.dst_node += 1
        if sp.ext[0] != 0xFFFF:
            if sp.ext[0] == route_id and sp.ext[2] >= index:
                sp.ext[2] += 1
            if sp.ext[1] == route_id and sp.ext[3] >= index:
                sp.ext[3] += 1
    for it in net.intersections:
        for j in range(3):
            if it.routes[j] == route_id and it.nodes[j] >= index:
                it.nodes[j] += 1
    finish_route(r)
    return n


def project_on_route(r, x, y):
    """(segment index k, t, distance): closest point on segment k..k+1."""
    best = None
    for k in range(r.count - 1):
        a, b = r.nodes[k], r.nodes[k + 1]
        vx, vy = b.x - a.x, b.y - a.y
        L2 = vx * vx + vy * vy
        t = max(0.0, min(1.0, ((x - a.x) * vx + (y - a.y) * vy) / L2))
        d = math.hypot(a.x + vx * t - x, a.y + vy * t - y)
        if best is None or d < best[2]:
            best = (k, t, d)
    return best


def junction_node(net, route_id, x, y, snap=4.0):
    """Index of a node of `route_id` at (x, y) with two free slots: reuses a node
    within `snap` metres, otherwise splits the nearest segment there."""
    r = net.by_id[route_id]
    k, t, d = project_on_route(r, x, y)
    for i in (k, k + 1):
        n = r.nodes[i]
        if math.hypot(n.x - x, n.y - y) <= snap and n.slots == [NO_SLOT, NO_SLOT] and 0 < i < r.count - 1:
            return i
    a, b = r.nodes[k], r.nodes[k + 1]
    px, py = a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t
    if t < 0.05 or t > 0.95:          # too close to an existing node: nudge into the segment
        t = min(max(t, 0.25), 0.75)
        px, py = a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t
    insert_node(net, route_id, k + 1, px, py)
    return k + 1


def connect_end_to_route(net, new_id, end, old_id, old_node, traffic=True):
    """Two-way T junction between one end of a new route and a node of an old route.
    Uses both slots of the old node and both slots of the new end node:

        old node : forward on old  -> new end, entering the new route
                   backward on old -> new end, entering the new route
        new end  : leaving the new route -> old node, forward on old
                   leaving the new route -> old node, backward on old
    """
    new = net.by_id[new_id]
    k = 0 if end == 'start' else new.count - 1
    enter_backward = end == 'end'          # entering at the last node means driving it backwards
    leave_backward = end == 'start'        # we arrive at node 0 driving backwards
    b = 2 if traffic else 7
    for old_backward in (False, True):
        add_signpost(net, old_id, old_node, new_id, k, old_backward, old_backward != enter_backward, 2, b)
    for old_forward in (True, False):
        add_signpost(net, new_id, k, old_id, old_node, leave_backward, leave_backward != (not old_forward), 2, b)
