"""Draws the README's cover, docs/cover.png (1280 x 640), in the FRS Mods style.

    python tools/make_cover.py <screenshot.png>

The screenshot is a capture of the editor's window; the cover frames it on the
right, beside the icon (tools/make_icon.py), the name and what the editor does.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_icon  # noqa: E402

OUT = os.path.join(HERE, '..', 'docs', 'cover.png')
W, H = 1280, 640
FONT_BOLD = r'C:\Windows\Fonts\segoeuib.ttf'
FONT = r'C:\Windows\Fonts\segoeui.ttf'
FONT_LIGHT = r'C:\Windows\Fonts\segoeuil.ttf'

TEXT = (0xF2, 0xF5, 0xF9)
SUBTLE = (0xA9, 0xB8, 0xCC)
DIM = (0x6F, 0x82, 0x9C)
CHIP = (0x13, 0x2A, 0x4A)
CHIP_EDGE = (0x24, 0x48, 0x76)
FEATURES = [['Roads & junctions', 'Terrain', 'Traffic & GPS'], ['Collision', 'Objects', 'Blender round trip']]


def background():
    """The logo's navy, darker bottom left, with the street grid fading in."""
    s = 4
    small = Image.new('RGB', (W // s, H // s))
    px = small.load()
    for y in range(H // s):
        for x in range(W // s):
            t = (x / (W / s) * 0.6 + (1 - y / (H / s)) * 0.4)
            px[x, y] = tuple(int(make_icon.TILE_LOW[k] + (make_icon.TILE_HIGH[k] - make_icon.TILE_LOW[k]) * t)
                             for k in range(3))
    image = small.resize((W, H), Image.BICUBIC).convert('RGBA')
    grid = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(grid)
    horizon, vanish = H * 0.52, W * 0.30
    for k in range(-14, 16):
        x0 = vanish + k * 150
        for t in range(30):
            a, b = t / 30.0, (t + 1) / 30.0
            ya, yb = H + (horizon - H) * a, H + (horizon - H) * b
            alpha = int(70 * (1 - a) ** 1.6)
            d.line([(x0 + (vanish - x0) * a, ya), (x0 + (vanish - x0) * b, yb)], fill=make_icon.GRID + (alpha,), width=1)
    for k in range(1, 12):
        a = 1 - 0.74 ** k
        y = H + (horizon - H) * a
        d.line([(0, y), (W, y)], fill=make_icon.GRID + (int(70 * (1 - a) ** 1.6),), width=1)
    image.alpha_composite(grid)
    return image


def framed(shot, width):
    """The screenshot, rounded, with a thin blue edge and a shadow under it."""
    height = int(shot.height * width / shot.width)
    shot = shot.convert('RGB').resize((width, height), Image.LANCZOS)
    radius = 12
    mask = Image.new('L', (width, height), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, width - 1, height - 1], radius=radius, fill=255)
    out = Image.new('RGBA', (width + 80, height + 80), (0, 0, 0, 0))
    shadow = Image.new('RGBA', out.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle([40, 52, 40 + width, 52 + height], radius=radius, fill=(0, 0, 0, 170))
    out.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(18)))
    out.paste(shot, (40, 40), mask)
    ImageDraw.Draw(out).rounded_rectangle([40, 40, 40 + width - 1, 40 + height - 1], radius=radius,
                                          outline=make_icon.BAR_MID + (200,), width=2)
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    image = background()
    d = ImageDraw.Draw(image)

    # The screenshot, its left part kept: the panels and the city.
    shot = Image.open(sys.argv[1])
    shot = shot.crop((0, 0, min(shot.width, int(shot.height * 1.62)), shot.height))
    frame = framed(shot, 700)
    image.alpha_composite(frame, (W - 48 - 700 - 40, (H - frame.height) // 2 - 6))

    x = 64
    icon = make_icon.draw(256).resize((88, 88), Image.LANCZOS)
    glow = Image.new('RGBA', image.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).rounded_rectangle([x - 2, 76, x + 89, 167], radius=17, outline=CHIP_EDGE + (255,), width=2)
    image.alpha_composite(glow)
    image.alpha_composite(icon, (x, 78))

    title = ImageFont.truetype(FONT_BOLD, 66)
    d.text((x - 3, 186), 'FRS World', font=title, fill=TEXT)
    d.text((x - 3, 260), 'Editor', font=title, fill=make_icon.BAR_LIGHT)

    sub = ImageFont.truetype(FONT, 22)
    d.text((x, 352), 'Map editor for Need for Speed', font=sub, fill=SUBTLE)
    d.text((x, 380), 'Underground 2', font=sub, fill=SUBTLE)

    chip_font = ImageFont.truetype(FONT, 15)
    y = 432
    for row in FEATURES:
        cx = x
        for label in row:
            box = d.textbbox((0, 0), label, font=chip_font)
            w = box[2] - box[0] + 26
            d.rounded_rectangle([cx, y, cx + w, y + 32], radius=16, fill=CHIP, outline=CHIP_EDGE, width=1)
            d.text((cx + 13, y + 16), label, font=chip_font, fill=TEXT, anchor='lm')
            cx += w + 8
        y += 42

    foot = ImageFont.truetype(FONT_LIGHT, 17)
    d.text((x, 580), 'v0.1.0  \u00b7  Windows  \u00b7  free  \u00b7  FRS Mods', font=foot, fill=DIM)

    image.convert('RGB').save(OUT)
    print('wrote', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
