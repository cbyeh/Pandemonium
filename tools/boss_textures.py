"""Paints the Bonus bosses' tiling textures (greyscale, I8), multiplied in-game by the procedural
meshes' vertex colours:

  assets/dragon_boss/scales.i8.png    Shadowborne's scale map, 128x32, once round each tube (scales())
  assets/dragon_boss/membrane.i8.png  its wing membranes: creased leather with veins
  assets/shackled_sun/veins.rgba16.png  the Shackled Sun's veins: thin, branching, red on white (colour), 64x32

The others are 32x32. Every texture tiles both ways and is rolled so its brightest spot is at texels 1..3 x 1..3: unlit faces (crystals, eyes,
the sun's iris) all sample texel (2, 2), so they keep their colour (proc_mesh_uv).

Needs numpy (Blender's bundled Python has it):
  /Applications/Blender.app/Contents/Resources/*/python/bin/python3.* tools/boss_textures.py
"""
import os
import struct
import sys
import zlib

import numpy as np

N = 32      # texels
SS = 4      # supersampled, then averaged (smooth edges)
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")


def write_png(path, img):
    a = np.clip(np.rint(img * 255.0), 0, 255).astype(np.uint8)
    raw = b"".join(b"\0" + a[y].tobytes() for y in range(a.shape[0]))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", a.shape[1], a.shape[0], 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)
    print(f"{path}: mean {a.mean() / 255:.2f}, min {a.min()}, max {a.max()}")


def write_png_rgb(path, img):
    """img: (h, w, 3) in 0..1."""
    a = np.clip(np.rint(img * 255.0), 0, 255).astype(np.uint8)
    raw = b"".join(b"\0" + a[y].tobytes() for y in range(a.shape[0]))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", a.shape[1], a.shape[0], 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)
    print(f"{path}: mean {a.mean() / 255:.2f}")


def grid():
    """Supersampled texel coordinates (x across, y down), 0..N."""
    c = (np.arange(N * SS) + 0.5) / SS
    return np.meshgrid(c, c)


def down(img):
    return img.reshape(N, SS, N, SS).mean(axis=(1, 3))


def hash01(*k):
    h = 2166136261
    for v in k:
        h = ((h ^ (int(v) & 0xFFFFFFFF)) * 16777619) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def noise(seed, amp):
    """Per-texel grain (tiles: it is per texel)."""
    rng = np.random.default_rng(seed)
    return 1.0 + amp * (rng.random((N, N)) * 2.0 - 1.0)


def bright_corner(img):
    """Rolls the tile (it still tiles) so its brightest 3x3 lands on texels 1..3: unlit faces
    sample texel (2, 2), so they keep (nearly) their colour. Not lifted to white: a spot per tile showed"""
    best, at = -1.0, (0, 0)
    for y in range(N):
        for x in range(N):
            w = np.roll(np.roll(img, -y, 0), -x, 1)[:3, :3].min()
            if w > best:
                best, at = w, (y, x)
    img = np.roll(np.roll(img, 1 - at[0], 0), 1 - at[1], 1)
    print(f"  texel (2, 2) area: {best:.2f}")
    return np.clip(img, 0, 1)


def wrapped(d):
    return (d + N / 2) % N - N / 2


MU, MV = 128, 32    # the scale map: texels round a tube, and down it before it repeats (4 KB: TMEM)


