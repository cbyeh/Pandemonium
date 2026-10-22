#!/usr/bin/env python3
"""Renders metadata/cover.jpg: a raymarched SDF scene (the knight, small and seen from
behind, facing the giant Guardian before the Shackled Sun) inside the N64-box frame.

    pip install numpy pillow && python3 tools/cover_render.py [--ss 2]

The right-hand info band is lifted from the first cover in git (dfe2a2b)."""
import argparse, io, os, subprocess
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H, ART_W = 1000, 663, 835
f32 = np.float32

def V(*a): return np.array(a, f32)
def length(v): return np.sqrt(np.einsum('...i,...i->...', v, v))
def dot(a, b): return np.einsum('...i,...i->...', a, b)
def norm(v): return v / length(v)[..., None]
def clip01(x): return np.clip(x, 0, 1)

def smin(a, b, k):
    h = clip01(0.5 + 0.5 * (b - a) / k)
    return b * (1 - h) + a * h - k * h * (1 - h)

def sphere(p, c, r): return length(p - c) - r

def ellipsoid(p, c, r):
    q = (p - c) / r
    k0 = length(q); k1 = length(q / r)
    return k0 * (k0 - 1) / np.maximum(k1, 1e-6)

def capsule(p, a, b, ra, rb=None):
    pa, ba = p - a, b - a
    h = clip01(dot(pa, ba) / dot(ba, ba))
    r = ra if rb is None else ra + (rb - ra) * h
    return length(pa - ba * h[:, None]) - r

def box(q, b, r=0.0):
    q = np.abs(q) - (b - r)
    return length(np.maximum(q, 0)) + np.minimum(q.max(1), 0) - r

def basis(y, hint=V(0, 0, 1)):
    """Columns x,y,z with y along `y`; (p - c) @ R gives local coordinates."""
    y = norm(y); x = norm(np.cross(y, hint)); z = np.cross(x, y)
    return np.stack([x, y, z], 1)

def blade(p, a, b, hw, ht, hint=V(0, 0, 1)):
    """Flat double-edged blade from a (hilt) to b (tip), tapering at the tip."""
    L = length(b - a); q = (p - (a + b) / 2) @ basis(b - a, hint)
    w = hw * clip01((L / 2 - q[:, 1]) / (L * 0.18))
    dx = np.abs(q[:, 0]) - w
    dy = np.abs(q[:, 1]) - L / 2
    dz = np.abs(q[:, 2]) - ht * (0.4 + 0.6 * clip01(1 - np.abs(q[:, 0]) / np.maximum(w, 1e-4)))
    m = np.stack([dx, dy, dz], 1)
    return (length(np.maximum(m, 0)) + np.minimum(m.max(1), 0)) * 0.7

