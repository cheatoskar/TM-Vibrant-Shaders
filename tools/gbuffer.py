"""The game's own normals and light from F12 (GameGBuffer / GameLight on), against the depth.

    python tools/gbuffer.py capture_090.tmcap                  how much of the image has the game's
                                                               normal, and how much of it the pipeline
                                                               takes (it belongs to the surface seen)
    python tools/gbuffer.py capture_090.tmcap --map gbuf.png   left: the game's normals (world, as
                                                               colour), right: green taken, red
                                                               written but rejected (drawn over later
                                                               by a shader without normals, or not a
                                                               unit vector), black none, blue sky
    python tools/gbuffer.py --synth capture_083.tmcap          writes capture_083.nrm16 from the depth
                                                               (plumbing test: no normal map in it)
    python tools/gbuffer.py --light capture_097.tmcap [--map light.png]
                                                               the same for capture_097.lit16
                                                               (GameLight): how much of the image has
                                                               the game's light and how bright it is
                                                               (where the night maps' lamps start);
                                                               map: the light | taken / rejected
    python tools/gbuffer.py --synth-light capture_083.tmcap    writes capture_083.lit16: grey light from
                                                               the blurred float image (.hdr16 less
                                                               .add16) and the depth's distance
                                                               (plumbing test of the pipeline's
                                                               night on the light, not the game's)

capture_NNN.nrm16: "TMH1", width, height (uint32 each), then RGBA half floats per pixel, rows
top-down: the world normal (xyz) and the distance from the eye (w) that the game's patched
pixel shader wrote; 0 where none drew. The test is the one PS_PrepareGBuffer makes: the
distance within 1 % + 3 cm of the depth's and a unit normal.
capture_NNN.lit16, the same format: the light the game's shader multiplied the surface's colour
with (rgb; its debug view's "light diffuse") and the distance (w). The pipeline takes it where
the distance matches (lampLight in tmvs.hlsl).
"""
import struct
import sys

import numpy as np

# lampLight (tmvs.hlsl): light above this (the brightest channel) starts to count as the game's
# lamps, fully at the second (1 = the paint as it is).
LAMP = (1.0, 1.6)


