"""Reads a memory recording made in the game (FRSWorldLoader with [Recorder] Enabled=1: F9).

The file holds the executable's writable sections as they were when the
recording started, then every 32-bit word that changed, ten times a second,
and the moments the player marked with F8.

What is worth reading is the handful of words that change rarely and do so
right at a mark: a flag that goes up when a shop's prompt appears, an index
that points at the shop being entered. Counters and timers change on every
sample and are left out.

    python memrec.py FRS-memory-001.bin [--window 1500] [--max-changes 6]
"""
import argparse
import struct
import sys
from collections import defaultdict


def read(path):
    data = open(path, 'rb').read()
    magic, nranges, _ = struct.unpack_from('<3I', data, 0)
    if magic != 0x314D5246:
        raise SystemExit('not a recording')
    at = 12
    ranges = []
    for _ in range(nranges):
        ranges.append(struct.unpack_from('<2I', data, at))
        at += 8
    words = sum((e - b) // 4 for b, e in ranges)
    initial = {}
    k = 0
    for b, e in ranges:
        for a in range(b, e, 4):
            initial[a] = struct.unpack_from('<I', data, at + 4 * k)[0]
            k += 1
    at += 4 * words

    history = defaultdict(list)     # address -> [(ms, value)]
    marks = []
    samples = 0
    while at + 12 <= len(data):
        tag, ms, n = struct.unpack_from('<3I', data, at)
        at += 12
        if tag == 0x4B52414D:        # MARK
            marks.append((ms, n))
            continue
        if tag != 0x454D4152:
            break
        samples += 1
        for i in range(n):
            a, v = struct.unpack_from('<2I', data, at + 8 * i)
            history[a].append((ms, v))
        at += 8 * n
    return initial, history, marks, samples


def as_float(v):
    return struct.unpack('<f', struct.pack('<I', v))[0]


def show(v):
    f = as_float(v)
    text = '%08X %d' % (v, v if v < 0x80000000 else v - 0x100000000)
    if 1e-4 < abs(f) < 1e7:
        text += ' (%.3f)' % f
    return text


def main():
    p = argparse.ArgumentParser()
    p.add_argument('file')
    p.add_argument('--window', type=int, default=1500, help='ms around a mark')
    p.add_argument('--max-changes', type=int, default=6, help='ignore words that change more often')
    a = p.parse_args()

    initial, history, marks, samples = read(a.file)
    last_ms = max((h[-1][0] for h in history.values()), default=0)
    print('%d samples over %.1f s, %d words ever changed, marks: %s' % (
        samples, last_ms / 1000.0, len(history), ', '.join('#%d at %.1f s' % (n, ms / 1000.0) for ms, n in marks)))

    rare = {addr: h for addr, h in history.items() if len(h) <= a.max_changes}
    print('%d words changed at most %d times' % (len(rare), a.max_changes))

    for ms, n in marks:
        print('\n=== mark #%d at %.1f s: rare words changing within %d ms' % (n, ms / 1000.0, a.window))
        hits = []
        for addr, h in rare.items():
            for t, v in h:
                if abs(t - ms) <= a.window:
                    hits.append((abs(t - ms), t, addr, v))
        for _, t, addr, v in sorted(hits)[:60]:
            before = initial.get(addr, 0)
            for tt, vv in history[addr]:
                if tt >= t:
                    break
                before = vv
            print('  %+6d ms  0x%08X  %s  ->  %s' % (t - ms, addr, show(before), show(v)))


if __name__ == '__main__':
    main()
