"""World height (y) of screen points in a .tmcap: what is the water level, where is the ground?

    python tools/capheight.py capture.tmcap [u,v ...]     (default: a grid over the lower half)

Reconstructs the view position from the raw depth (d = P22 + P32 / z, D3D9 pixel centres)
and takes it to the world with the inverse view matrix.
"""
import struct
import sys

import numpy as np


def load(path):
    data = open(path, "rb").read()
    magic = data[:4]
    w, h = struct.unpack_from("<II", data, 4)
    view = np.array(struct.unpack_from("<16f", data, 12)).reshape(4, 4)
    proj = np.array(struct.unpack_from("<16f", data, 76)).reshape(4, 4)
    off = 76 + 64 + 16 + 4 + (16 if magic == b"TMC2" else 0)
    depth = np.frombuffer(data, np.float32, w * h, off + w * h * 4).reshape(h, w)
    return w, h, view, proj, depth


def world(w, h, view, proj, depth, u, v):
    x, y = int(u * (w - 1)), int(v * (h - 1))
    d = float(depth[y, x])
    if d >= 0.99999:
        return None
    z = proj[3, 2] / (d - proj[2, 2])
    ndc_x = 2.0 * x / w - 1.0
    ndc_y = 1.0 - 2.0 * y / h
    vp = np.array([(ndc_x - proj[2, 0]) * z / proj[0, 0], (ndc_y - proj[2, 1]) * z / proj[1, 1], z, 1.0])
    return vp @ np.linalg.inv(view)


def main():
    w, h, view, proj, depth = load(sys.argv[1])
    points = [tuple(map(float, a.split(","))) for a in sys.argv[2:]]
    if not points:
        points = [(u / 8.0, v / 10.0) for v in range(5, 10) for u in range(1, 8)]
    for u, v in points:
        p = world(w, h, view, proj, depth, u, v)
        print("%.3f,%.3f  %s" % (u, v, "sky" if p is None else "x %8.2f  y %7.3f  z %8.2f" % (p[0], p[1], p[2])))


if __name__ == "__main__":
    main()