def read_capture(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        w, h = struct.unpack("<II", f.read(8))
        view = np.array(struct.unpack("<16f", f.read(64)), np.float64).reshape(4, 4)
        proj = np.array(struct.unpack("<16f", f.read(64)), np.float64).reshape(4, 4)
        f.read(16 + 4)
        if magic == b"TMC2":
            f.read(16)
        f.seek(w * h * 4, 1)
        depth = np.frombuffer(f.read(w * h * 4), np.float32).reshape(h, w).astype(np.float64)
    return w, h, view, proj, depth


def view_positions(w, h, proj, depth):
    """View-space position of every pixel, as the pipeline reconstructs it (sky: inf)."""
    sky = depth >= 0.999999
    z = np.where(sky, 1.0, proj[3, 2] / (depth - proj[2, 2]))
    ndcx = 2.0 * np.arange(w)[None, :] / w - 1.0
    ndcy = 1.0 - 2.0 * np.arange(h)[:, None] / h
    x = (ndcx - proj[2, 0]) * z / proj[0, 0]
    y = (ndcy - proj[2, 1]) * z / proj[1, 1]
    return np.stack([np.broadcast_to(x, z.shape), np.broadcast_to(y, z.shape), z], -1), sky


def read_half(path, w, h):
    with open(path, "rb") as f:
        magic, fw, fh = struct.unpack("<3I", f.read(12))
        if magic != 0x31484D54 or fw != w or fh != h:
            sys.exit("%s: not a TMH1 file of %dx%d" % (path, w, h))
        return np.frombuffer(f.read(w * h * 8), np.float16).reshape(h, w, 4).astype(np.float64)


def write_half(path, rgba):
    h, w = rgba.shape[:2]
    with open(path, "wb") as f:
        f.write(struct.pack("<3I", 0x31484D54, w, h))
        f.write(rgba.astype(np.float16).tobytes())


def blur(img, radius):
    """Box blur, three passes (about Gaussian), along both axes."""
    out = img.copy()
    for axis in (0, 1):
        for _ in range(3):
            c = np.cumsum(np.pad(out, [(radius + 1, radius) if a == axis else (0, 0) for a in range(out.ndim)], mode="edge"), axis=axis)
            hi = np.take(c, np.arange(2 * radius + 1, c.shape[axis]), axis=axis)
            lo = np.take(c, np.arange(0, c.shape[axis] - 2 * radius - 1), axis=axis)
            out = (hi - lo) / (2 * radius + 1)
    return out


def taken_mask(g, dist, sky):
    return (g[..., 3] > 0) & ~sky & (np.abs(g[..., 3] - dist) < 0.01 * dist + 0.03)


def light_report(stem, w, h, p, sky, args):
    g = read_half(stem + ".lit16", w, h)
    dist = np.linalg.norm(p, axis=-1)
    written = g[..., 3] > 0
    peak = g[..., :3].max(-1)
    luma = g[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
    taken = taken_mask(g, dist, sky) & (luma > 0.002)
    seen = ~sky
    print("%s: %dx%d" % (stem + ".lit16", w, h))
    print("  sky                         %5.1f %%" % (100.0 * sky.mean()))
    print("  the game's light written    %5.1f %% of the image, %5.1f %% of the ground and objects" %
          (100.0 * written.mean(), 100.0 * (written & seen).sum() / max(seen.sum(), 1)))
    print("  taken by the pipeline       %5.1f %% of the image (%5.1f %% of the written)" %
          (100.0 * taken.mean(), 100.0 * taken.sum() / max(written.sum(), 1)))
    if taken.any():
        q = np.percentile(peak[taken], [5, 25, 50, 75, 95, 99])
        print("  brightest channel where taken: p5 %.3f  p25 %.3f  median %.3f  p75 %.3f  p95 %.3f  p99 %.3f  max %.3f" %
              (tuple(q) + (peak[taken].max(),)))
        lamp = np.clip((peak - LAMP[0]) / (LAMP[1] - LAMP[0]), 0, 1)
        lamp = lamp * lamp * (3 - 2 * lamp)
        print("  counted as lamp light (lampLight %.2f..%.2f): partly %5.1f %%, fully %5.1f %% of the taken pixels" %
              (LAMP[0], LAMP[1], 100.0 * (lamp[taken] > 0).mean(), 100.0 * (lamp[taken] >= 1).mean()))
        hue = g[..., :3][taken] / np.maximum(luma[taken][:, None], 1e-6)
        print("  mean colour of the light (rgb / luma): %.2f %.2f %.2f" % tuple(hue.mean(0)))
    if "--map" in args:
        from PIL import Image

        out = args[args.index("--map") + 1]
        light = np.clip(g[..., :3] / (1.0 + g[..., :3]) * 2.0, 0, 1) ** (1 / 2.2) * 255 * written[..., None]
        state = np.zeros((h, w, 3))
        state[taken] = (40, 200, 60)
        state[written & ~taken] = (220, 40, 40)
        state[sky] = (40, 60, 140)
        Image.fromarray(np.concatenate([light, state], 1).astype(np.uint8)).save(out)
        print("  wrote %s (left: the light, x/(1+x) x 2; right: green taken, red rejected, blue sky)" % out)


def synth_light(stem, w, h, p, sky):
    """Grey light from the blurred surface brightness: what lampLight sees on a real capture
    looks like this in its levels (lamp pools bright, moonlit ground dim), not in its detail."""
    hdr = read_half(stem + ".hdr16", w, h)[..., :3]
    try:
        hdr = np.maximum(hdr - read_half(stem + ".add16", w, h)[..., :3], 0)
    except FileNotFoundError:
        pass
    luma = hdr @ np.array([0.2126, 0.7152, 0.0722])
    # Paint is about 0.5: the light is about twice the surface's brightness, smoothed so that
    # the paint's pattern stays in the paint.
    small = luma[::4, ::4]
    smooth = blur(np.where(sky[::4, ::4], 0.0, small), 6)
    weight = blur(np.where(sky[::4, ::4], 0.0, 1.0), 6)
    light = np.repeat(np.repeat(2.0 * smooth / np.maximum(weight, 1e-3), 4, 0), 4, 1)[:h, :w]
    rgba = np.zeros((h, w, 4))
    rgba[..., :3] = light[..., None]
    rgba[..., 3] = np.linalg.norm(p, axis=-1)
    rgba[sky] = 0
    write_half(stem + ".lit16", rgba)
    print("wrote %s.lit16 (synthetic grey light from the blurred float image: a plumbing test, not the game's light)" % stem)
    print("  light p50 %.3f p95 %.3f max %.3f" % tuple(np.percentile(light[~sky], [50, 95, 100])))


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    cap = [a for a in args if a.endswith(".tmcap")][0]
    w, h, view, proj, depth = read_capture(cap)
    p, sky = view_positions(w, h, proj, depth)
    stem = cap[: -len(".tmcap")]
    if "--synth-light" in args:
        synth_light(stem, w, h, p, sky)
        return
    if "--light" in args:
        light_report(stem, w, h, p, sky, args)
        return
    if "--synth" in args:
        dx = np.zeros_like(p)
        dy = np.zeros_like(p)
        dx[:, 1:-1] = p[:, 2:] - p[:, :-2]
        dy[1:-1] = p[2:] - p[:-2]
        n = np.cross(dx, dy)
        n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)
        n = np.where((np.sum(n * p, -1) > 0)[..., None], -n, n)
        rgba = np.zeros((h, w, 4))
        rgba[..., :3] = n @ view[:3, :3].T
        rgba[..., 3] = np.linalg.norm(p, axis=-1)
        rgba[sky] = 0
        write_half(stem + ".nrm16", rgba)
        print("wrote %s.nrm16 (from the depth: a plumbing test, no normal map in it)" % stem)
        return
    g = read_half(stem + ".nrm16", w, h)
    written = g[..., 3] > 0
    dist = np.linalg.norm(p, axis=-1)
    length = np.linalg.norm(g[..., :3], axis=-1)
    taken = written & ~sky & (np.abs(g[..., 3] - dist) < 0.01 * dist + 0.03) & (np.abs(length - 1.0) < 0.05)
    seen = ~sky
    print("%s: %dx%d" % (stem + ".nrm16", w, h))
    print("  sky                         %5.1f %%" % (100.0 * sky.mean()))
    print("  the game's normal written   %5.1f %% of the image, %5.1f %% of the ground and objects" %
          (100.0 * written.mean(), 100.0 * (written & seen).sum() / max(seen.sum(), 1)))
    print("  taken by the pipeline       %5.1f %% of the image (%5.1f %% of the written)" %
          (100.0 * taken.mean(), 100.0 * taken.sum() / max(written.sum(), 1)))
    off = written & ~taken
    if off.any():
        ratio = g[..., 3][off] / np.maximum(dist[off], 1e-6)
        print("  rejected: its distance / the depth's  median %.3f; %.1f %% lie behind the surface seen (drawn over later)" %
              (np.median(ratio), 100.0 * (ratio > 1.0).mean()))
    if "--map" in args:
        from PIL import Image

        out = args[args.index("--map") + 1]
        nrm = np.clip((g[..., :3] * 0.5 + 0.5) * 255, 0, 255) * written[..., None]
        state = np.zeros((h, w, 3))
        state[taken] = (40, 200, 60)
        state[off] = (220, 40, 40)
        state[sky] = (40, 60, 140)
        Image.fromarray(np.concatenate([nrm, state], 1).astype(np.uint8)).save(out)
        print("  wrote %s" % out)


if __name__ == "__main__":
    main()
