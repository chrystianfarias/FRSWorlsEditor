"""Draws the editor's icon, src/ui/App.ico, and docs/icon.png.

The FRS Mods mark -- three slanted bars on a navy tile, light blue to deep
blue -- over a city's street grid in perspective, with a map pin: the world
being edited. Small sizes keep only the bars, which stay readable at 16 px.
Run it again after changing the look; the .ico is committed.

    python tools/make_icon.py
"""
import os

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'src', 'ui', 'App.ico')

# The FRS Mods palette, read off the logo.
BAR_LIGHT = (0x4D, 0xB0, 0xE6)
BAR_MID = (0x36, 0x96, 0xDB)
BAR_DEEP = (0x24, 0x79, 0xC8)
TILE_LOW = (0x08, 0x16, 0x29)       # bottom left
TILE_HIGH = (0x11, 0x28, 0x47)      # top right
GRID = (0x2A, 0x5A, 0x8F)


def tile(s):
    """The navy tile: a diagonal gradient, rounded like the logo's."""
    gradient = Image.new('RGBA', (s, s))
    px = gradient.load()
    for y in range(s):
        for x in range(s):
            t = (x + (s - 1 - y)) / (2.0 * (s - 1))
            px[x, y] = tuple(int(TILE_LOW[k] + (TILE_HIGH[k] - TILE_LOW[k]) * t) for k in range(3)) + (255,)
    mask = Image.new('L', (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.18), fill=255)
    out = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    out.paste(gradient, (0, 0), mask)
    return out, mask


def street_grid(s, mask):
    """A street grid seen from above and behind, fading into the distance."""
    layer = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    horizon, vanish_x = s * 0.40, s * 0.62
    bottom = s * 1.02
    w = max(1, s // 110)
    # Streets running away from the viewer, meeting at the vanishing point.
    for k in range(-6, 9):
        x0 = vanish_x + k * s * 0.17
        for t in range(24):
            a, b = t / 24.0, (t + 1) / 24.0
            ya, yb = bottom + (horizon - bottom) * a, bottom + (horizon - bottom) * b
            xa, xb = x0 + (vanish_x - x0) * a, x0 + (vanish_x - x0) * b
            alpha = int(150 * (1.0 - a) ** 1.4)
            d.line([(xa, ya), (xb, yb)], fill=GRID + (alpha,), width=w)
    # Cross streets, closer together towards the horizon.
    for k in range(1, 9):
        a = 1.0 - 0.72 ** k
        y = bottom + (horizon - bottom) * a
        alpha = int(150 * (1.0 - a) ** 1.4)
        d.line([(0, y), (s, y)], fill=GRID + (alpha,), width=w)
    clipped = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    clipped.paste(layer, (0, 0), mask)
    return clipped


def bars(d, s, scale, dx, dy):
    """The three bars of the FRS mark (the logo's geometry, in 0..1 units)."""
    shapes = [
        (BAR_LIGHT, [(0.268, 0.230), (0.908, 0.230), (0.865, 0.390), (0.225, 0.390)]),
        (BAR_MID, [(0.214, 0.430), (0.655, 0.430), (0.612, 0.590), (0.171, 0.590)]),
        (BAR_DEEP, [(0.161, 0.630), (0.341, 0.630), (0.298, 0.790), (0.118, 0.790)]),
    ]
    for colour, points in shapes:
        d.polygon([((x * scale + dx) * s, (y * scale + dy) * s) for x, y in points], fill=colour + (255,))


def pin(image, s, cx, cy, r):
    """A map pin standing at (cx, cy + its tip), in the light blue."""
    shadow = Image.new('RGBA', image.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).ellipse([cx - r * 0.75, cy + r * 2.15, cx + r * 0.75, cy + r * 2.45],
                                   fill=(0, 0, 0, 110))
    image.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(r * 0.12)))
    d = ImageDraw.Draw(image)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=BAR_LIGHT + (255,))
    d.polygon([(cx - r * 0.86, cy + r * 0.5), (cx + r * 0.86, cy + r * 0.5), (cx, cy + r * 2.3)],
              fill=BAR_LIGHT + (255,))
    d.ellipse([cx - r * 0.42, cy - r * 0.42, cx + r * 0.42, cy + r * 0.42], fill=TILE_LOW + (255,))


def draw(size):
    scale = 4 if size < 256 else 2        # drawn large and scaled down, for clean edges
    s = size * scale
    image, mask = tile(s)
    if size >= 48:
        image.alpha_composite(street_grid(s, mask))
    d = ImageDraw.Draw(image)
    if size >= 32:
        # The mark a little smaller and up, the pin below it on the right.
        bars(d, s, 0.86, 0.02, -0.02)
        pin(image, s, s * 0.74, s * 0.60, s * 0.095)
    else:
        bars(d, s, 1.0, -0.015, 0.0)
    return image.resize((size, size), Image.LANCZOS)


if __name__ == '__main__':
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [draw(n) for n in sizes]
    images[-1].save(OUT, format='ICO', sizes=[(n, n) for n in sizes], append_images=images[:-1])
    draw(512).save(os.path.join(HERE, '..', 'docs', 'icon.png'))
    print('wrote', os.path.normpath(OUT))
