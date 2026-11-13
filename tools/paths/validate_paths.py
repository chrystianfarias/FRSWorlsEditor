"""Parses every Routes*.bin and Paths*.bin of the game and proves the layouts.

For each file: parse into records, rebuild the bytes from the parsed fields
(must be identical: every byte is accounted for), and run the cross-reference
checks of nfsroutes.Network.check / nfspaths.PathFile.check.

    python tools/paths/validate_paths.py                  # all regions
    python tools/paths/validate_paths.py --region L4RA -v  # one region, free-roam summary
"""
import argparse
import collections
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nfsroutes as nr
import nfspaths as npth

GAME = 'F:/Games/NFSU2'


def summary(net):
    if not net.routes:
        print('  no routes')
        return
    nodes = sum(r.count for r in net.routes)
    lanes = sum(1 for r in net.routes if any(r.lane_present))
    km = sum(r.length for r in net.routes) / 1000
    types = collections.Counter((s.a, s.b) for s in net.signposts)
    dangling = sum(1 for r in net.routes for n in r.nodes for s in n.slots
                   if s != nr.NO_SLOT and s >= len(net.signposts))
    print('  routes %d (ids %d..%d, %d with lanes), nodes %d, %.1f km' % (
        len(net.routes), net.routes[0].id, net.routes[-1].id, lanes, nodes, km))
    print('  signposts %d, (A,B) %s' % (len(net.signposts), ', '.join('%d/%d:%d' % (a, b, n) for (a, b), n in types.most_common(8))))
    print('  slot values >= signpost count (ignored by the game): %d' % dangling)
    print('  intersections %d' % len(net.intersections))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=GAME)
    ap.add_argument('--region', default='*')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.game, 'TRACKS', 'ROUTES%s' % a.region, '*.bin')))
    ok = fail = 0
    for path in files:
        base = os.path.basename(path)
        buf = open(path, 'rb').read()
        try:
            if base.startswith('Routes'):
                obj = nr.Network(buf)
            elif base.startswith('Paths'):
                obj = npth.PathFile(buf)
            else:
                continue
            same = obj.to_bytes() == buf
            bad = obj.check(strict=base.endswith('FreeRoam.bin')) if base.startswith('Routes') else obj.check()
        except ValueError as e:
            same, bad = False, [str(e)]
        if same and not bad:
            ok += 1
        else:
            fail += 1
            print('FAIL %s: rebuild %s, %d problems: %s' % (path, 'ok' if same else 'DIFFERS', len(bad), bad[:5]))
        if a.v and base in ('RoutesFreeRoam.bin', 'PathsFreeRoam.bin'):
            print(path)
            if base.startswith('Routes'):
                summary(obj)
            else:
                z = collections.Counter(zz.type_name for zz in obj.zones)
                print('  zones %d %s' % (len(obj.zones), dict(z)))
                print('  barrier lines %d, children %s' % (len(obj.barriers), ['%X' % c for c in obj.order]))
    print('%d files parsed and rebuilt byte for byte, %d failed' % (ok, fail))
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