def chain(p, o, u, n_links, s, R, r):
    """Chain of alternating elongated-torus links starting at o along unit u."""
    v = norm(np.cross(u, V(0, 0, 1)) + V(0, 1e-3, 0)); w = np.cross(u, v)
    q = p - o; t = dot(q, u); a = dot(q, v); b = dot(q, w)
    d = np.full(len(p), 1e9, f32)
    for phase, (c1, c2) in ((0, (a, b)), (1, (b, a))):
        k = np.clip(np.round((t / s - phase) / 2), 0, n_links // 2)
        lt = t - (2 * k + phase) * s
        e = np.maximum(np.abs(lt) - s * 0.35, 0)
        d = np.minimum(d, length(np.stack([np.sqrt(e * e + c1 * c1) - R, c2], 1)) - r)
    return d

# ---------------------------------------------------------------- scene
HERO = V(-0.9, 0, 0)
BOSS = V(1.4, 0, 19); BOSS_S = 1.0  # boss is modelled ~14 units tall, then scaled
SWORD_BASE = BOSS + V(0, 0, -3.4) * BOSS_S
CAM = V(-1.55, 1.15, -6.2)
_d = BOSS + V(0, 12.0, 0) * BOSS_S - CAM
SUN = CAM + _d / np.linalg.norm(_d) * 85; SUN_R = 16.0  # right behind the boss's head
ST, CLOTH, PLUME, DARK, GOLD, RED, BLADE, GLOW, IRON, LEATHER, ROCK, SUNM, EYE, GROUND = range(1, 15)

def grooves(d, coord, period, depth, width=0.12):
    """Engraved lines across a surface every `period` along `coord` (plate lames, bands)."""
    f = coord / period - np.floor(coord / period)
    return d + depth * clip01(1 - np.minimum(f, 1 - f) / width)

def hero(p):
    q = p - HERO
    x, y, z = q[:, 0], q[:, 1], q[:, 2]
    parts = []
    # legs: tapered thigh, knee cop, greave, pointed sabaton
    legs = 1e9
    for hip, knee, ank, toe in ((V(-0.1, 0.92, 0), V(-0.14, 0.52, -0.03), V(-0.15, 0.12, -0.07), V(-0.16, 0.04, 0.08)),
                                (V(0.1, 0.92, 0), V(0.15, 0.52, 0.09), V(0.17, 0.12, 0.15), V(0.18, 0.04, 0.3))):
        leg = capsule(q, hip, knee, 0.088, 0.068)
        leg = smin(leg, capsule(q, knee, ank, 0.066, 0.048), 0.03)
        leg = smin(leg, ellipsoid(q, knee + V(0, 0.01, 0.045), V(0.062, 0.055, 0.045)), 0.015)
        foot = smin(ellipsoid(q, ank + V(0, -0.06, 0.02), V(0.06, 0.05, 0.09)), capsule(q, ank + V(0, -0.07, 0.04), toe, 0.045, 0.02), 0.03)
        legs = np.minimum(legs, smin(leg, foot, 0.03))
    legs = grooves(legs, y, 0.07, 0.004)
    fauld = grooves(capsule(q, V(0, 1.03, 0), V(0, 0.84, 0), 0.165, 0.2), y, 0.065, 0.006)
    chest = smin(ellipsoid(q, V(0, 1.31, 0.0), V(0.2, 0.2, 0.135)), ellipsoid(q, V(0, 1.08, 0), V(0.16, 0.14, 0.11)), 0.06)
    body = smin(smin(chest, fauld, 0.04), legs, 0.04)
    # layered pauldrons
    for sx in (-1, 1):
        body = np.minimum(body, ellipsoid(q, V(0.25 * sx, 1.48, 0), V(0.12, 0.085, 0.12)))
        body = np.minimum(body, ellipsoid(q, V(0.28 * sx, 1.42, 0), V(0.115, 0.07, 0.12)))
    # arms: left hangs, right holds the sword low and out
    arms = smin(capsule(q, V(-0.29, 1.42, 0), V(-0.33, 1.12, 0.03), 0.058, 0.05),
                capsule(q, V(-0.33, 1.12, 0.03), V(-0.31, 0.9, 0.09), 0.048, 0.042), 0.02)
    arms = np.minimum(arms, smin(capsule(q, V(0.29, 1.42, 0), V(0.38, 1.13, 0.03), 0.058, 0.05),
                                 capsule(q, V(0.38, 1.13, 0.03), V(0.46, 0.94, 0.1), 0.048, 0.042), 0.02))
    for e in (V(-0.335, 1.12, 0.0), V(0.385, 1.13, 0.0)):   # elbow cops
        arms = np.minimum(arms, ellipsoid(q, e, V(0.05, 0.05, 0.045)))
    arms = np.minimum(arms, ellipsoid(q, V(0.47, 0.9, 0.11), V(0.05, 0.055, 0.05)))
    arms = np.minimum(arms, ellipsoid(q, V(-0.31, 0.87, 0.1), V(0.045, 0.055, 0.045)))
    helm = smin(ellipsoid(q, V(0, 1.71, 0.0), V(0.12, 0.14, 0.13)), ellipsoid(q, V(0, 1.66, 0.06), V(0.105, 0.11, 0.1)), 0.03)
    helm = smin(helm, capsule(q, V(0, 1.84, 0.08), V(0, 1.82, -0.1), 0.018), 0.02)   # crest
    helm = smin(helm, capsule(q, V(0, 1.5, 0), V(0, 1.6, 0), 0.07), 0.04)
    parts.append((np.minimum(np.minimum(body, arms), helm), ST))
    # horsehair plume streaming back
    plume = capsule(q, V(0, 1.84, 0.03), V(0, 1.91, -0.08), 0.04, 0.045)
    plume = smin(plume, capsule(q, V(0, 1.91, -0.08), V(0.01, 1.84, -0.24), 0.045, 0.03), 0.03)
    plume = smin(plume, capsule(q, V(0.01, 1.84, -0.24), V(0.03, 1.62, -0.34), 0.03, 0.01), 0.03)
    parts.append((grooves(plume, x * 3 + y, 0.02, 0.004), PLUME))
    # cloak: pleated folds, flaring and torn toward the knees
    top = 1.47
    bot = 0.4 + 0.05 * np.sin(x * 13) + 0.03 * np.sin(x * 31 + 1) + 0.02 * np.abs(np.sin(x * 57))
    hw = 0.22 + (top - y) * 0.16
    wrap = 0.14 * clip01(np.abs(x) / hw) ** 2   # the sides curl forward round the body
    zc = -0.15 - 0.07 * (top - y) + wrap + (0.008 + 0.035 * (top - y)) * np.sin(x * 17 + 0.6 * np.sin(x * 9)) + 0.006 * np.sin(x * 47)
    m = np.stack([np.abs(x) - hw, np.maximum(y - top, bot - y), np.abs(z - zc) - 0.01], 1)
    parts.append(((length(np.maximum(m, 0)) + np.minimum(m.max(1), 0)) * 0.6, CLOTH))
    hand, tip = V(0.47, 0.9, 0.11), V(1.05, 0.06, 0.62)
    u = norm(tip - hand)
    parts.append((blade(q, hand + u * 0.1, tip, 0.035, 0.008, V(0, 1, 0)), BLADE))
    parts.append((capsule(q, hand - u * 0.12, hand + u * 0.08, 0.018), LEATHER))
    guard = (q - (hand + u * 0.1)) @ basis(u, V(0, 1, 0))
    parts.append((np.minimum(box(guard, V(0.12, 0.012, 0.02), 0.008), sphere(q, hand - u * 0.14, 0.03)), GOLD))
    return parts

def boss(p):
    return [(d * BOSS_S, m) for d, m in boss_local(BOSS + (p - BOSS) / BOSS_S)]

def boss_local(p):
    q = p - BOSS
    x, y, z = q[:, 0], q[:, 1], q[:, 2]
    ax = np.abs(x); qs = np.stack([ax, y, z], 1)  # mirrored
    parts = []
    # legs: banded cuisse, fanned knee cop, ridged greave, layered sabaton
    H, K, A = V(0.95, 6.0, 0.1), V(1.12, 3.3, -0.25), V(1.3, 0.8, 0.0)
    legs = grooves(capsule(qs, H, K, 0.95, 0.68), y, 0.62, 0.05)
    legs = smin(legs, capsule(qs, K, A, 0.66, 0.5), 0.2)
    legs = smin(legs, capsule(qs, K + V(0, -0.35, -0.55), A + V(0, 0.3, -0.43), 0.1), 0.12)
    cop = smin(ellipsoid(qs, K + V(0, 0.05, -0.45), V(0.62, 0.55, 0.42)), ellipsoid(qs, K + V(0.45, 0, -0.22), V(0.16, 0.55, 0.42)), 0.08)
    sab = smin(ellipsoid(qs, V(1.3, 0.42, -0.4), V(0.64, 0.44, 1.05)), ellipsoid(qs, V(1.3, 0.3, -1.25), V(0.42, 0.28, 0.6)), 0.15)
    legs = np.minimum(np.minimum(legs, cop), grooves(sab, z, 0.32, 0.04))
    # faulds: stacked lames flaring over the hips, split at the front
    sq = q * V(1, 1, 1.3)
    skirt = grooves(capsule(sq, V(0, 7.05, 0.05), V(0, 5.3, 0.05), 1.7, 2.15) / 1.3, y, 0.55, 0.06, 0.1)
    skirt = np.maximum(skirt, -box(q - V(0, 5.2, -2.0), V(0.35, 1.1, 0.8)))
    # breastplate: chest, pecs and keel over a banded abdomen
    chest = ellipsoid(q, V(0, 9.1, -0.05), V(2.05, 1.75, 1.45))
    pecs = ellipsoid(qs, V(0.85, 9.35, -0.7), V(0.95, 0.8, 0.75))
    abdomen = grooves(ellipsoid(q, V(0, 7.5, 0), V(1.55, 1.25, 1.15)), y, 0.42, 0.05)
    torso = smin(smin(chest, pecs, 0.35), abdomen, 0.45)
    torso = smin(torso, capsule(q, V(0, 7.9, -1.18), V(0, 10.1, -1.33), 0.11), 0.15)
    torso = smin(torso, skirt, 0.3)
    gorget = grooves(capsule(q, V(0, 10.35, 0.1), V(0, 11.0, 0.05), 0.98, 0.72), y, 0.2, 0.03, 0.15)
    # pauldrons: three overlapping lames
    paul = np.minimum.reduce([ellipsoid(qs, V(2.55, 10.15, 0), V(1.45, 1.0, 1.45)),
                              ellipsoid(qs, V(2.78, 9.55, 0), V(1.5, 0.8, 1.5)),
                              ellipsoid(qs, V(2.98, 9.0, 0), V(1.38, 0.62, 1.38))])
    # arms: upper arm, couter, vambrace, flared cuff, fists on the pommel
    arms = capsule(qs, V(2.7, 9.6, 0), V(2.3, 7.4, -1.5), 0.72, 0.58)
    arms = smin(arms, ellipsoid(qs, V(2.35, 7.35, -1.55), V(0.62, 0.6, 0.62)), 0.1)
    arms = smin(arms, capsule(qs, V(2.3, 7.4, -1.6), V(0.8, 8.2, -3.05), 0.55, 0.48), 0.15)
    arms = smin(arms, capsule(qs, V(1.05, 8.05, -2.85), V(0.85, 8.2, -3.05), 0.62, 0.6), 0.05)
    fist = ellipsoid(qs, V(0.45, 8.45, -3.38), V(0.4, 0.5, 0.48))
    for k in range(4):   # curled fingers
        fy = 8.12 + k * 0.21
        fist = smin(fist, capsule(qs, V(0.42, fy, -3.78), V(0.12, fy, -3.62), 0.105, 0.095), 0.04)
    fist = smin(fist, capsule(qs, V(0.55, 8.95, -3.25), V(0.2, 9.0, -3.5), 0.11), 0.04)   # thumb
    # helm: dome, faceplate, crest, brow, visor slit with breaths
    helm = smin(ellipsoid(q, V(0, 12.0, -0.1), V(0.95, 1.1, 1.05)), ellipsoid(q, V(0, 11.55, -0.55), V(0.78, 0.85, 0.72)), 0.2)
    helm = smin(helm, capsule(q, V(0, 13.02, -0.55), V(0, 12.75, 0.85), 0.12), 0.1)
    helm = smin(helm, capsule(qs, V(0, 12.18, -1.1), V(0.66, 12.15, -0.78), 0.1), 0.06)
    helm = np.maximum(helm, -box(q - V(0, 11.95, -1.1), V(0.62, 0.065, 0.6)))
    bx = x - np.round(x / 0.2) * 0.2
    breaths = box(np.stack([bx, y - 11.35, z + 1.25], 1), V(0.035, 0.22, 0.4))
    helm = np.maximum(helm, -np.maximum(breaths, np.abs(x) - 0.45))
    parts.append((np.minimum.reduce([legs, torso, gorget, paul, arms, fist, helm]), DARK))
    parts.append((sphere(qs, V(0.3, 11.95, -0.75), 0.11), EYE))
    # ribbed horns sweeping out and up
    horn = capsule(qs, V(0.75, 12.3, 0), V(1.55, 12.75, 0.1), 0.32, 0.25)
    horn = smin(horn, capsule(qs, V(1.55, 12.75, 0.1), V(2.05, 13.7, 0.2), 0.25, 0.15), 0.1)
    horn = smin(horn, capsule(qs, V(2.05, 13.7, 0.2), V(1.85, 14.7, 0.3), 0.15, 0.03), 0.08)
    parts.append((grooves(horn, ax * 0.6 + y, 0.18, 0.025), LEATHER))
    # gold: pauldron spikes and rims, couter spikes, knee bosses, the sun-ray crown
    gold = capsule(qs, V(2.7, 10.9, 0), V(3.3, 12.3, 0.2), 0.32, 0.02)
    gold = np.minimum(gold, capsule(qs, V(3.4, 10.5, 0), V(4.4, 11.4, 0.2), 0.28, 0.02))
    gold = np.minimum(gold, capsule(qs, V(2.6, 7.25, -1.4), V(3.25, 6.75, -0.6), 0.22, 0.02))
    gold = np.minimum(gold, ellipsoid(qs, K + V(0, 0.05, -0.85), V(0.2, 0.2, 0.08)))
    rq = qs - V(2.98, 8.62, 0)
    gold = np.minimum(gold, length(np.stack([np.sqrt(rq[:, 0] ** 2 + rq[:, 2] ** 2) - 1.3, rq[:, 1] * 1.6], 1)) - 0.07)
    cc = q - V(0, 12.1, 0.8)
    ang = np.arctan2(cc[:, 1], cc[:, 0])
    k = np.clip(np.round((ang - np.pi / 2) / 0.33), -4, 4)
    a = np.pi / 2 + k * 0.33
    dirs = np.stack([np.cos(a), np.sin(a), np.zeros_like(a)], 1)
    ln = (2.3 - 0.25 * np.abs(k))[:, None]
    gold = np.minimum(gold, capsule(cc, dirs * 0.9, dirs * (0.9 + ln), 0.2, 0.01))
    parts.append((gold, GOLD))
    # tattered cape with deep folds, and a tabard between the legs
    top = 10.3
    bot = 0.5 + 0.6 * np.abs(np.sin(x * 1.3)) + 0.35 * np.sin(x * 4.7 + 1)
    hw = 2.3 + (top - y) * 0.22
    zc = 1.75 + 0.07 * (top - y) + (0.1 + 0.035 * (top - y)) * np.sin(x * 1.9 + 0.3) + 0.12 * np.sin(x * 4.3)
    m = np.stack([np.abs(x) - hw, np.maximum(y - top, bot - y), np.abs(z - zc) - 0.1], 1)
    cape = (length(np.maximum(m, 0)) + np.minimum(m.max(1), 0)) * 0.6
    tz = -1.15 + 0.06 * np.sin(x * 7)
    tabard = box(np.stack([x, y - 4.0, z - tz], 1), V(0.85, 2.4, 0.05), 0.03) * 0.8
    tabard = np.maximum(tabard, (1.6 + 0.35 * np.abs(np.sin(x * 6)) - y) * 0.5)
    parts.append((np.minimum(cape, tabard), RED))
    # greatsword planted in the ground, runes glowing down the fuller
    sb = SWORD_BASE - BOSS
    parts.append((blade(q, sb + V(0, 7.3, 0), sb + V(0, -0.6, 0), 0.6, 0.14, V(0, 0, 1)), DARK))
    parts.append((box(q - (sb + V(0, 4.0, 0)), V(0.09, 2.9, 0.16), 0.04), GLOW))
    guard = box(q - (sb + V(0, 7.45, 0)), V(1.9, 0.22, 0.28), 0.1)
    guard = np.minimum(guard, sphere(qs, V(1.95, 7.55, sb[2]), 0.32))
    parts.append((np.minimum(guard, sphere(q, sb + V(0, 9.15, 0), 0.38)), GOLD))
    parts.append((capsule(q, sb + V(0, 7.6, 0), sb + V(0, 8.85, 0), 0.22), LEATHER))
    return parts

def sky_objects(p):
    parts = [(sphere(p, SUN, SUN_R), SUNM)]
    ring_c = SUN + V(0, 0, -6)
    rq = p - ring_c
    parts.append((length(np.stack([np.sqrt(rq[:, 0] ** 2 + rq[:, 1] ** 2) - SUN_R * 0.98, rq[:, 2]], 1)) - 0.9, IRON))
    for deg in (28, 152, 208, 332, 95):
        u = V(np.cos(np.radians(deg)), np.sin(np.radians(deg)), -0.05)
        u = u / np.linalg.norm(u)
        parts.append((chain(p, ring_c + u * SUN_R, u, 26, 2.2, 1.1, 0.35), IRON))
    return parts

PILLARS = [(V(-15, 0, 34), 1.8, 17, 0.3), (V(-9.5, 0, 50), 1.6, 12, -0.4),
           (V(16, 0, 31), 1.7, 10, 0.5), (V(23, 0, 47), 2.0, 19, -0.2), (V(-26, 0, 40), 2.2, 9, 0.2)]

def ruins(p):
    d = np.full(len(p), 1e9, f32)
    for c, r, h, tilt in PILLARS:
        q = p - c
        cyl = np.maximum(np.sqrt(q[:, 0] ** 2 + q[:, 2] ** 2) - r - 0.08 * np.sin(np.arctan2(q[:, 2], q[:, 0]) * 12), -q[:, 1])
        cyl = np.maximum(cyl, q[:, 1] - h - tilt * q[:, 0] * 1.5)  # broken top
        d = np.minimum(d, cyl * 0.8)
        d = np.minimum(d, box(q - V(0, 0.4, 0), V(r * 1.4, 0.4, r * 1.4), 0.1))
    for c, r in ((V(-3.5, 0, 6), 0.7), (V(2.8, 0, 3.5), 0.45), (V(5.5, 0, 9), 1.0), (V(-6, 0, 14), 1.3)):
        d = np.minimum(d, ellipsoid(p, c, V(r * 1.3, r * 0.6, r)))
    return [(d, ROCK)]

def ground(p):
    return [(p[:, 1] + 0.02 * np.sin(p[:, 0] * 1.3) * np.sin(p[:, 2] * 1.1), GROUND)]

GROUPS = [  # (bound centre, radius, fn)
    (HERO + V(0.2, 0.95, 0.15), 1.5, hero),
    (BOSS + V(0, 7.5, -0.8) * BOSS_S, 10.5 * BOSS_S, boss),
    (SUN + V(0, 0, -6), 75.0, sky_objects),
    (None, 0, ruins), (None, 0, ground),
]

def scene(p, want_mat=False):
    d = np.full(len(p), 1e9, f32); m = np.zeros(len(p), np.int8)
    for c, r, fn in GROUPS:
        if c is None:
            idx = np.arange(len(p)); dd = np.full(len(p), 1e9, f32)
        else:
            bd = sphere(p, c, r)
            near = bd < 0.5
            dd = np.where(near, 1e9, bd).astype(f32)
            idx = np.nonzero(near)[0]
        mm = np.zeros(len(p), np.int8)
        if len(idx):
            sub = p[idx]
            sd = np.full(len(idx), 1e9, f32); sm = np.zeros(len(idx), np.int8)
            for di, mi in fn(sub):
                sm = np.where(di < sd, mi, sm); sd = np.minimum(sd, di)
            dd[idx] = sd; mm[idx] = sm
        m = np.where(dd < d, mm, m); d = np.minimum(d, dd)
    return (d, m) if want_mat else d

def march(ro, rd, tmax=220.0, steps=260):
    t = np.full(len(rd), 0.05, f32); hit = np.zeros(len(rd), bool)
    active = np.arange(len(rd))
    for _ in range(steps):
        if not len(active): break
        d = scene(ro[active] + rd[active] * t[active, None])
        t[active] += d * 0.85
        done = d < 0.0015 * t[active]
        hit[active[done]] = True
        active = active[~done & (t[active] < tmax)]
    return t, hit

def soft_shadow(p, l, k=10.0, tmax=60.0, steps=70):
    res = np.ones(len(p), f32); t = np.full(len(p), 0.03, f32)
    active = np.arange(len(p))
    for _ in range(steps):
        if not len(active): break
        d = scene(p[active] + l[active] * t[active, None])
        res[active] = np.minimum(res[active], k * d / t[active])
        t[active] += np.clip(d, 0.02, 2.0)
        active = active[(res[active] > 0.01) & (t[active] < tmax)]
    return clip01(res)

def normals(p, e=0.002):
    k = np.array([[1, -1, -1], [-1, -1, 1], [-1, 1, -1], [1, 1, 1]], f32)
    n = sum(kk * scene(p + kk * e)[:, None] for kk in k)
    return norm(n)

def ambient_occ(p, n):
    occ = np.zeros(len(p), f32); s = 1.0
    for i in range(5):
        h = 0.03 + 0.16 * i
        occ += (h - scene(p + n * h)) * s; s *= 0.85
    return clip01(1 - 1.5 * occ)

SUN_COL = V(2.6, 1.15, 0.42)
KEY_DIR = norm(V(-0.55, 0.65, -0.55)); KEY_COL = V(0.5, 0.6, 0.95)

def sky(rd, sun_dir, disc=1.0):
    y = rd[:, 1:2]
    base = V(0.42, 0.1, 0.035) * (1 - clip01(y * 2.2)) + V(0.05, 0.012, 0.02) * clip01(y * 2.2)
    s = np.maximum(dot(rd, sun_dir), 0)[:, None]
    return base + V(1.3, 0.5, 0.14) * disc * s ** 18 + V(0.35, 0.1, 0.03) * s ** 4

MAT = {  # albedo, metalness, shininess
    ST: (V(0.6, 0.62, 0.66), 0.85, 70), CLOTH: (V(0.05, 0.15, 0.06), 0.0, 6), PLUME: (V(0.6, 0.04, 0.03), 0.0, 8),
    DARK: (V(0.25, 0.25, 0.28), 0.85, 45), GOLD: (V(1.0, 0.68, 0.25), 1.0, 60), RED: (V(0.3, 0.025, 0.025), 0.0, 6),
    BLADE: (V(0.78, 0.8, 0.84), 1.0, 140), IRON: (V(0.14, 0.12, 0.12), 0.6, 30), LEATHER: (V(0.12, 0.08, 0.06), 0.2, 20),
    ROCK: (V(0.2, 0.16, 0.14), 0.0, 10), GROUND: (V(0.17, 0.14, 0.13), 0.0, 20),
    GLOW: (V(0, 0, 0), 0, 1), EYE: (V(0, 0, 0), 0, 1), SUNM: (V(0, 0, 0), 0, 1),
}

def noise3(p):
    """Smooth value noise in [0, 1]."""
    i = np.floor(p); f = p - i; f = f * f * (3 - 2 * f)
    def h(o):
        n = i + o
        return np.modf(np.abs(np.sin(n[:, 0] * 127.1 + n[:, 1] * 311.7 + n[:, 2] * 74.7) * 43758.5453))[0]
    c = [h(V(a, b, cc)) for a in (0, 1) for b in (0, 1) for cc in (0, 1)]
    l = lambda a, b, t: a + (b - a) * t
    fx, fy, fz = f[:, 0], f[:, 1], f[:, 2]
    return l(l(l(c[0], c[1], fz), l(c[2], c[3], fz), fy), l(l(c[4], c[5], fz), l(c[6], c[7], fz), fy), fx)

BUMP = {ST: (0.07, 40), DARK: (0.12, 3.0), GOLD: (0.06, 5), RED: (0.2, 5), CLOTH: (0.1, 55), PLUME: (0.25, 60),
        LEATHER: (0.15, 7), ROCK: (0.2, 1.5), GROUND: (0.06, 1.2), IRON: (0.15, 2)}

def hash2(a, b): return np.modf(np.abs(np.sin(a * 12.9898 + b * 78.233) * 43758.5453))[0]

def shade(ro, rd, t, hit):
    n_px = len(rd)
    sun_dir_cam = norm(SUN - ro[0])
    col = sky(rd, sun_dir_cam)
    idx = np.nonzero(hit)[0]
    p = ro[idx] + rd[idx] * t[idx, None]
    _, m = scene(p, True)
    n = normals(p); v = -rd[idx]
    sun_l = norm(SUN - p)
    sh_k = soft_shadow(p + n * 0.01, np.broadcast_to(KEY_DIR, p.shape).copy())
    sh_s = soft_shadow(p + n * 0.01, sun_l)
    ao = ambient_occ(p, n)
    alb = np.zeros_like(p); met = np.zeros(len(p), f32); shin = np.ones(len(p), f32)
    for k, (a, me, s) in MAT.items():
        sel = m == k; alb[sel] = a; met[sel] = me; shin[sel] = s
    g = m == GROUND
    if g.any():  # flagstones with dark grooves
        gx, gz = p[g, 0] / 2.4, p[g, 2] / 2.4 + 0.5 * np.floor(p[g, 0] / 2.4)
        fx, fz = np.abs(np.modf(gx)[0]), np.abs(np.modf(gz)[0])
        edge = np.minimum(np.minimum(fx, 1 - fx), np.minimum(fz, 1 - fz))
        tone = 0.7 + 0.6 * hash2(np.floor(gx), np.floor(gz))
        alb[g] *= (tone * (0.35 + 0.65 * clip01(edge / 0.04)))[:, None]
    for k, (amp, fr) in BUMP.items():   # hammered metal, weave, stone grain
        sel = m == k
        if sel.any():
            pp = p[sel] * fr
            dn = np.stack([noise3(pp), noise3(pp + 17.3), noise3(pp + 41.7)], 1) - 0.5
            n[sel] = norm(n[sel] + dn * amp * 2)
    nl_k = clip01(dot(n, KEY_DIR))[:, None]; nl_s = clip01(dot(n, sun_l))[:, None]
    amb = (V(0.07, 0.05, 0.06) + V(0.05, 0.02, 0.01) * clip01(-n[:, 1:2])) * ao[:, None]
    F0 = V(0.04, 0.04, 0.04) * (1 - met[:, None]) + alb * met[:, None]
    ndv = clip01(dot(n, v))[:, None]
    F = F0 + (1 - F0) * (1 - ndv) ** 5
    diff = alb * (1 - met[:, None]) * (nl_k * KEY_COL * 0.9 * sh_k[:, None] + nl_s * SUN_COL * sh_s[:, None] + amb * 3)
    spec = 0
    for L, C, S in ((KEY_DIR, KEY_COL, sh_k), (sun_l, SUN_COL, sh_s)):
        hv = norm(L + v)
        spec = spec + F * C * (clip01(dot(n, hv)) ** shin)[:, None] * (shin[:, None] + 8) / 25 * S[:, None]
    refl = rd[idx] - 2 * dot(rd[idx], n)[:, None] * n
    env = sky(refl, sun_dir_cam) * np.where(refl[:, 1:2] < 0, 0.25, 1.0)
    rim = SUN_COL * 0.6 * ((1 - ndv) ** 3) * clip01(dot(n, sun_l) + 0.4)[:, None] * (0.3 + 0.7 * sh_s[:, None])
    c = diff + spec + env * F * ao[:, None] + rim * (m != GROUND)[:, None]
    # emissives
    c[m == GLOW] = V(5.0, 1.8, 0.35)
    c[m == EYE] = V(9.0, 3.2, 0.6)
    s = m == SUNM
    if s.any():
        mu = clip01(dot(n[s], v[s]))[:, None]
        gran = 1 + 0.12 * (np.sin(n[s, 0] * 23) * np.sin(n[s, 1] * 19 + 1) * np.sin(n[s, 2] * 17))[:, None]
        c[s] = (V(3.0, 1.0, 0.25) + V(4.0, 3.0, 1.4) * mu ** 0.7) * gran
    if g.any():  # molten cracks radiating from the planted sword
        dx, dz = p[g, 0] - SWORD_BASE[0], p[g, 2] - SWORD_BASE[2]
        r = np.sqrt(dx * dx + dz * dz); ang = np.arctan2(dz, dx)
        crack = clip01(1 - np.abs(np.sin(ang * 4.5 + 0.9 * np.sin(r * 0.6))) * 10 * (0.6 + r * 0.08))
        c[g] += (V(3.5, 1.0, 0.18) * (crack * np.exp(-r * 0.13))[:, None]
                 + V(1.2, 0.35, 0.06) * np.exp(-r * 0.45)[:, None])
    # atmospheric fog tinted by the sun glow
    fog = 1 - np.exp(-t[idx] * 0.011)
    fogcol = sky(norm(rd[idx] * V(1, 0.3, 1)), sun_dir_cam) * 0.9
    col[idx] = c * (1 - fog[:, None]) + fogcol * fog[:, None]
    return col

def render(ss):
    w, h = int(ART_W * ss), int(H * ss)
    ro = CAM; target = V(0.6, 6.2, 19)
    fwd = norm(target - ro); right = norm(np.cross(V(0, 1, 0), fwd)); up = np.cross(fwd, right)
    fov = np.tan(np.radians(54 / 2))
    ys, xs = np.mgrid[0:h, 0:w].astype(f32)
    u = (2 * (xs + 0.5) / w - 1) * fov * w / h; v = (1 - 2 * (ys + 0.5) / h) * fov
    rd = norm(fwd + u[..., None] * right + v[..., None] * up).reshape(-1, 3).astype(f32)
    ros = np.broadcast_to(ro, rd.shape).copy()
    t, hit = march(ros, rd)
    col = shade(ros, rd, t, hit).reshape(h, w, 3)
    # sun's screen position for the god rays
    sd = SUN - ro
    sx = (dot(sd, right) / dot(sd, fwd) / (fov * w / h) + 1) / 2
    sy = (1 - dot(sd, up) / dot(sd, fwd) / fov) / 2
    return col, (sx, sy)

# ---------------------------------------------------------------- post
def to_img(a): return Image.fromarray((clip01(a) * 255).astype(np.uint8))
def from_img(im): return np.asarray(im, f32) / 255

def post(hdr, sun_uv, rng):
    h, w, _ = hdr.shape
    lum = hdr.mean(2, keepdims=True)
    # god rays: radially smear the bright parts away from the sun
    bright = to_img(np.clip(hdr - 1.0, 0, 4) / 4)
    cx, cy = sun_uv[0] * w, sun_uv[1] * h
    acc = np.zeros_like(hdr)
    for i in range(48):
        s = 1 - i * 0.012
        im = bright.transform((w, h), Image.AFFINE, (s, 0, cx * (1 - s), 0, s, cy * (1 - s)), Image.BILINEAR)
        acc += from_img(im) * 0.97 ** i
    hdr = hdr + acc / 48 * 4 * V(1.0, 0.6, 0.3)
    # bloom
    small = to_img(np.clip(hdr - 0.9, 0, 3) / 3).resize((w // 4, h // 4), Image.BILINEAR)
    bloom = sum(from_img(small.filter(ImageFilter.GaussianBlur(r)).resize((w, h), Image.BILINEAR)) for r in (2, 6, 16))
    hdr = hdr + bloom * 0.9
    # drifting embers
    ember = Image.new('RGB', (w, h)); dr = ImageDraw.Draw(ember)
    for _ in range(110):
        x, y = rng.uniform(0, w), h * rng.uniform(0.15, 1) ** 0.7
        r = rng.uniform(0.6, 2.4) * w / ART_W
        b = rng.uniform(0.4, 1)
        dr.ellipse((x - r, y - r, x + r, y + r * 1.8), fill=(int(255 * b), int(140 * b), int(40 * b)))
    e = from_img(ember)
    hdr = hdr + e * 1.5 + from_img(ember.filter(ImageFilter.GaussianBlur(4 * w / ART_W))) * 3
    # filmic tone map + vignette
    x = np.maximum(hdr, 0)
    x = x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14)
    yy, xx = np.mgrid[0:h, 0:w]
    vig = 1 - 0.45 * (((xx / w - 0.5) * 1.1) ** 2 + ((yy / h - 0.45) * 1.3) ** 2)
    return clip01(x ** (1 / 2.2) * vig[..., None])

# ---------------------------------------------------------------- frame
def font(name, size): return ImageFont.truetype(f'/System/Library/Fonts/Supplemental/{name}', size)

def gradient_text(img, xy, text, fnt, top, bot, stroke, extrude):
    x, y = xy
    d = ImageDraw.Draw(img)
    for i in range(extrude, 0, -1):
        d.text((x + i, y + i), text, font=fnt, fill=(70, 8, 4), stroke_width=4, stroke_fill=(40, 4, 2))
    d.text((x, y), text, font=fnt, fill=stroke, stroke_width=4, stroke_fill=stroke)
    mask = Image.new('L', img.size); ImageDraw.Draw(mask).text((x, y), text, font=fnt, fill=255)
    l, t, r, b = mask.getbbox()
    grad = np.zeros((img.size[1], img.size[0], 3), f32)
    k = clip01((np.arange(img.size[1]) - t) / max(b - t, 1))[:, None, None]
    grad[:] = np.array(top, f32) * (1 - k) + np.array(bot, f32) * k
    img.paste(Image.fromarray(grad.astype(np.uint8)), (0, 0), mask)

def libdragon_logo(w):
    """The libdragon logo, from its boot screen: red dragon, the lettering dark for a light box."""
    shot = Image.open(os.path.join(ROOT, 'docs', 'media', 'libdragon_logo.png')).convert('RGB')
    a = np.asarray(shot, f32) / 255
    red = (a[..., 0] - a[..., 2]) > 0.3                  # the dragon
    white = a.min(axis=2) > 0.6                           # the lettering
    rgba = np.zeros(a.shape[:2] + (4,), f32)
    rgba[red] = (0.72, 0.12, 0.10, 1.0)
    rgba[white] = (0.22, 0.07, 0.05, 1.0)
    im = Image.fromarray((rgba * 255).astype(np.uint8), 'RGBA')
    im = im.crop(im.getbbox())
    return im.resize((w, round(im.size[1] * w / im.size[0])), Image.LANCZOS)

def frame(art):
    # The side band of the first cover (dark red, the 2025 N64brew Game Jam tag)
    old = subprocess.run(['git', '-C', ROOT, 'show', 'dfe2a2b:metadata/cover.jpg'], capture_output=True, check=True).stdout
    old = Image.open(io.BytesIO(old)).convert('RGB')
    img = Image.new('RGB', (W, H))
    img.paste(art, (0, 0))
    img.paste(old.crop((ART_W, 0, W, H)), (ART_W, 0))
    d = ImageDraw.Draw(img)
    # "Made with": the libdragon logo over Tiny3D, in the cream box (x 859-982, y 544-636)
    d.rectangle((863, 548, 978, 632), fill=(248, 239, 210))
    small = font('Georgia Bold.ttf', 14)
    d.text((920, 556), 'Made with', font=small, fill=(60, 20, 14), anchor='mt')
    logo = libdragon_logo(70)
    img.paste(logo, (921 - logo.size[0] // 2, 573), logo)
    d.text((920, 628), 'Tiny3D', font=font('Georgia Bold.ttf', 16), fill=(60, 20, 14), anchor='mb')
    # The title in engraved capitals, from one edge of the art to the other
    size = 120
    while True:
        title = ImageFont.truetype('/System/Library/Fonts/Supplemental/Copperplate.ttc', size, index=2)
        tw = d.textlength('PANDEMONIUM', font=title)
        if tw <= ART_W - 36: break
        size -= 1
    gradient_text(img, ((ART_W - tw) / 2 - 3, 10), 'PANDEMONIUM', title, (255, 236, 150), (255, 120, 20), (110, 14, 6), 5)
    return img

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ss', type=float, default=2, help='supersampling factor')
    ap.add_argument('--out', default=os.path.join(ROOT, 'metadata', 'cover.jpg'))
    ap.add_argument('--art', help='cache the rendered art here (PNG) and reuse it when it exists')
    a = ap.parse_args()
    if a.art and os.path.exists(a.art):
        art = Image.open(a.art).convert('RGB')
    else:
        hdr, sun_uv = render(a.ss)
        art = to_img(post(hdr, sun_uv, np.random.default_rng(7))).resize((ART_W, H), Image.LANCZOS)
        if a.art: art.save(a.art)
    frame(art).save(a.out, quality=92)

if __name__ == '__main__':
    main()
