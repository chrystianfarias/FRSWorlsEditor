"""Draws every race start trigger on the L4RA city map.

    python tools/race_events_map.py <dir with TRACKMAP*.png> <out dir>
"""
import collections
import glob
import math
import os
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(__file__))
import race_events

K, CX, CY = 0.0917, 312.0, 298.5          # world -> TRACKMAP pixel, see NFSU2-FORMATS.md
UP = 4
TYPES = [('CIRCUIT', 'Circuit', (230, 25, 75)), ('SPRINT', 'Sprint', (60, 180, 75)),
         ('DRIFT', 'Drift', (70, 200, 240)), ('DRAG', 'Drag', (245, 130, 48)),
         ('STREET', 'Street X', (240, 50, 230)), ('URL', 'URL', (255, 225, 25)),
         ('SUV', 'SUV', (170, 110, 40))]


def kind_of(name):
    for key, _, _ in TYPES:
        if re.search(r'(^|_)%s(_|$)' % key, name):
            return key
    return None


def main(maps, out_dir):
    races = [r for r in race_events.races() if not r['name'].startswith('DDAY')]
    by_track = {}
    for r in races:
        k = kind_of(r['name'])
        if k:
            by_track[r['track']] = k
    for r in races:
        r['kind'] = kind_of(r['name']) or by_track.get(r['track'], 'CIRCUIT')
    trig = race_events.triggers({r['event_hash'] for r in races})

    events = collections.OrderedDict()
    for r in sorted(races, key=lambda r: r['event']):
        if r['event_hash'] in trig:
            events.setdefault(r['event'], []).append(r)

    # median of three race maps whose routes do not overlap = the map with no route drawn
    a, b, c = (Image.open(os.path.join(maps, 'TRACKMAP%s.png' % n)).convert('RGB') for n in ('4011', '4061', '4041'))
    base = ImageChops.lighter(ImageChops.darker(a, b), ImageChops.darker(ImageChops.lighter(a, b), c))
    w = 512 * UP
    img = Image.eval(base.resize((w, w), Image.LANCZOS), lambda v: int(v * 0.8))
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype('arialbd.ttf', 17)
    big = ImageFont.truetype('arialbd.ttf', 30)
    mid = ImageFont.truetype('arialbd.ttf', 22)
    colour = {k: c for k, _, c in TYPES}

    placed = []
    for rs in events.values():
        x, y, _ = trig[rs[0]['event_hash']]
        px, py = (K * x + CX) * UP, (-K * y + CY) * UP
        placed.append((px - 16, py - 16, px + 16, py + 16))
    for ev, rs in events.items():
        x, y, _ = trig[rs[0]['event_hash']]
        px, py = (K * x + CX) * UP, (-K * y + CY) * UP
        kinds = collections.Counter(r['kind'] for r in rs)
        r0 = 13
        d.ellipse([px - r0 - 3, py - r0 - 3, px + r0 + 3, py + r0 + 3], fill=(0, 0, 0))
        start = -90.0
        for k, n in kinds.most_common():
            span = 360.0 * n / len(rs)
            d.pieslice([px - r0, py - r0, px + r0, py + r0], start, start + span, fill=colour[k])
            start += span
        label = '%s (%d)' % (ev.replace('EVENT_LOCATOR_', 'LOC_'), len(rs)) if len(rs) > 1 else ev.replace('EVENT_LOCATOR_', 'LOC_')
        tw = d.textlength(label, font=font)
        spots = [(18, -10), (-18 - tw, -10), (-tw / 2, 17), (-tw / 2, -37)]
        spots += [(sx * r, sy * r) for r in range(30, 200, 14)
                  for sx, sy in [(1, -1), (1, 1), (-1, -1), (-1, 1), (1, 0), (-1, 0), (0, 1), (0, -1)]]
        for dx, dy in spots:
            if dx < 0 and abs(dx) > 18 + tw and (dx, dy) not in spots[:4]:
                pass
            ox = dx if dx >= 0 or (dx, dy) in spots[:4] else dx - tw
            box = (px + ox - 2, py + dy - 2, px + ox + tw + 2, py + dy + 19)
            if not any(box[0] < b[2] and box[2] > b[0] and box[1] < b[3] and box[3] > b[1] for b in placed):
                break
        placed.append(box)
        if (dx, dy) not in spots[:4]:
            d.line([(px, py), ((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)], fill=(255, 255, 255), width=1)
            d.rectangle(box, fill=(0, 0, 0))
        d.text((px + ox, py + dy), label, font=font, fill=(255, 255, 255), stroke_width=3, stroke_fill=(0, 0, 0))

    lx, ly = 40, w - 70 - 32 * len(TYPES)
    d.rectangle([lx - 20, ly - 70, lx + 520, w - 30], fill=(0, 0, 0))
    d.text((lx, ly - 60), 'NFSU2 - %d race start points' % len(events), font=big, fill=(255, 255, 255))
    for i, (k, title, c) in enumerate(TYPES):
        n = sum(1 for r in races if r['kind'] == k)
        y = ly + i * 32
        d.ellipse([lx, y + 2, lx + 22, y + 24], fill=c)
        d.text((lx + 34, y), '%s  (%d races)' % (title, n), font=mid, fill=(255, 255, 255))
    d.text((lx, ly + 32 * len(TYPES) + 2), '(n) = races sharing the point; split icon = mixed types',
           font=font, fill=(200, 200, 200))
    img.save(os.path.join(out_dir, 'race-events-L4RA.png'))

    with open(os.path.join(out_dir, 'race-events-L4RA.txt'), 'w') as f:
        for ev, rs in events.items():
            x, y, _ = trig[rs[0]['event_hash']]
            f.write('%-18s x=%8.1f y=%8.1f\n' % (ev, x, y))
            for r in rs:
                f.write('    %-22s track %d  %s\n' % (r['name'] or '(unnamed)', r['track'], r['kind']))
    print(len(events), 'points,', len(races), 'races')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