def scales():
    # Shadowborne's scale map, 128x32, wrapping once round a tube (u: from its right side going
    # up first, so the spine is at 32 and the belly at 96; v: down the body toward the tail):
    # rows of flank scales, a row of larger keeled scales down the spine, broad plates under the
    # belly. Each scale painted with its own size, lean, shade and outline; a few old and dark,
    # a few chipped, a few raked by scars. Under its neighbour toward the head, each shows the
    # crescent of its free edge: in shadow at the root, lit toward a crisp dark rim
    rng = np.random.default_rng(1983)
    X, Y = np.meshgrid((np.arange(MU * SS) + 0.5) / SS, (np.arange(MV * SS) + 0.5) / SS)
    wrapu = lambda d: (d + MU / 2) % MU - MU / 2
    wrapv = lambda d: (d + MV / 2) % MV - MV / 2
    out = np.full_like(X, 0.28)
    best = np.full_like(X, -1e9)        # the layer and how far past its centre: who's on top
    els = []
    COLS, ROWS = 13, 8
    for r in range(ROWS):           # flanks
        for k in range(COLS):
            u = (k + (0.5 if r & 1 else 0.0)) * MU / COLS + rng.uniform(-0.8, 0.8)
            if 80.0 < u < 112.0:
                continue
            els.append(dict(u=u, v=r * MV / ROWS + rng.uniform(-0.3, 0.3), rx=6.6 * rng.uniform(0.86, 1.12), ry=3.4 * rng.uniform(0.9, 1.1),
                            lean=rng.uniform(-0.12, 0.12), layer=1, keel=0.03))
    for r in range(4):              # the spine
        els.append(dict(u=32 + rng.uniform(-1.0, 1.0), v=r * 8 + rng.uniform(-0.3, 0.3), rx=10.0 * rng.uniform(0.94, 1.06), ry=6.4 * rng.uniform(0.95, 1.05),
                        lean=rng.uniform(-0.04, 0.04), layer=2, keel=0.12))
    for r in range(4):              # the belly
        els.append(dict(u=96 + rng.uniform(-0.8, 0.8), v=r * 8 + rng.uniform(-0.2, 0.2), rx=20.0 * rng.uniform(0.98, 1.02), ry=6.6 * rng.uniform(0.96, 1.04),
                        lean=rng.uniform(-0.015, 0.015), layer=0, keel=0.0, plate=True))
    for e in els:
        worn, chip = rng.random() < 0.09, rng.random() < 0.07
        shade = rng.uniform(0.8, 1.0) * (0.72 if worn else 1.0)
        ca, sa = np.cos(e["lean"]), np.sin(e["lean"])
        dx0, dy0 = wrapu(X - e["u"]), wrapv(Y - e["v"])
        dx, dy = dx0 * ca - dy0 * sa, dx0 * sa + dy0 * ca
        plate = e.get("plate", False)
        p = 4.0 if plate else 2.0         # plates: squarer sides
        r = ((np.abs(dx) / e["rx"]) ** p + (np.abs(dy) / e["ry"]) ** p) ** (1.0 / p)
        inside = (r < 1.0) & (dy > -e["ry"] * 0.35)
        if chip:            # a notch out of its free edge: the scale under it shows
            a = rng.uniform(-0.9, 0.9)
            inside &= np.hypot((dx - np.sin(a) * e["rx"] * 0.85) * 0.5, dy - np.cos(a) * e["ry"] * 0.85) > 1.2
        lit = np.clip((dy + e["ry"] * 0.3) / (e["ry"] * 1.0), 0, 1)
        v = 0.58 + 0.42 * lit ** 0.8
        v *= 1.0 - 0.2 * np.clip(np.abs(dx) / e["rx"], 0, 1) ** 2
        v += e["keel"] * np.exp(-(dx / 1.3) ** 2) * (dy > -1.0)
        rim = 0.78 if not worn else 0.7
        v = np.where(r > rim, v * (1.0 - (r - rim) / (1.0 - rim) * 0.58), v)
        if plate:           # fine growth lines across a plate
            v *= 1.0 - 0.06 * (np.sin(dy * 2.6) > 0.6)
        v *= shade
        key = e["layer"] * 1000.0 + dy
        take = inside & (key > best)
        out = np.where(take, v, out)
        best = np.where(take, key, best)
    for k in range(5):              # scars raked across the flanks and the spine
        u0, v0 = rng.uniform(0, MU), rng.uniform(0, MV)
        while 76 < u0 < 116:
            u0 = rng.uniform(0, MU)
        a, L = rng.uniform(0.15, 0.6) * (1 if k & 1 else -1), rng.uniform(10, 18)
        dx, dy = wrapu(X - u0), wrapv(Y - v0)
        t = np.clip(dx * np.cos(a) + dy * np.sin(a), -L / 2, L / 2)
        d = np.hypot(dx - t * np.cos(a), dy - t * np.sin(a))
        out = np.where(d < 0.45, out * 0.5, np.where(d < 0.9, out * (1.0 + 0.12 * (d - 0.45) / 0.45), out))   # a gouge, its lip catching light
    img = out.reshape(MV, SS, MU, SS).mean(axis=(1, 3))
    img *= 1.0 + 0.04 * (np.random.default_rng(7).random((MV, MU)) * 2 - 1)
    img = np.clip(img, 0, 1)
    # rolled down the tile (along the body: free) so a bright spot in columns 1..3 lands on texels
    # (1..3, 1..3), lifted to white: unlit faces (crystals, eyes, teeth) sample texel (2, 2)
    bestw, at = -1.0, 0
    for y in range(MV):
        w = np.roll(img, -y, 0)[:3, 1:4].min()
        if w > bestw:
            bestw, at = w, y
    img = np.roll(img, 1 - at, 0)
    img[1:4, 1:4] = np.maximum(img[1:4, 1:4], 0.92)
    print(f"  texel (2, 2) area: {bestw:.2f}, lifted to 0.92")
    return img


