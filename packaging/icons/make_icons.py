"""Procedural icons for TM Vibrant Shaders (TMLoader icon_link proposals).

Run:  python packaging/icons/make_icons.py
Writes icon_a/b/c.png (512), *_128.png, *_64.png and preview.png next to this file.
Everything is rendered at 4x (2048 px) and downscaled with Lanczos.
"""
import math
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
FINAL = 512
SS = 4
S = FINAL * SS
rng = np.random.default_rng(7)


def blur(arr, r):
    """Gaussian blur of a float HxW array (0..1) via PIL."""
    im = Image.fromarray(np.clip(arr * 255, 0, 255).astype(np.uint8), "L")
    im = im.filter(ImageFilter.GaussianBlur(r))
    return np.asarray(im).astype(np.float32) / 255.0


def stroke(d, pts, w):
    """Smooth thick polyline: round stamps along it."""
    r = w / 2
    for (x0, y0), (x1, y1) in zip(pts[:-1], pts[1:]):
        n = max(2, int(math.hypot(x1 - x0, y1 - y0) / (r * 0.35)))
        for i in range(n + 1):
            t = i / n
            x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            d.ellipse([x - r, y - r, x + r, y + r], fill=255)


def mask_from(fn):
    im = Image.new("L", (S, S), 0)
    fn(ImageDraw.Draw(im))
    return np.asarray(im).astype(np.float32) / 255.0


def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def add_glow(canvas, m, color, w, gain=1.0, core=(1, 1, 1), core_mix=0.75):
    """Add a neon-tube look for mask m (stroke width w px at S scale)."""
    col = np.array(color, np.float32)
    radii = [(w * 0.35, 0.9), (w * 1.1, 0.7), (w * 2.8, 0.55), (w * 7, 0.4), (w * 16, 0.22)]
    for r, g in radii:
        b = blur(m, r)
        canvas += b[..., None] * col * g * gain
    canvas += m[..., None] * (col * (1 - core_mix) + np.array(core, np.float32) * core_mix) * 1.2


def finish(canvas, name, exposure=1.0):
    img = 1 - np.exp(-np.maximum(canvas, 0) * exposure)  # soft shoulder
    img = img ** (1 / 1.15)
    img += rng.normal(0, 0.004, img.shape)  # dither against banding
    im = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8), "RGB")
    im = im.resize((FINAL, FINAL), Image.LANCZOS)
    im.save(os.path.join(OUT, f"icon_{name}.png"))
    im.resize((128, 128), Image.LANCZOS).save(os.path.join(OUT, f"icon_{name}_128.png"))
    im.resize((64, 64), Image.LANCZOS).save(os.path.join(OUT, f"icon_{name}_64.png"))
    return im


def stars(canvas, n, ymax=1.0, bright=1.0, mask=None):
    m = np.zeros((S, S), np.float32)
    xs = rng.uniform(0, S, n)
    ys = rng.uniform(0, S * ymax, n)
    big = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(big)
    for x, y in zip(xs, ys):
        r = rng.choice([2.5, 3.5, 5, 7], p=[0.5, 0.3, 0.15, 0.05]) * SS / 2
        v = int(rng.uniform(110, 255))
        d.ellipse([x - r, y - r, x + r, y + r], fill=v)
    m = np.asarray(big).astype(np.float32) / 255.0
    if mask is not None:
        m = m * mask
    tint = np.array([0.85, 0.9, 1.0], np.float32)
    canvas += m[..., None] * tint * bright
    canvas += blur(m, 6)[..., None] * tint * bright * 0.6


