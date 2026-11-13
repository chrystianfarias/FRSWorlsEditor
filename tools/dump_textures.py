"""Decodes textures out of a track stream and writes them as PNG.

Independent of the editor and of the GPU, so it can be used to check what a
texture is actually supposed to look like.

    python tools/dump_textures.py L4RA --sheet out.png --count 64
    python tools/dump_textures.py L4RA --name RDP_LANEA_YS_AA_KT --out .
"""
import argparse
import collections
import os
import struct
import zlib

ALIGN = {0x00134B01: 128, 0x33320002: 128, 0x00134012: 8}


def chunks(buf, off, end):
    """Yields (id, dataOffset, dataSize, rawOffset, rawSize) for each chunk."""
    while off + 8 <= end:
        cid, csz = struct.unpack_from("<II", buf, off)
        if off + 8 + csz > end:
            return
        a = ALIGN.get(cid, 16)
        raw = off + 8
        data = (raw + a - 1) // a * a
        if not all(b == 0x11 for b in buf[raw:data]):
            data = raw
        yield cid, data, csz - (data - raw), raw, csz
        off += 8 + csz


def write_png(path, width, height, rgb):
    rows = b''.join(b'\x00' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(rows, 6))
        + chunk(b'IEND', b''))


def rgb565(value):
    return (((value >> 11) & 31) * 255 // 31,
            ((value >> 5) & 63) * 255 // 63,
            (value & 31) * 255 // 31)


def decode_dxt(data, width, height, dxt1):
    """DXT1 and DXT3 colour blocks. Alpha is ignored; this is for looking at."""
    out = bytearray(width * height * 3)
    stride = 8 if dxt1 else 16
    blocks_x = max(1, (width + 3) // 4)
    for block in range(len(data) // stride):
        at = block * stride + (0 if dxt1 else 8)
        if at + 8 > len(data):
            break
        c0, c1, bits = struct.unpack_from("<HHI", data, at)
        p = [rgb565(c0), rgb565(c1)]
        if c0 > c1 or not dxt1:
            p.append(tuple((2 * p[0][k] + p[1][k]) // 3 for k in range(3)))
            p.append(tuple((p[0][k] + 2 * p[1][k]) // 3 for k in range(3)))
        else:
            p.append(tuple((p[0][k] + p[1][k]) // 2 for k in range(3)))
            p.append((0, 0, 0))
        bx, by = (block % blocks_x) * 4, (block // blocks_x) * 4
        for ty in range(4):
            for tx in range(4):
                x, y = bx + tx, by + ty
                if x >= width or y >= height:
                    continue
                colour = p[(bits >> (2 * (ty * 4 + tx))) & 3]
                o = (y * width + x) * 3
                out[o:o + 3] = bytes(colour)
    return bytes(out)


def decode_p8(data, width, height, palette):
    out = bytearray(width * height * 3)
    for i in range(min(len(data), width * height)):
        e = data[i] * 4
        # palette entries are D3DCOLOR: blue, green, red, then a gloss byte
        out[i * 3 + 0] = palette[e + 2]
        out[i * 3 + 1] = palette[e + 1]
        out[i * 3 + 2] = palette[e + 0]
    return bytes(out)


def load(region, path=None):
    """`region` names a track stream; `path` reads any file that holds packs."""
    if path is None:
        path = "F:/Games/NFSU2/TRACKS/STREAM%s.BUN" % region
    st = open(path, 'rb').read()

    uses = collections.Counter()
    textures = {}
    off = 0
    while off + 8 <= len(st):
        cid, csz = struct.unpack_from("<II", st, off)
        if cid == 0 and csz == 0:
            off += 8
            continue
        if off + 8 + csz > len(st):
            break
        if cid == 0x80134000:
            for c1, d1, s1, r1, rs1 in chunks(st, off + 8, off + 8 + csz):
                if c1 != 0x80134010:
                    continue
                for c2, d2, s2, _, _ in chunks(st, r1, r1 + rs1):
                    if c2 == 0x00134012:
                        for k in range(s2 // 8):
                            uses[struct.unpack_from("<I", st, d2 + k * 8)[0]] += 1
        elif cid == 0xB3300000:
            hdr = fmt = pix = None
            for c1, d1, s1, r1, rs1 in chunks(st, off + 8, off + 8 + csz):
                if c1 == 0xB3310000:
                    for c2, d2, s2, _, _ in chunks(st, d1, d1 + s1):
                        if c2 == 0x33310004:
                            hdr = (d2, s2)
                        elif c2 == 0x33310005:
                            fmt = (d2, s2)
                elif c1 == 0xB3320000:
                    for c2, d2, s2, _, _ in chunks(st, d1, d1 + s1):
                        if c2 == 0x33320002:
                            pix = (d2, s2)
            if not (hdr and fmt and pix):
                continue
            for i in range(hdr[1] // 124):
                e = hdr[0] + i * 124
                g = fmt[0] + i * 32
                z = st.find(b'\0', e + 0x0C)
                h = struct.unpack_from("<I", st, e + 0x24)[0]
                textures[h] = dict(
                    name=st[e + 0x0C:z].decode('latin1'),
                    fourcc=struct.unpack_from("<I", st, g + 0x14)[0],
                    width=struct.unpack_from("<H", st, e + 0x44)[0],
                    height=struct.unpack_from("<H", st, e + 0x46)[0],
                    data=st[pix[0] + struct.unpack_from("<I", st, e + 0x30)[0]:
                            pix[0] + struct.unpack_from("<I", st, e + 0x30)[0]
                            + struct.unpack_from("<I", st, e + 0x38)[0]],
                    palette=st[pix[0] + struct.unpack_from("<I", st, e + 0x34)[0]:
                               pix[0] + struct.unpack_from("<I", st, e + 0x34)[0] + 1024]
                    if struct.unpack_from("<I", st, e + 0x3C)[0] else b'')
        off += 8 + csz
    return textures, uses


def to_rgb(t):
    w, h = t['width'], t['height']
    if not w or not h:
        return None
    if t['fourcc'] == 0x31545844:
        return decode_dxt(t['data'], w, h, True)
    if t['fourcc'] in (0x33545844, 0x35545844):
        return decode_dxt(t['data'], w, h, False)
    if t['fourcc'] == 41 and len(t['palette']) >= 1024:
        return decode_p8(t['data'], w, h, t['palette'])
    return None


def scale(rgb, w, h, size):
    out = bytearray(size * size * 3)
    for y in range(size):
        sy = y * h // size
        for x in range(size):
            sx = x * w // size
            s = (sy * w + sx) * 3
            d = (y * size + x) * 3
            out[d:d + 3] = rgb[s:s + 3]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("region", nargs='?', default="L4RA")
    ap.add_argument("--sheet")
    ap.add_argument("--count", type=int, default=64)
    ap.add_argument("--cell", type=int, default=96)
    ap.add_argument("--name")
    ap.add_argument("--out", default=".")
    ap.add_argument("--file", help="read packs from this file instead of a track stream")
    args = ap.parse_args()

    textures, uses = load(args.region, args.file)
    print("%s: %d textures" % (args.region, len(textures)))

    if args.name:
        for h, t in textures.items():
            if t['name'] != args.name:
                continue
            rgb = to_rgb(t)
            if not rgb:
                print("cannot decode %s" % t['name'])
                return
            path = os.path.join(args.out, t['name'] + ".png")
            write_png(path, t['width'], t['height'], rgb)
            print("wrote %s (%dx%d)" % (path, t['width'], t['height']))
        return

    ranked = sorted(textures.items(), key=lambda kv: -uses.get(kv[0], 0))
    picked = []
    for h, t in ranked:
        rgb = to_rgb(t)
        if rgb:
            picked.append((t, rgb, uses.get(h, 0)))
        if len(picked) >= args.count:
            break

    cell = args.cell
    cols = 8
    rows = (len(picked) + cols - 1) // cols
    sheet = bytearray(cols * cell * rows * cell * 3)
    width = cols * cell
    for i, (t, rgb, use) in enumerate(picked):
        thumb = scale(rgb, t['width'], t['height'], cell)
        ox, oy = (i % cols) * cell, (i // cols) * cell
        for y in range(cell):
            d = ((oy + y) * width + ox) * 3
            s = y * cell * 3
            sheet[d:d + cell * 3] = thumb[s:s + cell * 3]

    out = args.sheet or os.path.join(args.out, "texture-sheet.png")
    write_png(out, width, rows * cell, bytes(sheet))
    print("wrote %s: %d textures, %d per row" % (out, len(picked), cols))
    for i, (t, _, use) in enumerate(picked):
        print("  %2d,%d  %-26s %4dx%-4d used %d" %
              (i // cols, i % cols, t['name'], t['width'], t['height'], use))


if __name__ == "__main__":
    main()
