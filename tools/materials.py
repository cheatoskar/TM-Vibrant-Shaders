"""The game's materials per pixel, from F12's capture_NNN.mat (material masks on).

    python tools/materials.py capture_045.mat                 which material drew how much of the
                                                              image, with the game's brightness there
    python tools/materials.py capture_045.mat --map ids.png   false-colour picture of the ids
                                                              (lamp materials yellow, the car red)
    python tools/materials.py --synth capture_044.tmcap --light x0,y0,x1,y1 [--car x0,y0,x1,y1]
        writes capture_044.mat for previewer tests: in the boxes, what the game drew bright
        (>= 0.9) becomes a lamp / the car's body. Not the game's materials: a plumbing test.

capture_NNN.mat: "TMM1", width, height, count (uint32 each), then per id 1..count: class
(uint8, engine::Material: 0 other, 1 light, 2 car, 3 glass, 4 grass, 5 water, 6 neon, 7 dirt), flags (uint8:
1 depth writes, 2 one-one blending, 4 its textures were read), draws (uint32), name length (uint16) and
UTF-8 name (the material's shader file, then " | " and its textures' files when they were read); then one id byte per pixel, rows top-down (0: nothing drawn with depth).
The brightness columns read capture_NNN.tmcap (the game's 8-bit image) and capture_NNN.hdr16
(GameHdr: its float image) next to it when they exist.
"""
import os
import struct
import sys

import numpy as np

CLASSES = ["other", "LIGHT", "car", "glass", "grass", "water", "NEON", "dirt"]


def read_mat(path):
    with open(path, "rb") as f:
        magic, w, h, count = struct.unpack("<4I", f.read(16))
        if magic != 0x314D4D54:
            raise SystemExit(f"{path}: not a material capture")
        materials = [("(nothing)", 0, 0, 0)]
        for _ in range(count):
            cls, flags, draws, length = struct.unpack("<BBIH", f.read(8))
            materials.append((f.read(length).decode("utf-8", "replace"), cls, flags, draws))
        ids = np.frombuffer(f.read(w * h), dtype=np.uint8).reshape(h, w)
    return w, h, materials, ids


def read_tmcap_color(path, w, h):
    with open(path, "rb") as f:
        magic = f.read(4)
        f.seek(176 if magic == b"TMC2" else 160)
        bgra = np.frombuffer(f.read(w * h * 4), dtype=np.uint8).reshape(h, w, 4)
    return bgra[..., 2::-1].astype(np.float32) / 255.0  # rgb


def read_hdr(path, w, h):
    with open(path, "rb") as f:
        magic, hw, hh = struct.unpack("<3I", f.read(12))
        if magic != 0x31484D54 or hw != w or hh != h:
            return None
        return np.frombuffer(f.read(), dtype=np.float16).astype(np.float32).reshape(h, w, 4)[..., :3]


def census(path, map_out=None):
    w, h, materials, ids = read_mat(path)
    base = path[:-4]
    game = read_tmcap_color(base + ".tmcap", w, h) if os.path.exists(base + ".tmcap") else None
    hdr = read_hdr(base + ".hdr16", w, h) if os.path.exists(base + ".hdr16") else None
    gpeak = game.max(axis=2) if game is not None else None
    hpeak = hdr.max(axis=2) if hdr is not None else None
    counts = np.bincount(ids.ravel(), minlength=256)
    print(f"{w}x{h}, {len(materials) - 1} materials")
    head = f"{'id':>3} {'class':6} {'flags':5} {'draws':>5} {'pixels':>8} {'%':>6}"
    if gpeak is not None:
        head += f" {'game>=250':>9}"
    if hpeak is not None:
        head += f" {'hdr p50':>7} {'p95':>5} {'max':>5} {'>1.0':>6}"
    print(head + "  file")
    order = sorted(range(len(materials)), key=lambda i: -counts[i])
    for i in order:
        name, cls, flags, draws = materials[i]
        n = counts[i]
        if n == 0 and i == 0:
            continue
        flag = ("z" if flags & 1 else "-") + ("+" if flags & 2 else "-") + ("t" if flags & 4 else "-")
        line = f"{i:>3} {CLASSES[cls] if cls < len(CLASSES) else cls:6} {flag:5} {draws:>5} {n:>8} {n * 100.0 / (w * h):6.2f}"
        sel = ids == i
        if gpeak is not None:
            line += f" {((gpeak[sel] >= 250 / 255).mean() * 100 if n else 0):8.1f}%"
        if hpeak is not None:
            if n:
                v = hpeak[sel]
                line += f" {np.percentile(v, 50):7.2f} {np.percentile(v, 95):5.2f} {v.max():5.2f} {(v > 1.0).mean() * 100:5.1f}%"
            else:
                line += f" {'':7} {'':5} {'':5} {'':6}"
        print(line + "  " + name)
    if map_out:
        from PIL import Image
        rng = np.random.default_rng(7)
        palette = (rng.random((256, 3)) * 0.6 + 0.2) * 255
        palette[0] = (0, 0, 0)
        for i, (_, cls, _, _) in enumerate(materials):
            if cls == 1:
                palette[i] = (255, 230, 0)
            elif cls == 2:
                palette[i] = (230, 30, 30)
        img = palette[ids].astype(np.uint8)
        if game is not None:  # the image under the ids, so the places can be found
            img = (img * 0.65 + (game ** (1 / 2.2)) * 255 * 0.35).astype(np.uint8)
        Image.fromarray(img).save(map_out)
        print("map:", map_out)


def synth(tmcap, boxes):
    with open(tmcap, "rb") as f:
        magic = f.read(4)
        w, h = struct.unpack("<2I", f.read(8))
    game = read_tmcap_color(tmcap, w, h)
    peak = game.max(axis=2)
    ids = np.zeros((h, w), np.uint8)
    names = []
    for cls, (x0, y0, x1, y1) in boxes:
        names.append((cls, f"synthetic {CLASSES[cls]} box {x0},{y0},{x1},{y1}"))
        region = ids[y0:y1, x0:x1]
        region[peak[y0:y1, x0:x1] >= 0.9] = len(names)
    out = tmcap[:-6] + ".mat"
    with open(out, "wb") as f:
        f.write(struct.pack("<4I", 0x314D4D54, w, h, len(names)))
        for cls, name in names:
            data = name.encode("utf-8")
            f.write(struct.pack("<BBIH", cls, 1, 1, len(data)) + data)
        f.write(ids.tobytes())
    print(f"{out}: {[(n, int((ids == i + 1).sum())) for i, (_, n) in enumerate(names)]}")


def main(argv):
    if "--synth" in argv:
        tmcap = argv[argv.index("--synth") + 1]
        boxes = []
        for flag, cls in (("--light", 1), ("--car", 2)):
            if flag in argv:
                boxes.append((cls, tuple(int(v) for v in argv[argv.index(flag) + 1].split(","))))
        synth(tmcap, boxes)
        return
    if not argv:
        raise SystemExit(__doc__)
    census(argv[0], argv[argv.index("--map") + 1] if "--map" in argv else None)


if __name__ == "__main__":
    main(sys.argv[1:])
