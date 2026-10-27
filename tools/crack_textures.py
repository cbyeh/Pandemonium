"""Paints the Fallen Warrior's extra floor cracks (64x64 IA8, dark lines on clear), drawn in
place of ground_crushed.ia8.png on its slams so they don't all leave the same mark:

  assets/boss/ground_crushed_b.ia8.png  a split along the blade's line, with side branches
  assets/boss/ground_crushed_c.ia8.png  a shattered star: short spokes and a broken ring
  assets/boss/ground_crushed_d.ia8.png  a fan thrown forward from the bite

Needs numpy (Blender's bundled Python has it):
  /Applications/Blender.app/Contents/Resources/*/python/bin/python3.* tools/crack_textures.py
"""
import math
import os
import struct
import zlib

import numpy as np

N = 64
SS = 4      # supersampled, then averaged (smooth edges)
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "boss")


def write_png(path, rgba):
    a = np.clip(np.rint(rgba * 255.0), 0, 255).astype(np.uint8)
    raw = b"".join(b"\0" + a[y].tobytes() for y in range(a.shape[0]))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", a.shape[1], a.shape[0], 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print(f"{path}: {(a[..., 3] > 0).sum()} texels drawn")


class Canvas:
    def __init__(self):
        self.a = np.zeros((N * SS, N * SS))

    def line(self, x0, y0, x1, y1, w):
        """A stroke from (x0, y0) to (x1, y1) in texels, `w` texels wide"""
        n = int(max(abs(x1 - x0), abs(y1 - y0)) * SS * 2) + 1
        r = max(w * SS * 0.5, 0.6)
        for i in range(n + 1):
            t = i / n
            cx, cy = (x0 + (x1 - x0) * t) * SS, (y0 + (y1 - y0) * t) * SS
            ys, xs = slice(max(int(cy - r - 1), 0), int(cy + r + 2)), slice(max(int(cx - r - 1), 0), int(cx + r + 2))
            yy, xx = np.mgrid[ys, xs]
            self.a[ys, xs] = np.maximum(self.a[ys, xs], ((xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2 <= r * r) * 1.0)

    def crack(self, rng, x, y, ang, length, w, depth):
        """A jagged crack that tapers and forks: each step kinks off its heading, which holds"""
        seg, head = 2.0, ang
        while length > 0 and w > 0.3:
            a = head + rng.uniform(-0.7, 0.7)
            nx, ny = x + math.cos(a) * seg, y + math.sin(a) * seg
            self.line(x, y, nx, ny, w)
            for _ in range(2):      # grit: chips knocked out beside the line
                if rng.random() < 0.45:
                    o = rng.uniform(1.0, 2.5) * rng.choice((-1, 1))
                    px, py = nx - math.sin(a) * o, ny + math.cos(a) * o
                    self.line(px, py, px, py, rng.uniform(0.5, 1.0))
            x, y, length, w = nx, ny, length - seg, w * 0.95
            head += rng.uniform(-0.08, 0.08)
            if depth > 0 and rng.random() < 0.18:
                self.crack(rng, x, y, head + rng.choice((-1, 1)) * rng.uniform(0.6, 1.2), length * 0.5, w * 0.75, depth - 1)

    def save(self, path, rng):
        a = self.a.reshape(N, SS, N, SS).mean(axis=(1, 3))
        a = np.where(a > 0.08, np.clip(a * 1.15, 0.0, 1.0), 0.0)
        grey = (10 + rng.integers(0, 40, a.shape)) / 255.0 * (a > 0)     # 10..50, as the original
        write_png(path, np.dstack([grey, grey, grey, a]))


def split():
    rng = np.random.default_rng(7)
    c = Canvas()
    for ang in (-math.pi / 2, math.pi / 2):     # both ways from the bite, along the blade
        c.crack(rng, 32, 32, ang + rng.uniform(-0.1, 0.1), 29, 2.0, 2)
    for _ in range(4):                          # short side cracks off the middle
        y = 32 + rng.uniform(-10, 10)
        c.crack(rng, 32, y, rng.choice((0.0, math.pi)) + rng.uniform(-0.5, 0.5), 10, 1.2, 1)
    c.save(os.path.join(ROOT, "ground_crushed_b.ia8.png"), rng)


def shatter():
    rng = np.random.default_rng(11)
    c = Canvas()
    for i in range(7):                          # spokes from the bite
        a = i * 2 * math.pi / 7 + rng.uniform(-0.2, 0.2)
        c.crack(rng, 32, 32, a, rng.uniform(16, 28), 1.8, 1)
    pts = []                                    # a broken ring where the floor gave
    for i in range(14):
        a = i * 2 * math.pi / 14
        r = 16 + rng.uniform(-2.5, 2.5)
        pts.append((32 + math.cos(a) * r, 32 + math.sin(a) * r))
    for i in range(14):
        if rng.random() < 0.75:
            (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % 14]
            c.line(x0, y0, x1, y1, 1.0)
    c.save(os.path.join(ROOT, "ground_crushed_c.ia8.png"), rng)


def fan():
    rng = np.random.default_rng(23)
    c = Canvas()
    for i in range(5):                          # spread ahead of the bite (up the texture)
        a = -math.pi / 2 + (i - 2) * 0.42 + rng.uniform(-0.1, 0.1)
        c.crack(rng, 32, 46, a, rng.uniform(22, 36), 2.0, 2)
    c.crack(rng, 32, 46, math.pi / 2, 10, 1.6, 0)   # a short tail behind
    c.save(os.path.join(ROOT, "ground_crushed_d.ia8.png"), rng)


if __name__ == "__main__":
    split()
    shatter()
    fan()
