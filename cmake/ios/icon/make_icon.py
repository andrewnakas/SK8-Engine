#!/usr/bin/env python3
"""Generate the iOS app icon.

Original artwork - a skateboard in side profile, the silhouette that reads as
"skateboard" at 40 pixels with no text. Deliberately not the game's logo or box
art: the bundle carries none of the publisher's material, and the icon is not
an exception to that.

The deck is a STROKED curve rather than a filled polygon - a fill has to close
somewhere, and closing it across the tips turns the board into a boat.
"""
import os
from PIL import Image, ImageDraw

BG_TOP = (32, 35, 44)
BG_BOT = (14, 15, 19)
DECK = (246, 247, 249)
WHEEL = (255, 141, 42)
TRUCK = (150, 156, 168)

SS = 4  # supersample; everything here is a curve or a diagonal


def deck_points(w, h):
    """Flat middle with a kicked nose and tail, as a centre-line path."""
    x0, x1 = w * 0.150, w * 0.850
    y = h * 0.520
    kick = h * 0.115
    flat0, flat1 = x0 + (x1 - x0) * 0.22, x1 - (x1 - x0) * 0.22
    pts = []
    for i in range(61):
        x = x0 + (x1 - x0) * i / 60
        if x < flat0:
            t = (flat0 - x) / (flat0 - x0)
        elif x > flat1:
            t = (x - flat1) / (x1 - flat1)
        else:
            t = 0.0
        pts.append((x, y - kick * (t ** 1.8)))
    return pts


def render(size):
    w = h = size * SS
    img = Image.new("RGB", (w, h), BG_BOT)
    d = ImageDraw.Draw(img)
    for yy in range(h):
        t = yy / max(1, h - 1)
        d.line([(0, yy), (w, yy)],
               fill=tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3)))

    pts = deck_points(w, h)
    deck_y = pts[len(pts) // 2][1]
    deck_half = w * 0.021          # half the deck's thickness
    wheel_r = w * 0.052
    wheel_cy = deck_y + deck_half + w * 0.055

    # Trucks and wheels sit UNDER the deck and must stay clear of it, or the
    # board reads as a single blob at small sizes.
    for sx in (-1, 1):
        cx = w / 2 + sx * w * 0.190
        d.rounded_rectangle([cx - w * 0.019, deck_y,
                             cx + w * 0.019, wheel_cy],
                            radius=w * 0.011, fill=TRUCK)
        d.ellipse([cx - wheel_r, wheel_cy - wheel_r,
                   cx + wheel_r, wheel_cy + wheel_r], fill=WHEEL)

    d.line(pts, fill=DECK, width=int(deck_half * 2), joint="curve")
    # Rounded tips: a stroked line leaves square ends.
    for x, y in (pts[0], pts[-1]):
        d.ellipse([x - deck_half, y - deck_half, x + deck_half, y + deck_half], fill=DECK)

    return img.resize((size, size), Image.LANCZOS)


SIZES = {
    "AppIcon60x60@2x.png": 120, "AppIcon60x60@3x.png": 180,
    "AppIcon76x76@2x~ipad.png": 152, "AppIcon83.5x83.5@2x~ipad.png": 167,
    "AppIcon40x40@2x.png": 80, "AppIcon40x40@3x.png": 120,
    "AppIcon29x29@2x.png": 58, "AppIcon29x29@3x.png": 87,
    "AppIcon20x20@2x.png": 40, "AppIcon20x20@3x.png": 60,
    "AppIcon1024x1024.png": 1024,
}
out = os.path.dirname(os.path.abspath(__file__))
for name, px in SIZES.items():
    render(px).save(os.path.join(out, name), "PNG")
print(f"wrote {len(SIZES)} icons")
