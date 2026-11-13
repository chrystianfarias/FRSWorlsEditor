"""Draws the road network of a Routes*.bin as a PNG, to check it by eye.

    python tools/paths/render_paths.py --out docs/research/road-paths-L4RA.png
    python tools/paths/render_paths.py --map --out docs/research/road-paths-L4RA-map.png
    python tools/paths/render_paths.py --zoom 1100 -900 1700 -300 --lanes --out zoom.png

Colours: routes with lanes both ways (blue), lanes one way only (orange), no
lanes (grey: AI/GPS only). Signposts are drawn from source node to destination
node: green when traffic may take them (B = 2), purple otherwise. Red dots are
intersections. --map draws over the free-roam minimap TRACKMAP4000 using the
L4RA framing px = 0.0917 x + 312, py = -0.0917 y + 298.5.
"""
import argparse
import math
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import nfsroutes as nr
import nfspaths as npth

GAME = 'F:/Games/NFSU2'
BOTH, ONE, NONE = (60, 140, 255), (255, 150, 40), (150, 150, 150)


def route_colour(r):
    f, b = any(r.lane_present[:4]), any(r.lane_present[4:])
    return BOTH if f and b else ONE if f or b else NONE


def minimap(game):
    import dump_textures as dt
    tex, _ = dt.load(None, os.path.join(game, 'TRACKS', 'TRACKMAP4000.BIN'))
    t = next(iter(tex.values()))
    return Image.frombytes('RGB', (t['width'], t['height']), dt.to_rgb(t))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=GAME)
    ap.add_argument('--region', default='L4RA')
    ap.add_argument('--routes', help='a Routes*.bin (default RoutesFreeRoam.bin of the region)')
    ap.add_argument('--map', action='store_true', help='draw over TRACKMAP4000 (L4RA framing)')
    ap.add_argument('--zoom', nargs=4, type=float, metavar=('X0', 'Y0', 'X1', 'Y1'))
    ap.add_argument('--lanes', action='store_true', help='draw lane centre lines and road edges')
    ap.add_argument('--barriers', action='store_true', help='draw 0x3414D barrier lines')
    ap.add_argument('--size', type=int, default=4096)
    ap.add_argument('--highlight', type=int, nargs='*', default=[], help='route ids to draw in yellow')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    folder = os.path.join(a.game, 'TRACKS', 'ROUTES' + a.region)
    net = nr.Network.load(a.routes or os.path.join(folder, 'RoutesFreeRoam.bin'))

    if a.map:
        K, CX, CY = 0.0917, 312.0, 298.5
        S = a.size / 512.0
        img = minimap(a.game).resize((a.size, a.size), Image.LANCZOS)
        img = Image.eval(img, lambda v: int(v * 0.55))

        def P(x, y):
            return ((K * x + CX) * S, (-K * y + CY) * S)
    else:
        if a.zoom:
            x0, y0, x1, y1 = a.zoom
        else:
            x0 = min(r.bbox[0] for r in net.routes) - 50
            y0 = min(r.bbox[1] for r in net.routes) - 50
            x1 = max(r.bbox[2] for r in net.routes) + 50
            y1 = max(r.bbox[3] for r in net.routes) + 50
        k = a.size / max(x1 - x0, y1 - y0)
        W, H = int((x1 - x0) * k), int((y1 - y0) * k)
        img = Image.new('RGB', (W, H), (16, 18, 22))

        def P(x, y):
            return ((x - x0) * k, (y1 - y) * k)
    d = ImageDraw.Draw(img)
    scale = (P(100, 0)[0] - P(0, 0)[0]) / 100.0       # pixels per metre
    w = max(1, int(round(scale * 2.5))) if a.zoom else max(1, a.size // 1400)

    if a.barriers:
        pf = npth.PathFile.load(os.path.join(folder, 'PathsFreeRoam.bin'))
        for b in pf.barriers:
            d.line([P(*b.p0), P(*b.p1)], fill=(110, 40, 40), width=w)

    for r in net.routes:
        col = (255, 230, 0) if r.id in a.highlight else route_colour(r)
        if a.lanes:
            for side in (1, -1):
                edge = []
                for n in r.nodes:
                    dx, dy = n.dir_x / 32767.0, n.dir_y / 32767.0
                    o = n.half_right if side > 0 else -n.half_left
                    edge.append(P(n.x + dy * o, n.y - dx * o))
                d.line(edge, fill=(70, 70, 70), width=1)
            for j in range(8):
                pts = [n.lane_world(j) for n in r.nodes]
                run = []
                for p in pts + [None]:
                    if p is None:
                        if len(run) > 1:
                            d.line([P(*q) for q in run], fill=(90, 220, 120) if j < 4 else (230, 90, 200), width=1)
                        run = []
                    else:
                        run.append(p)
        d.line([P(n.x, n.y) for n in r.nodes], fill=col, width=w)
        if a.zoom:
            for n in r.nodes:
                x, y = P(n.x, n.y)
                d.ellipse([x - w, y - w, x + w, y + w], outline=col)
    for s in net.signposts:
        if s.src_route not in net.by_id or s.dst_route not in net.by_id:
            continue
        p, q = net.node(s.src_route, s.src_node), net.node(s.dst_route, s.dst_node)
        col = (40, 230, 90) if s.b == 2 else (200, 80, 255)
        if s.a == 7 and s.b == 7:
            continue
        d.line([P(p.x, p.y), P(q.x, q.y)], fill=col, width=max(1, w))
    for it in net.intersections:
        n = net.node(it.routes[0], it.nodes[0])
        x, y = P(n.x, n.y)
        rr = 3 * w + 2
        d.ellipse([x - rr, y - rr, x + rr, y + rr], outline=(255, 40, 40), width=w)
    img.save(a.out)
    print('wrote %s (%dx%d), %d routes, %d signposts, %d intersections' % (
        a.out, img.size[0], img.size[1], len(net.routes), len(net.signposts), len(net.intersections)))


if __name__ == '__main__':
    main()
