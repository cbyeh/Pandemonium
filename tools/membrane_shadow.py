# Shadowborne's wing membrane, shadowy: wisps of smoke streaming out along the fingers (v runs from
# the wrist out), the veins faint under them. 32 x 64, I8, tiling.
#   python3 tools/membrane_shadow.py   ->  assets/dragon_boss/membrane_shadow.i8.png
import math, os, random, struct, zlib
W, H = 32, 64
random.seed(11)
waves = [(random.uniform(0, 6.28), random.uniform(0, 6.28), random.choice((2, 3, 4, 5)), 1, random.uniform(0.5, 1.0)) for _ in range(8)]
px = [[0.0] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        u, v = x / W, y / H
        smoke = 0.0
        for p1, p2, fu, fv, a in waves:     # tiling wisps, stretched along v (streaming out)
            smoke += a * math.sin(2 * math.pi * (fu * u + 0.35 * math.sin(2 * math.pi * fv * v + p2)) + p1)
        smoke = 0.5 + 0.13 * smoke
        vein = 0.0
        for cx in (4.0, 12.0, 20.0, 28.0):
            cxw = cx + 1.0 * math.sin(2 * math.pi * v * 2 + cx)
            d = min(abs(x - cxw), W - abs(x - cxw)); vein -= 0.10 * math.exp(-d * d / 1.4)
        px[y][x] = max(0.0, min(1.0, smoke + vein))
mean = sum(map(sum, px)) / (W * H)
raw = b''.join(b'\x00' + bytes(int(255 * min(1.0, max(0.0, 0.62 + (p - mean) * 0.85))) for p in row) for row in px)
def chunk(t, d): c = struct.pack('>I', len(d)) + t + d; return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets/dragon_boss/membrane_shadow.i8.png")
open(out, 'wb').write(png); print("wrote", out)
