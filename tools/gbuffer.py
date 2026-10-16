"""The game's own normals from F12 (GameGBuffer on): capture_NNN.nrm16 against the depth.

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

capture_NNN.nrm16: "TMH1", width, height (uint32 each), then RGBA half floats per pixel, rows
top-down: the world normal (xyz) and the distance from the eye (w) that the game's patched
pixel shader wrote; 0 where none drew. The test is the one PS_PrepareGBuffer makes: the
distance within 1 % + 3 cm of the depth's and a unit normal.
"""
import struct
import sys

import numpy as np


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


def read_normals(path, w, h):
    with open(path, "rb") as f:
        magic, fw, fh = struct.unpack("<3I", f.read(12))
        if magic != 0x31484D54 or fw != w or fh != h:
            sys.exit("%s: not a TMH1 file of %dx%d" % (path, w, h))
        return np.frombuffer(f.read(w * h * 8), np.float16).reshape(h, w, 4).astype(np.float64)


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    synth = "--synth" in args
    cap = [a for a in args if a.endswith(".tmcap")][0]
    w, h, view, proj, depth = read_capture(cap)
    p, sky = view_positions(w, h, proj, depth)
    stem = cap[: -len(".tmcap")]
    if synth:
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
        with open(stem + ".nrm16", "wb") as f:
            f.write(struct.pack("<3I", 0x31484D54, w, h))
            f.write(rgba.astype(np.float16).tobytes())
        print("wrote %s.nrm16 (from the depth: a plumbing test, no normal map in it)" % stem)
        return
    g = read_normals(stem + ".nrm16", w, h)
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
