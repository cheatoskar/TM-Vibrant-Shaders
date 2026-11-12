"""The four Horizon module icons (128 px PNGs, plugin.json "icon"): TM Vibrant Shaders' black hole
(packaging/icons/icon_a.png); the other three carry a white sign on a dark round badge.

Run:  python packaging/horizon/make_icons.py
Writes packaging/horizon/<id>/icon.png.
"""
import os
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(HERE, "..", "icons", "icon_a.png")
S = 512  # drawn at 512, saved at 128
OUT = 128
WHITE = (255, 255, 255, 255)
ACCENT = (255, 168, 72, 255)


def base(dim):
    im = Image.open(BASE).convert("RGBA").resize((S, S), Image.LANCZOS)
    if dim < 1.0:
        im = ImageEnhance.Brightness(im).enhance(dim)
    return im


def badge(im):
    """A dark disc in the lower right, a soft shadow under it; returns its draw and centre/radius."""
    cx, cy, r = S * 0.66, S * 0.66, S * 0.30
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).ellipse([cx - r - 6, cy - r + 4, cx + r + 6, cy + r + 14], fill=(0, 0, 0, 200))
    im.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(12)))
    d = ImageDraw.Draw(im)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(18, 20, 28, 255), outline=ACCENT, width=10)
    return d, cx, cy, r


def capture(im):
    d, cx, cy, r = badge(im)
    w, h = r * 1.15, r * 0.78
    x0, y0 = cx - w / 2, cy - h / 2 + r * 0.08
    d.rounded_rectangle([x0, y0, x0 + w, y0 + h], radius=r * 0.12, fill=WHITE)
    d.rounded_rectangle([cx - r * 0.22, y0 - r * 0.16, cx + r * 0.22, y0 + 4], radius=r * 0.05, fill=WHITE)
    lr = h * 0.33
    d.ellipse([cx - lr, y0 + h / 2 - lr, cx + lr, y0 + h / 2 + lr], fill=(18, 20, 28, 255))
    lr2 = lr * 0.55
    d.ellipse([cx - lr2, y0 + h / 2 - lr2, cx + lr2, y0 + h / 2 + lr2], fill=ACCENT)


def upscaling(im):
    d, cx, cy, r = badge(im)
    # a small square growing into a large one: two corners and a diagonal arrow
    s1, s2 = r * 0.42, r * 1.05
    x0, y0 = cx - s2 / 2, cy - s2 / 2
    d.rectangle([x0, y0 + s2 - s1, x0 + s1, y0 + s2], fill=WHITE)
    t = 12
    d.line([x0, y0, x0 + s2 * 0.35, y0], fill=WHITE, width=t)
    d.line([x0, y0, x0, y0 + s2 * 0.35], fill=WHITE, width=t)
    d.line([x0 + s2, y0 + s2, x0 + s2 * 0.65, y0 + s2], fill=WHITE, width=t)
    d.line([x0 + s2, y0 + s2, x0 + s2, y0 + s2 * 0.65], fill=WHITE, width=t)
    ax0, ay0, ax1, ay1 = x0 + s1 + 10, y0 + s2 - s1 - 10, x0 + s2 - 8, y0 + 8
    d.line([ax0, ay0, ax1, ay1], fill=ACCENT, width=16)
    d.polygon([(ax1 + 6, ay1 - 6), (ax1 - 52, ay1 + 2), (ax1 - 2, ay1 + 52)], fill=ACCENT)


def packs(im):
    d, cx, cy, r = badge(im)
    # three stacked tiles (layers)
    w, h = r * 1.15, r * 0.42
    for i, col in enumerate([(120, 128, 150, 255), (200, 205, 220, 255), WHITE]):
        oy = (1 - i) * r * 0.28
        pts = [(cx, cy - h / 2 + oy), (cx + w / 2, cy + oy), (cx, cy + h / 2 + oy), (cx - w / 2, cy + oy)]
        d.polygon(pts, fill=col, outline=(18, 20, 28, 255))
    d.polygon([(cx, cy - h / 2 - r * 0.28), (cx + w / 2, cy - r * 0.28), (cx, cy + h / 2 - r * 0.28), (cx - w / 2, cy - r * 0.28)],
              fill=ACCENT, outline=(18, 20, 28, 255))


def save(im, ident):
    folder = os.path.join(HERE, ident)
    os.makedirs(folder, exist_ok=True)
    im.resize((OUT, OUT), Image.LANCZOS).save(os.path.join(folder, "icon.png"), optimize=True)


def main():
    save(base(1.0), "tmvs")
    for ident, draw in (("tmvs-capture", capture), ("tmvs-upscaling", upscaling), ("tmvs-packs", packs)):
        im = base(0.55)
        draw(im)
        save(im, ident)


if __name__ == "__main__":
    main()
