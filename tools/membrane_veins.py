# Shadowborne's wing membrane: veins running out along the fingers (vertical in the map: the
# membrane's UVs run from the wrist out), fine wrinkles across them. 32 x 64, I8.
#   python3 tools/membrane_veins.py   ->  assets/dragon_boss/membrane_veins.i8.png
import math, os, random, struct, zlib
W, H = 32, 64
random.seed(7)
px = [[0.0] * W for _ in range(H)]
veins = [(2.0, 1.6), (9.5, 1.0), (16.0, 1.9), (24.0, 1.1), (29.5, 0.8)]       # (u, strength); u wraps
for y in range(H):
    for x in range(W):
        v = 0.70
        for cx, k in veins:
            cxw = cx + 1.2 * math.sin(2 * math.pi * (y / H) * 2 + cx)     # a vein wanders a little
            d = min(abs(x - cxw), W - abs(x - cxw))
            v -= 0.28 * k * math.exp(-d * d / 1.6)          # the vein's dark groove
            v += 0.10 * k * math.exp(-(d - 1.6) ** 2 / 0.8)  # and its lit ridge beside it
        v += 0.05 * math.sin(2 * math.pi * y / 5.3 + x * 0.4) + 0.04 * (random.random() - 0.5)     # wrinkles across
        px[y][x] = max(0.0, min(1.0, v))
mean = sum(map(sum, px)) / (W * H)
raw = b''.join(b'\x00' + bytes(int(255 * min(1.0, p * 0.72 / mean)) for p in row) for row in px)     # mean ~0.72 like the others
def chunk(t, d): c = struct.pack('>I', len(d)) + t + d; return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets/dragon_boss/membrane_veins.i8.png")
open(out, 'wb').write(png); print("wrote", out, "mean", round(mean, 2))
