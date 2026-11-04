#!/usr/bin/env python3
"""The six Lv 5 form icons, edited from the HUD's own skill icons (assets/ui/skills/*.rgba16.png, 32x32) so each still
reads as its skill: Full Moon Dance more X blades, Shattering Dance a cracked X, Twin Moon a double crescent, Full Moon a
ring, Crescent Storm trailing blades, Long Cleave a stretched beam. Used by tower_icons.py (assets/ui/skills/form_<name>.rgba16.png, 32x32)."""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from icon_png import read_png

REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'ui', 'skills')
N = 32
def load(n): return [[tuple(c / 255 for c in p) for p in r] for r in read_png('%s/skill_%s.rgba16.png' % (REF, n))[2]]
def blank(): return [[(0.0, 0.0, 0.0, 0.0)] * N for _ in range(N)]

def sample(img, x, y):      # bilinear, premultiplied; outside clear
    x -= 0.5; y -= 0.5
    x0, y0 = math.floor(x), math.floor(y); fx, fy = x - x0, y - y0
    acc = [0.0] * 4
    for dy, wy in ((0, 1 - fy), (1, fy)):
        for dx, wx in ((0, 1 - fx), (1, fx)):
            xx, yy = x0 + dx, y0 + dy
            if 0 <= xx < N and 0 <= yy < N:
                r, g, b, a = img[yy][xx]; w = wx * wy
                acc[0] += r * a * w; acc[1] += g * a * w; acc[2] += b * a * w; acc[3] += a * w
    a = acc[3]
    return (acc[0] / a, acc[1] / a, acc[2] / a, a) if a > 1e-4 else (0.0, 0.0, 0.0, 0.0)

def warp(img, inv):         # inv: output (x, y) -> source (x, y)
    return [[sample(img, *inv(x + 0.5, y + 0.5)) for x in range(N)] for y in range(N)]

def over(top, bot):
    out = blank()
    for y in range(N):
        for x in range(N):
            r, g, b, a = top[y][x]; R, G, B, A = bot[y][x]
            oa = a + A * (1 - a)
            out[y][x] = tuple((c * a + C * A * (1 - a)) / oa for c, C in zip((r, g, b), (R, G, B))) + (oa,) if oa > 0 else (0.0, 0.0, 0.0, 0.0)
    return out

def dim(img, k, ka=1.0): return [[(p[0] * k, p[1] * k, p[2] * k, p[3] * ka) for p in r] for r in img]
def gold(p): return p[0] > p[2] + 0.15     # the skills' gold feet (the blades are cyan-white)
def only(img, keep): return [[p if keep(x, y, p) else (0.0, 0.0, 0.0, 0.0) for x, p in enumerate(r)] for y, r in enumerate(img)]
def rot(img, deg, s=1.0, cx=16, cy=16):
    c, sn = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return warp(img, lambda x, y: (cx + (c * (x - cx) + sn * (y - cy)) / s, cy + (-sn * (x - cx) + c * (y - cy)) / s))

def full_moon_dance(mb):
    # four more blades: the X turned 45 degrees (a plus), a shade smaller and darker, behind the X
    return over(mb, dim(rot(mb, 45, 0.86), 0.78))

