"""Draws the editor's icon, src/ui/App.ico: a dark tile with a street grid and
the letters FRS. Run it again after changing the look; the .ico is committed.

    python tools/make_icon.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'src', 'ui', 'App.ico')
FONT = r'C:\Windows\Fonts\segoeuib.ttf'

BACKGROUND = (24, 28, 34, 255)
STREET = (58, 66, 78, 255)
ACCENT = (163, 230, 53, 255)


def draw(size):
    scale = 4  # drawn large and scaled down, for clean edges
    s = size * scale
    image = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=s // 6, fill=BACKGROUND)
    # A few streets, one of them the route.
    w = max(scale, s // 22)
    for f in (0.28, 0.72):
        d.line([(s * f, s * 0.08), (s * f, s * 0.92)], fill=STREET, width=w)
        d.line([(s * 0.08, s * f), (s * 0.92, s * f)], fill=STREET, width=w)
    d.line([(s * 0.28, s * 0.92), (s * 0.28, s * 0.72), (s * 0.72, s * 0.72), (s * 0.72, s * 0.28),
            (s * 0.92, s * 0.28)], fill=ACCENT, width=w, joint='curve')
    if size >= 24:
        font = ImageFont.truetype(FONT, int(s * 0.36))
        box = d.textbbox((0, 0), 'FRS', font=font)
        x = (s - (box[2] - box[0])) / 2 - box[0]
        y = (s - (box[3] - box[1])) / 2 - box[1] - s * 0.04
        d.text((x, y), 'FRS', font=font, fill=(236, 240, 244, 255), stroke_width=max(1, s // 64),
               stroke_fill=BACKGROUND)
    return image.resize((size, size), Image.LANCZOS)


sizes = [16, 24, 32, 48, 64, 128, 256]
images = [draw(n) for n in sizes]
images[-1].save(OUT, format='ICO', sizes=[(n, n) for n in sizes], append_images=images[:-1])
images[-1].save(os.path.join(HERE, '..', 'docs', 'icon.png'))
print('wrote', os.path.normpath(OUT))
