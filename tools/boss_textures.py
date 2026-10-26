"""Paints the Bonus bosses' tiling textures (32x32 greyscale, I8), multiplied in-game by the
procedural meshes' vertex colours:

  assets/dragon_boss/scales.i8.png    Shadowborne's overlapping keeled scales (body, legs, head, horns)
  assets/dragon_boss/membrane.i8.png  its wing membranes: creased leather with veins
  assets/shackled_sun/surface.i8.png  the Shackled Sun's granulation: boiling cells, dark lanes

Every texture tiles both ways and is rolled so its brightest spot is at texels 1..3 x 1..3: unlit faces (crystals, eyes,
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


def scales():
    # Overlapping scales, rows 4 texels apart down the body (y runs from the chest toward the
    # tail), 8 across, each row shifted half a scale. A row lies over the root of the next, so
    # each scale shows the crescent of its free edge: in shadow under the scale before it, lit
    # toward its rounded edge, a dark rim, a faint keel down its middle
    X, Y = grid()
    W, H, R = 8.0, 4.0, 5.2
    out = np.full_like(X, 0.22)
    top = np.full_like(X, -1e9)
    for row in range(-3, int(N / H) + 3):
        for col in range(-2, int(N / W) + 2):
            cx = col * W + (W / 2 if row & 1 else 0.0)
            cy = row * H
            dx, dy = wrapped(X - cx), wrapped(Y - cy)
            r = np.hypot(dx, dy * 1.05) / R
            inside = (r < 1.0) & (dy > -R * 0.35)
            lit = np.clip((dy + R * 0.3) / (R * 1.0), 0, 1)
            v = 0.42 + 0.58 * lit ** 0.8                                # shadow at the root, lit toward the edge
            v *= 1.0 - 0.18 * (np.abs(dx) / R) ** 2                     # rounded across
            v += 0.04 * np.exp(-(dx / 0.8) ** 2) * (dy > 0)             # the keel
            v = np.where(r > 0.8, v * (1.0 - (r - 0.8) / 0.2 * 0.7), v)    # its rim
            v *= 0.9 + 0.14 * hash01(row % 8, col % 4)
            take = inside & (dy > top)      # the row before (the pixel furthest past its centre) on top
            out = np.where(take, v, out)
            top = np.where(take, dy, top)
    img = down(out) * noise(7, 0.04)
    return bright_corner(np.clip(img, 0, 1))


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


def surface():
    # Granulation: bright cells of rising plasma, dark lanes between
    X, Y, d1, d2, idx = cells(21, 36, 0.95)
    edge = d2 - d1
    # (light: it glows, and the ending's sun is seen close up, its white-gold core dimmed by darker lanes)
    v = 1.0 - 0.16 * (d1 / 4.0) ** 2 - 0.05 * np.vectorize(lambda k: hash01(k, 5))(idx)
    v = np.where(edge < 1.2, 0.72 + 0.2 * edge / 1.2, v)
    img = down(v) * noise(23, 0.02)
    return bright_corner(np.clip(img, 0, 1))


if __name__ == "__main__":
    write_png(os.path.join(ROOT, "dragon_boss", "scales.i8.png"), scales())
    write_png(os.path.join(ROOT, "dragon_boss", "membrane.i8.png"), membrane())
    write_png(os.path.join(ROOT, "shackled_sun", "surface.i8.png"), surface())
    sys.exit(0)