# ---------------------------------------------------------------- A
def icon_a():
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    x = (xx - S / 2) / S
    y = (yy - S / 2) / S
    canvas = np.zeros((S, S, 3), np.float32)
    # deep space gradient
    rr = np.sqrt(x * x + y * y)
    canvas += np.stack([0.010 + 0.05 * np.exp(-rr * 5),
                        0.012 + 0.025 * np.exp(-rr * 5),
                        0.03 + 0.07 * np.exp(-rr * 4)], -1)
    # keep stars away from the middle so the disc reads
    stars(canvas, 70, bright=0.9, mask=smooth(0.20, 0.38, rr))

    ang = math.radians(-14)
    ca, sa = math.cos(ang), math.sin(ang)
    xr = x * ca + y * sa
    yr = -x * sa + y * ca
    R = 0.135  # hole radius (shadow)
    tilt = 0.27

    def ramp(t):
        """t 0 (outer, cold red) -> 1 (inner, white-hot). returns rgb."""
        t = np.clip(t, 0, 1)[..., None]
        c0 = np.array([0.55, 0.10, 0.02], np.float32)
        c1 = np.array([1.00, 0.45, 0.06], np.float32)
        c2 = np.array([1.00, 0.80, 0.40], np.float32)
        c3 = np.array([1.00, 0.97, 0.90], np.float32)
        a = np.clip(t * 3, 0, 1)
        b = np.clip(t * 3 - 1, 0, 1)
        c = np.clip(t * 3 - 2, 0, 1)
        return c0 * (1 - a) + c1 * a + (c2 - c1) * b + (c3 - c2) * c

    # flat disc (ellipse, seen at an angle)
    d = np.sqrt(xr ** 2 + (yr / tilt) ** 2)
    din, dout = R * 1.2, R * 3.35
    t = 1 - (d - din) / (dout - din)
    prof = smooth(0.0, 0.10, t) * np.clip(t, 0, 1) ** 1.0
    prof *= smooth(dout, dout * 0.8, d) if False else 1
    prof = np.where((d > din * 0.98) & (d < dout), prof, 0)
    # streaks along the orbit + doppler boost on the approaching (left) side
    th = np.arctan2(yr / tilt, xr)
    streak = 0.72 + 0.28 * np.sin(th * 3 + d * 55) * np.sin(th * 7 - d * 31 + 1.3)
    dopp = 1.0 + 0.8 * (-xr / np.maximum(d, 1e-3)) * 0.8
    disc = prof * streak * np.clip(dopp, 0.3, 1.9)
    disc_col = ramp(t * 1.05)

    # lensed image of the far side: bright arc over the hole (circular band)
    dc = np.sqrt(xr ** 2 + yr ** 2)
    band = np.exp(-((dc - R * 1.32) / (R * 0.30)) ** 2)
    top_w = 0.30 + 0.70 * smooth(0.0, R * 1.2, -yr)  # strong above, dim below
    band_i = band * top_w
    band_col = ramp(0.55 + 0.45 * band)

    # photon ring right at the shadow edge
    ring = np.exp(-((dc - R * 1.04) / (R * 0.055)) ** 2)

    hole = smooth(R * 1.03, R * 0.985, dc)  # 1 inside
    front = (yr > 0)[..., None].astype(np.float32)
    front_soft = smooth(-0.004, 0.004, yr)[..., None]

    col = np.zeros_like(canvas)
    col += band_col * band_i[..., None] * 1.9
    col += np.array([1, 0.95, 0.85], np.float32) * ring[..., None] * 1.8
    # back half of the disc is hidden by the hole, front half passes over it
    disc_rgb = disc_col * disc[..., None] * 2.3
    vis_back = (1 - hole)[..., None] * (1 - front_soft) + front_soft
    col += disc_rgb * vis_back
    # hole itself eats everything that is not the front disc
    col = col * ((1 - hole)[..., None] + hole[..., None] * front_soft)
    # glow built from the light itself
    lum = col.max(-1)
    glow = np.zeros_like(canvas)
    for r, g in [(S * 0.008, 0.9), (S * 0.025, 0.7), (S * 0.07, 0.55), (S * 0.16, 0.35)]:
        for c in range(3):
            glow[..., c] += blur(np.clip(col[..., c], 0, 1), r) * g
    warm = np.array([1.0, 0.55, 0.25], np.float32)
    glow *= warm * 0.55 + 0.45
    # keep bloom from filling the hole: sharp black shadow stays dark in the centre
    shadow = smooth(R * 1.0, R * 0.7, dc) * (1 - front_soft[..., 0])
    glow *= (1 - shadow[..., None] * 0.95)
    canvas += col + glow * 0.6
    return finish(canvas, "a", exposure=0.95)


