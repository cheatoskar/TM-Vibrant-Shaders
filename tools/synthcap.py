"""Writes a synthetic .tmcap (TMC2) for offline tests without the game.

    python tools/synthcap.py out.tmcap [--night] [--size 1280x720]

A ray-traced scene with exact depth, in the game's conventions (GOTCHA.md): ground at
y = 9.001, a water pool at y = 8.00, boxes (AO, shadows), emissive lamp panels (bloom,
neon spill) and a sky at raw depth 1. Projection: infinite far plane, near 0.2
(d = 1 - 0.2 / z). Real captures stay better for looks; this one is for "does it run, does
it change what it should".
"""
import argparse
import math
import struct

import numpy as np

NEAR = 0.2
GROUND_Y = 9.001
WATER_Y = 8.0


def normalize(v):
    return v / np.linalg.norm(v, axis=-1, keepdims=True)


def boxes_hit(origin, dirs, boxes):
    """Nearest hit of rays with axis-aligned boxes: distance, box index, face normal."""
    t_best = np.full(dirs.shape[:-1], np.inf)
    index = np.full(dirs.shape[:-1], -1)
    normal = np.zeros(dirs.shape)
    with np.errstate(divide="ignore", invalid="ignore"):
        inv = 1.0 / dirs
        for k, (lo, hi, _) in enumerate(boxes):
            t0 = (np.array(lo) - origin) * inv
            t1 = (np.array(hi) - origin) * inv
            tmin = np.minimum(t0, t1)
            tmax = np.maximum(t0, t1)
            near = tmin.max(axis=-1)
            far = tmax.min(axis=-1)
            hit = (near <= far) & (near > NEAR) & (near < t_best)
            t_best = np.where(hit, near, t_best)
            index = np.where(hit, k, index)
            axis = tmin.argmax(axis=-1)
            n = np.zeros(dirs.shape)
            np.put_along_axis(n, axis[..., None], -np.sign(np.take_along_axis(dirs, axis[..., None], -1)), -1)
            normal = np.where(hit[..., None], n, normal)
    return t_best, index, normal


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--night", action="store_true")
    ap.add_argument("--size", default="1280x720")
    args = ap.parse_args()
    width, height = (int(v) for v in args.size.split("x"))

    # Camera: 12 m up, looking along +z, 12 degrees down, 70 degrees vertical FOV.
    eye = np.array([0.0, 12.0, -20.0])
    pitch = math.radians(-12.0)
    forward = np.array([0.0, math.sin(pitch), math.cos(pitch)])
    right = np.array([1.0, 0.0, 0.0])
    up = np.cross(forward, right)
    p11 = 1.0 / math.tan(math.radians(35.0))
    p00 = p11 * height / width

    # D3D row vectors: v_view = v_world * V. Columns of the rotation part are right, up, forward.
    view = np.identity(4)
    view[:3, 0], view[:3, 1], view[:3, 2] = right, up, forward
    view[3, :3] = [-eye @ right, -eye @ up, -eye @ forward]
    proj = np.zeros((4, 4))
    proj[0, 0], proj[1, 1], proj[2, 2], proj[2, 3], proj[3, 2] = p00, p11, 1.0, 1.0, -NEAR

    # Rays through the pixels (D3D9: pixel i at ndc 2 i / W - 1).
    i = np.arange(width)
    j = np.arange(height)
    x = 2.0 * i / width - 1.0
    y = 1.0 - 2.0 * j / height
    xv, yv = np.meshgrid(x / p00, y / p11)
    dirs_view = np.stack([xv, yv, np.ones_like(xv)], -1)
    dirs = dirs_view[..., 0:1] * right + dirs_view[..., 1:2] * up + dirs_view[..., 2:3] * forward
    dirs_n = normalize(dirs)

    sun = normalize(np.array([0.45, 0.75, 0.5])) if not args.night else normalize(np.array([-0.3, 0.6, 0.7]))
    sun_color = np.array([1.0, 0.97, 0.92]) if not args.night else np.array([0.33, 0.30, 0.44])

    # Boxes: (min, max, colour); the last two are lamp panels (emissive).
    boxes = [
        ((-14.0, GROUND_Y, 20.0), (-8.0, GROUND_Y + 10.0, 30.0), (0.75, 0.73, 0.70)),
        ((6.0, GROUND_Y, 35.0), (12.0, GROUND_Y + 4.0, 41.0), (0.55, 0.55, 0.60)),
        ((-3.0, GROUND_Y, 60.0), (3.0, GROUND_Y + 16.0, 64.0), (0.80, 0.30, 0.25)),
        ((-1.5, GROUND_Y, 14.0), (1.5, GROUND_Y + 1.2, 18.0), (0.15, 0.25, 0.65)),
        ((-8.02, GROUND_Y + 6.0, 22.0), (-7.98, GROUND_Y + 7.0, 26.0), (6.0, 5.6, 4.8)),
        ((5.98, GROUND_Y + 2.5, 36.0), (6.02, GROUND_Y + 3.0, 40.0), (0.2, 3.0, 6.0)),
    ]
    lamps = {4, 5}
    t_box, k_box, n_box = boxes_hit(eye, dirs, boxes)

    with np.errstate(divide="ignore", invalid="ignore"):
        t_ground = np.where(dirs[..., 1] < 0, (GROUND_Y - eye[1]) / dirs[..., 1], np.inf)
        t_water = np.where(dirs[..., 1] < 0, (WATER_Y - eye[1]) / dirs[..., 1], np.inf)
    p_water = eye + dirs * t_water[..., None]
    in_pool = (np.abs(p_water[..., 0] - 9.0) < 5.0) & (p_water[..., 2] > 12.0) & (p_water[..., 2] < 26.0)
    t_ground = np.where(np.isfinite(t_water) & in_pool & (t_water < t_box), np.inf, t_ground)
    t_water = np.where(in_pool, t_water, np.inf)

    t = np.minimum(np.minimum(t_box, t_ground), t_water)
    hit_box = (t == t_box) & np.isfinite(t)
    hit_ground = (t == t_ground) & np.isfinite(t) & ~hit_box
    hit_water = (t == t_water) & np.isfinite(t) & ~hit_box & ~hit_ground
    sky = ~np.isfinite(t)
    p = eye + dirs * np.where(np.isfinite(t), t, 0.0)[..., None]

    # Shading: the game bakes light into the colour, so do a simple N.L + ambient here.
    color = np.zeros(dirs.shape)
    checker = ((np.floor(p[..., 0] / 4.0) + np.floor(p[..., 2] / 4.0)) % 2)[..., None]
    road = (np.abs(p[..., 0]) < 4.0)[..., None]
    ground = np.where(road, 0.30 + 0.04 * checker, np.array([0.28, 0.45, 0.18]) * (0.9 + 0.1 * checker))
    ndl_ground = max(sun[1], 0.0)
    color = np.where(hit_ground[..., None], ground * (0.35 + 0.8 * ndl_ground * sun_color), color)
    box_colors = np.array([b[2] for b in boxes])
    albedo = box_colors[np.clip(k_box, 0, None)]
    ndl = np.clip((n_box * sun).sum(-1, keepdims=True), 0.0, None)
    lit = albedo * (0.35 + 0.8 * ndl * sun_color)
    is_lamp = np.isin(k_box, list(lamps))[..., None]
    color = np.where(hit_box[..., None], np.where(is_lamp, np.clip(albedo / 6.0, 0, 1), lit), color)
    color = np.where(hit_water[..., None], np.array([0.10, 0.22, 0.28]), color)
    sky_top = np.array([0.25, 0.45, 0.85]) if not args.night else np.array([0.02, 0.03, 0.07])
    sky_low = np.array([0.75, 0.85, 0.95]) if not args.night else np.array([0.08, 0.08, 0.14])
    h = np.clip(dirs_n[..., 1:2], 0.0, 1.0) ** 0.5
    color = np.where(sky[..., None], sky_low + (sky_top - sky_low) * h, color)
    color = np.clip(color, 0.0, 1.0)

    z_view = (dirs * np.where(np.isfinite(t), t, 0.0)[..., None]) @ forward
    depth = np.where(sky, 1.0, 1.0 - NEAR / np.maximum(z_view, NEAR)).astype(np.float32)

    bgra = (color[..., ::-1] * 255.0 + 0.5).astype(np.uint32)
    packed = (bgra[..., 0] | (bgra[..., 1] << 8) | (bgra[..., 2] << 16) | (255 << 24)).astype(np.uint32)

    with open(args.out, "wb") as f:
        f.write(b"TMC2")
        f.write(struct.pack("<II", width, height))
        f.write(struct.pack("<16f", *view.flatten()))
        f.write(struct.pack("<16f", *proj.flatten()))
        f.write(struct.pack("<4f", *sun, 1.0))
        f.write(struct.pack("<f", 12.0))
        f.write(struct.pack("<4f", *sun_color, 1.0))
        f.write(packed.tobytes())
        f.write(depth.tobytes())
    print("wrote %s (%dx%d, %s)" % (args.out, width, height, "night" if args.night else "day"))


if __name__ == "__main__":
    main()