def cells(seed, n, jitter=0.8):
    """Tiling Voronoi: the nearest and second-nearest distances and the nearest cell's id."""
    rng = np.random.default_rng(seed)
    g = int(np.sqrt(n))
    pts = []
    for j in range(g):
        for i in range(g):
            pts.append(((i + 0.5 + jitter * (rng.random() - 0.5)) * N / g, (j + 0.5 + jitter * (rng.random() - 0.5)) * N / g))
    X, Y = grid()
    d1 = np.full_like(X, 1e9)
    d2 = np.full_like(X, 1e9)
    idx = np.zeros(X.shape, dtype=int)
    for k, (px, py) in enumerate(pts):
        d = np.hypot(wrapped(X - px), wrapped(Y - py))
        closer = d < d1
        d2 = np.where(closer, d1, np.minimum(d2, d))
        idx = np.where(closer, k, idx)
        d1 = np.where(closer, d, d1)
    return X, Y, d1, d2, idx


def membrane():
    # Leather: puckered cells of skin between deep creases, two veins winding through
    X, Y, d1, d2, idx = cells(11, 16, 0.9)
    edge = d2 - d1
    v = 0.95 + 0.05 * np.vectorize(lambda k: hash01(k, 3))(idx) - 0.3 * (d1 / 6.5) ** 2
    v = np.where(edge < 1.2, 0.38 + 0.45 * edge / 1.2, v)        # creases
    for k, (ph, amp, y0) in enumerate(((0.0, 3.0, 8.0), (1.7, 3.5, 24.0))):
        yv = y0 + amp * np.sin(2 * np.pi * X / N * (1 + k) + ph)
        dv = np.abs(wrapped(Y - yv))
        v = np.where(dv < 1.3, np.minimum(v, 0.4 + 0.3 * dv), v)
        v += 0.1 * np.exp(-((dv - 1.8) / 0.6) ** 2)            # the vein's raised edge catches light
    img = down(v) * noise(13, 0.04)
    return bright_corner(np.clip(img, 0, 1))


VW, VH = 64, 32     # the veins: round the ball twice, down it twice (RGBA16, 4 KB: TMEM)


def veins():
    # Thin red veins wandering over white (multiplied by the sun's gradient: dark red on gold):
    # a few trunks, each a random walk that turns a little at every step and forks now and then,
    # its branches thinner and fainter; wrapped both ways, so it tiles
    rng = np.random.default_rng(31)
    xs = (np.arange(VW * SS) + 0.5) / SS
    ys = (np.arange(VH * SS) + 0.5) / SS
    X, Y = np.meshgrid(xs, ys)
    ink = np.zeros_like(X)                  # 0..1: how much vein covers the texel
    stack = [(rng.random() * VW, rng.random() * VH, rng.random() * 2 * np.pi, 1.0, 46) for _ in range(7)]
    while stack:
        x, y, a, w, n = stack.pop()
        for _ in range(n):
            a += rng.normal(0.0, 0.45)
            x, y = (x + np.cos(a) * 0.7) % VW, (y + np.sin(a) * 0.7) % VH
            dx = (X - x + VW / 2) % VW - VW / 2
            dy = (Y - y + VH / 2) % VH - VH / 2
            r = 0.32 + 0.33 * w                     # half-width (texels): trunks under a texel and a half wide
            ink = np.maximum(ink, w * np.clip(1.0 - np.hypot(dx, dy) / r, 0.0, 1.0) ** 0.7)
            if w > 0.35 and rng.random() < 0.06:    # a fork, thinner, off to one side
                stack.append((x, y, a + rng.choice((-1, 1)) * rng.uniform(0.6, 1.2), w * 0.65, int(n * 0.6)))
            w *= 0.993
    ink = ink.reshape(VH, SS, VW, SS).mean(axis=(1, 3))
    ink[0:5, 0:5] = 0.0      # unlit faces (the ending's iris) sample texel (2, 2): left clear
    vein = np.array([0.5, 0.08, 0.04])      # times the gradient: (255, 232, 165) -> dark red
    base = np.ones(3)
    img = base[None, None, :] * (1.0 - ink[..., None]) + vein[None, None, :] * ink[..., None]
    print(f"  veins cover {np.mean(ink > 0.3) * 100:.0f}% of the texels")
    return img


if __name__ == "__main__":
    write_png(os.path.join(ROOT, "dragon_boss", "scales.i8.png"), scales())
    write_png(os.path.join(ROOT, "dragon_boss", "membrane.i8.png"), membrane())
    write_png_rgb(os.path.join(ROOT, "shackled_sun", "veins.rgba16.png"), veins())
    sys.exit(0)