# ---------------------------------------------------------------- B
def icon_b():
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    u = xx / S
    v = yy / S
    canvas = np.zeros((S, S, 3), np.float32)
    vy = 0.40
    vx = 0.52
    # night sky: deep indigo, a violet/blue glow at the horizon
    sky = smooth(vy + 0.02, 0.0, v)
    canvas += np.stack([0.012 + 0.03 * (1 - sky), 0.014 + 0.035 * (1 - sky), 0.04 + 0.10 * (1 - sky)], -1)
    hg = np.exp(-((v - vy) / 0.06) ** 2) * np.exp(-((u - vx) / 0.35) ** 2)
    canvas += hg[..., None] * np.array([0.10, 0.18, 0.55], np.float32)
    stars(canvas, 55, ymax=0.36, bright=0.9)
    # ground
    ground = smooth(vy - 0.004, vy + 0.004, v)[..., None]
    gcol = np.stack([0.006 + 0.01 * v, 0.008 + 0.012 * v, 0.02 + 0.02 * v], -1)
    canvas = canvas * (1 - ground) + gcol * ground

    H = 0.68          # screen span of depth: y = vy + H*k
    W = 0.78          # half road width at k=1
    C = 0.20          # bend

    def road(k):
        """k = 1/z >0: returns centre x,y (0..1) and half-width."""
        yk = vy + H * k
        cx = vx - C * max(k * (1 - k), 0.0) * 0.7  # gentle bend, straight near the viewer
        return cx, yk, W * k

    ks = np.concatenate([np.linspace(0.004, 0.12, 500), np.linspace(0.12, 1.6, 1200)])

    # road surface: faint blue sheen toward the edges (neon spill on tarmac)
    surf = Image.new("L", (S, S), 0)
    sd = ImageDraw.Draw(surf)
    lpts = [(road(k)[0] - road(k)[2], road(k)[1]) for k in ks]
    rpts = [(road(k)[0] + road(k)[2], road(k)[1]) for k in ks]
    sd.polygon([(x * S, y * S) for x, y in lpts + rpts[::-1]], fill=255)
    road_m = np.asarray(surf).astype(np.float32) / 255.0
    rm = road_m[..., None]
    shade = np.stack([0.016 + 0.01 * v, 0.02 + 0.014 * v, 0.045 + 0.03 * v], -1)
    canvas = canvas * (1 - rm * 0.92) + shade * rm * 0.92

    def border(side):
        def f(d):
            for k in ks:
                cx, yk, hw = road(k)
                x0 = cx + side * hw
                r = max(1.0, 0.0125 * k * S * 1.3)
                d.ellipse([x0 * S - r, yk * S - r, x0 * S + r, yk * S + r], fill=255)
        return mask_from(f)

    blue = (0.10, 0.45, 1.0)
    cyan = (0.2, 0.85, 1.0)
    mL, mR = border(-1), border(1)
    wpx = 0.011 * S
    # glow only over the road side and sky; hide below horizon nothing needed
    add_glow(canvas, mL, blue, wpx * 1.1, gain=1.2, core=(0.75, 0.92, 1.0), core_mix=0.6)
    add_glow(canvas, mR, blue, wpx * 1.1, gain=1.2, core=(0.75, 0.92, 1.0), core_mix=0.6)

    # centre dashes: cross-bars of dim cyan light in perspective
    def dashes(d):
        k = 1.9
        while k > 0.07:
            k0, k1 = k, k * 0.86
            cx0, y0, hw0 = road(k0)
            cx1, y1, hw1 = road(k1)
            w0, w1 = 0.010 * k0, 0.010 * k1
            d.polygon([((cx0 - w0) * S, y0 * S), ((cx0 + w0) * S, y0 * S),
                       ((cx1 + w1) * S, y1 * S), ((cx1 - w1) * S, y1 * S)], fill=255)
            k *= 0.70
    dm = mask_from(dashes)
    add_glow(canvas, dm, (0.25, 0.55, 1.0), wpx * 0.5, gain=0.45, core=(0.7, 0.85, 1.0), core_mix=0.5)

    # neon trail: a cyan->magenta ribbon running into the distance in the right lane
    def trail(d):
        pts = []
        for k in np.linspace(0.02, 1.35, 500):
            cx, yk, hw = road(k)
            pts.append((cx + 0.30 * hw, yk, k))
        for i in range(len(pts) - 1):
            x0, y0, k0 = pts[i]
            r = max(1.0, 0.010 * k0 * S)
            d.ellipse([x0 * S - r, y0 * S - r, x0 * S + r, y0 * S + r], fill=255)
    tm = mask_from(trail)
    # fade the trail towards the viewer
    fade = smooth(1.05, 0.55, (yy / S - vy) / H)
    add_glow(canvas, tm * fade, (1.0, 0.25, 0.75), wpx * 0.9, gain=1.0, core=(1.0, 0.85, 0.95))

    # horizon bloom from the vanishing point
    vp = np.exp(-(((u - vx) / 0.05) ** 2 + ((v - vy) / 0.018) ** 2))
    canvas += vp[..., None] * np.array([0.5, 0.75, 1.0], np.float32) * 1.1
    # vignette
    rr = np.sqrt((u - 0.5) ** 2 + (v - 0.5) ** 2)
    canvas *= (1 - 0.55 * smooth(0.35, 0.75, rr))[..., None]
    return finish(canvas, "b", exposure=1.0)


# ---------------------------------------------------------------- C
def s_path(cx, cy, r, xs=0.92):
    pts = []
    for th in np.linspace(math.radians(25), math.radians(270), 90):
        pts.append((cx + xs * r * math.cos(th), cy - r - r * math.sin(th) * 1.0 + 0))
    # upper circle centre (cx, cy-r): point = (cx + r cos, cy - r - r sin) ends at bottom (cx, cy)
    # NOTE sin sign: top is th=90 -> cy-2r (top), th=270 -> cy
    for ph in np.linspace(math.radians(90), math.radians(-155), 100):
        pts.append((cx + xs * r * math.cos(ph), cy + r - r * math.sin(ph)))
    return pts


