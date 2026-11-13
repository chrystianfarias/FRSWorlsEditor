"""Adds a test road between two existing roads, the way an editor would.

    python tools/paths/demo_add_road.py --from -1290 2030 --to -1270 1960 --out <dir>

Snaps each end to the nearest route that has lanes (or --a/--b route ids),
splits that route there if no free node is close (insert_node renumbers every
reference), builds a straight two-way two-lane route between the two points
and links both ends with T-junction signposts (4 per junction). Then it checks
the result, rebuilds it, parses it again and draws the area.

Never writes into the game folder. Not tested in game.
"""
import argparse
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nfsroutes as nr

GAME = 'F:/Games/NFSU2'


def nearest_route(net, x, y, need_lanes=True):
    best = None
    for r in net.routes:
        if need_lanes and not (any(r.lane_present[:4]) and any(r.lane_present[4:])):
            continue
        k, t, d = nr.project_on_route(r, x, y)
        if best is None or d < best[1]:
            best = (r.id, d)
    return best[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=GAME)
    ap.add_argument('--region', default='L4RA')
    ap.add_argument('--from', dest='p0', nargs=2, type=float, required=True)
    ap.add_argument('--to', dest='p1', nargs=2, type=float, required=True)
    ap.add_argument('--a', type=int, help='route id for the first end')
    ap.add_argument('--b', type=int, help='route id for the second end')
    ap.add_argument('--spacing', type=float, default=30.0)
    ap.add_argument('--out', required=True, help='folder for RoutesFreeRoam.bin and a PNG')
    a = ap.parse_args()
    if os.path.abspath(a.out).lower().startswith(os.path.abspath(a.game).lower()):
        sys.exit('refusing to write into the game folder')
    src = os.path.join(a.game, 'TRACKS', 'ROUTES' + a.region, 'RoutesFreeRoam.bin')
    net = nr.Network.load(src)
    before = (len(net.routes), len(net.signposts))

    ra = a.a or nearest_route(net, *a.p0)
    rb = a.b or nearest_route(net, *a.p1)
    ka = nr.junction_node(net, ra, *a.p0)
    kb = nr.junction_node(net, rb, *a.p1)
    na, nb = net.node(ra, ka), net.node(rb, kb)
    length = math.hypot(nb.x - na.x, nb.y - na.y)
    steps = max(1, int(math.ceil(length / a.spacing)))
    pts = [(na.x + (nb.x - na.x) * i / steps, na.y + (nb.y - na.y) * i / steps,
            (na.z + (nb.z - na.z) * i / steps)) for i in range(steps + 1)]
    rid = nr.free_route_id(net)
    road = nr.make_route(rid, pts, half_left=6.0, half_right=6.0, fwd_lanes=[3.0], back_lanes=[3.0])
    nr.add_route(net, road)
    nr.connect_end_to_route(net, rid, 'start', ra, ka)
    nr.connect_end_to_route(net, rid, 'end', rb, kb)

    bad = net.check()
    out = os.path.join(a.out, 'RoutesFreeRoam.bin')
    os.makedirs(a.out, exist_ok=True)
    net.save(out)
    again = nr.Network.load(out)
    same = again.to_bytes() == open(out, 'rb').read()
    print('route %d: %d nodes, %.1f m, from route %d node %d to route %d node %d' % (
        rid, road.count, road.length, ra, ka, rb, kb))
    print('routes %d -> %d, signposts %d -> %d, file %d -> %d bytes' % (
        before[0], len(net.routes), before[1], len(net.signposts), os.path.getsize(src), os.path.getsize(out)))
    print('checks: %s; reparse identical: %s' % ('ok' if not bad else bad[:5], same))
    x0, x1 = sorted((na.x, nb.x))
    y0, y1 = sorted((na.y, nb.y))
    m = max(x1 - x0, y1 - y0) * 0.4 + 40
    subprocess.call([sys.executable, os.path.join(HERE, 'render_paths.py'), '--routes', out, '--lanes',
                     '--zoom', str(x0 - m), str(y0 - m), str(x1 + m), str(y1 + m), '--size', '1200',
                     '--highlight', str(rid), '--out', os.path.join(a.out, 'new-road.png')])
    return 0 if not bad and same else 1


if __name__ == '__main__':
    sys.exit(main())