def shattering_dance(mb):
    # each arm broken two thirds out: a jagged gap across it, the outer piece knocked a pixel outward, two shards flying
    out = blank()
    for y in range(N):
        for x in range(N):
            dx, dy = x + 0.5 - 16, y + 0.5 - 16
            ux, uy = math.copysign(1, dx) / math.sqrt(2), math.copysign(1, dy) / math.sqrt(2)   # its arm's way out
            t = dx * ux + dy * uy; s = -dx * uy + dy * ux
            cut = 8.4 + (1.1 if int(math.floor(s / 1.7)) % 2 else -0.5)     # a zigzag break
            if abs(t - cut) < 2.0: continue
            if t > cut: out[y][x] = sample(mb, x + 0.5 - 3.4 * ux - 0.9 * uy * math.copysign(1, ux * uy), y + 0.5 - 3.4 * uy + 0.9 * ux * math.copysign(1, ux * uy))
            if t > cut:     # the broken-off piece as bright as the blade's middle (the arm's own fade made it read dim)
                r, g, b, a = out[y][x]
                out[y][x] = (min(1, r * 1.35 + 0.08), min(1, g * 1.3 + 0.08), min(1, b * 1.2 + 0.08), 1.0 if a > 0.3 else a)
            else: out[y][x] = mb[y][x]
    for sx, sy in ((27, 5), (4, 7), (26, 27), (4, 25)):     # shards, 2x2 in the blades' white
        for dy in (0, 1):
            for dx in (0, 1): out[sy + dy][sx + dx] = (0.85, 0.97, 1.0, 1.0)
    return out

def twin_moon(hm):
    # a second slash behind the first: the arc again, smaller, lower and darker (the feet stay one pair)
    arc = only(hm, lambda x, y, p: not gold(p))
    second = dim(warp(arc, lambda x, y: (16 + (x - 16) / 0.78, 6 + (y - 13.5) / 0.78)), 0.8)
    return over(hm, second)

def full_moon(hm):
    # the arc closed into a ring: its top half as drawn (pulled in a little), mirrored below; the feet kept
    arc = only(hm, lambda x, y, p: not gold(p) and y < 21)
    feet = only(hm, lambda x, y, p: gold(p))
    s, cy0, cy = 0.78, 19.5, 14.0      # the arc's ends' height in the source; the ring's middle
    top = warp(arc, lambda x, y: (16 + (x - 16) / s, cy0 + (y - cy) / s) if y < cy + 0.5 else (-9, -9))
    bot = warp(arc, lambda x, y: (16 + (x - 16) / s, cy0 - (y - cy) / s) if y >= cy - 0.5 else (-9, -9))
    return over(feet, over(top, dim(bot, 0.85)))

def crescent_storm(cl):
    # three more blades following behind: the row of three again, smaller, up and to the left, clear of the first three (and of the form badge, top right)
    beams = only(cl, lambda x, y, p: not gold(p))
    front = warp(cl, lambda x, y: (16 + (x - 20.5) / 0.72, 30 + (y - 31) / 0.72))
    trail = dim(warp(beams, lambda x, y: (16 + (x - 8) / 0.5, 30 + (y - 12.5) / 0.5)), 0.88)
    return over(front, trail)

def long_cleave(cl):
    # the beams stretched: the icon a fifth narrower and a third taller, its feet on the same line
    return warp(cl, lambda x, y: (16 + (x - 16) / 0.7, 30 + (y - 30) / 1.1))

FORMS = [  # (name, skill, form, maker)
    ("Full Moon Dance", "moonblade", "A", full_moon_dance), ("Shattering Dance", "moonblade", "B", shattering_dance),
    ("Twin Moon", "halfmoon", "A", twin_moon), ("Full Moon", "halfmoon", "B", full_moon),
    ("Crescent Storm", "cleave", "A", crescent_storm), ("Long Cleave", "cleave", "B", long_cleave),
]

def to8(img): return [[tuple(int(round(min(max(c, 0), 1) * 255)) for c in p) for p in r] for r in img]
def rgba16(img):    # as the ROM holds it: 5 bits a colour, 1 bit of alpha (alpha compare 1 in the HUD)
    return [[(p[0] >> 3 << 3 | p[0] >> 5, p[1] >> 3 << 3 | p[1] >> 5, p[2] >> 3 << 3 | p[2] >> 5, 255 if p[3] >= 128 else 0) for p in r] for r in img]

def make():
    """the six forms as 32x32 RGBA rows (0-255), in FORMS' order"""
    src = {n: load(n) for n in ('moonblade', 'halfmoon', 'cleave')}
    return [(name, sk, f, to8(fn(src[sk]))) for name, sk, f, fn in FORMS]