def icon_c():
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    u = xx / S
    v = yy / S
    canvas = np.zeros((S, S, 3), np.float32)
    rr = np.sqrt((u - 0.5) ** 2 + (v - 0.5) ** 2)
    canvas += np.stack([0.012 + 0.03 * np.exp(-rr * 5), 0.012 + 0.02 * np.exp(-rr * 5),
                        0.03 + 0.07 * np.exp(-rr * 4)], -1)
    # faint perspective grid floor in the lower third
    gm = Image.new("L", (S, S), 0)
    gd = ImageDraw.Draw(gm)
    hy = 0.78
    for i in range(-12, 13):
        gd.line([(0.5 * S, hy * S), ((0.5 + i * 0.22) * S, S * 1.05)], fill=255, width=SS)
    for j in range(1, 9):
        yv = hy + (1.05 - hy) * (j / 8) ** 2
        gd.line([(0, yv * S), (S, yv * S)], fill=255, width=SS)
    gmask = np.asarray(gm).astype(np.float32) / 255.0
    gmask *= smooth(hy, 1.0, v) * 0.5
    canvas += gmask[..., None] * np.array([0.05, 0.25, 0.8], np.float32) * 0.5
    canvas += blur(gmask, 6)[..., None] * np.array([0.05, 0.25, 0.8], np.float32) * 0.5

    tw = int(0.060 * S)  # tube width

    # rounded frame in dim blue
    def frame(d):
        d.rounded_rectangle([0.06 * S, 0.06 * S, 0.94 * S, 0.94 * S], radius=0.12 * S,
                            outline=255, width=int(0.012 * S))
    fm = mask_from(frame)
    add_glow(canvas, fm, (0.08, 0.30, 0.9), 0.012 * S, gain=0.55, core=(0.6, 0.8, 1.0), core_mix=0.5)

    def vshape(d):
        pts = [(0.15 * S, 0.27 * S), (0.31 * S, 0.70 * S), (0.47 * S, 0.27 * S)]
        stroke(d, pts, tw)
    vm = mask_from(vshape)
    add_glow(canvas, vm, (0.0, 0.50, 1.0), tw, gain=1.0, core=(0.8, 0.95, 1.0), core_mix=0.5)

    def sshape(d):
        pts = [(x * S, y * S) for x, y in s_path(0.69, 0.43, 0.115, 0.95)]
        stroke(d, pts, tw)
    sm = mask_from(sshape)
    add_glow(canvas, sm, (1.0, 0.05, 0.60), tw, gain=1.0, core=(1.0, 0.8, 0.9), core_mix=0.5)

    # little reflection of both letters on the floor
    refl = np.zeros((S, S), np.float32)
    for m in (vm, sm):
        refl = np.maximum(refl, m)
    refl = np.flipud(np.roll(refl, int(S * 0.0), axis=0))
    shift = int(S * (0.74 - 0.0))
    refl = np.roll(refl, shift - S + int(0.15 * S), axis=0)
    rf = blur(refl, 10) * smooth(0.8, 0.95, v) * (1 - smooth(0.9, 1.0, v)) * 0.0
    canvas += rf[..., None] * 0.0

    canvas *= (1 - 0.4 * smooth(0.45, 0.78, rr))[..., None]
    return finish(canvas, "c", exposure=1.0)


def preview(ims):
    pad, lab = 40, 70
    w = 3 * 512 + 4 * pad
    h = pad + 512 + 30 + 64 + pad + lab
    sheet = Image.new("RGB", (w, h), (24, 26, 32))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf", 40)
    except Exception:
        font = ImageFont.load_default()
    for i, (name, im) in enumerate(zip("ABC", ims)):
        x = pad + i * (512 + pad)
        sheet.paste(im, (x, pad))
        sheet.paste(im.resize((64, 64), Image.LANCZOS), (x + 512 // 2 - 32, pad + 512 + 30))
        sheet.paste(im.resize((128, 128), Image.LANCZOS).resize((64, 64), Image.LANCZOS), (-1000, -1000))
        d.text((x + 256, pad + 512 + 30 + 64 + 20), name, fill=(235, 235, 240), font=font, anchor="mt")
    sheet.save(os.path.join(OUT, "preview.png"))


if __name__ == "__main__":
    ims = [icon_a(), icon_b(), icon_c()]
    preview(ims)
    print("done")
