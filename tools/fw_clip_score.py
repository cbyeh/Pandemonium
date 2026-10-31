#!/usr/bin/env python3
"""Scores the Fallen Warrior's horns against its own blade and pauldron over a fight's poses, offline.

    python3 tools/fw_clip_score.py <log.txt> [PATH R0]

The log comes from an HLE run with HLE_LOG_FWCLIP and HLE_LOG_FWPOSE ([fwpose]: the blade, guard to tip,
and the upper arm's frame, every 4th frame, in the head bone's space). The horns are the sample points
tools/fw_knight_model.py writes to src/game/fallen_knight/fw_clip_points.h, or a trial shape: its PATH
(head-local spline points, as in the script) and root radius R0, so a horn shape is scored without a
rebuild. Prints the share of frames with the blade through a horn (within its ~0.13 half-width) and the
pauldron touching one (< 0.04 glTF units), by state.
"""
import math, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
txt = open(os.path.join(ROOT, "src/game/fallen_knight/fw_clip_points.h")).read()
def table(name):
    body = txt.split(name + '[')[1].split('= {')[1].split('};')[0]
    return [[float(x) for x in re.findall(r'-?[\d.]+', l)] for l in body.strip().splitlines()]
P = table('FW_CLIP_P')      # the pauldron, in the upper arm's space

def norm(v): l = math.sqrt(sum(c * c for c in v)) + 1e-9; return [c / l for c in v]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
def cr(pts, t):
    u = t * (len(pts) - 1); k = min(int(u), len(pts) - 2); f = u - k
    p0, p1, p2, p3 = pts[max(k - 1, 0)], pts[k], pts[k + 1], pts[min(k + 2, len(pts) - 1)]
    return [0.5 * (2 * p1[m] + (-p0[m] + p2[m]) * f + (2 * p0[m] - 5 * p1[m] + 4 * p2[m] - p3[m]) * f * f + (-p0[m] + 3 * p1[m] - 3 * p2[m] + p3[m]) * f ** 3) for m in range(3)]
def horns(path, r0, rings=12, sides=6):     # as tools/fw_knight_model.py builds them
    out = []
    for side in (1, -1):
        C = [cr([(p[0], p[1], side * p[2]) for p in path], k / (rings - 1)) for k in range(rings)]
        N = None
        for k in range(rings):
            T = norm([C[min(k + 1, rings - 1)][m] - C[max(k - 1, 0)][m] for m in range(3)])
            N = norm(cross(cross(T, N if N else [0.0, 0.0, 1.0]), T)); B = cross(T, N)
            r = r0 * (1 - k / (rings - 1)) ** 0.7
            out += [[C[k][m] + r * (math.cos(2 * math.pi * q / sides) * N[m] + math.sin(2 * math.pi * q / sides) * B[m]) for m in range(3)] for q in range(sides)]
    return out
H = horns(eval(sys.argv[2]), float(sys.argv[3]))[::2] if len(sys.argv) > 3 else table('FW_CLIP_H')

def seg(p, a, b):
    e = [b[m] - a[m] for m in range(3)]; ee = sum(x * x for x in e)
    u = max(0.0, min(1.0, sum((p[m] - a[m]) * e[m] for m in range(3)) / ee))
    return math.sqrt(sum((p[m] - a[m] - e[m] * u) ** 2 for m in range(3))), u
ST = "INTRO CHASE COMBO ROLL CLEAVE HALFMOON MOONBLADE RECOVER STAGGER DEAD SPLITTER UPDOWN TRISLAM HOLYSLAM ROLLCUT PLUNGE LUNGE".split()
tot, blade, paul = {}, {}, {}
for l in open(sys.argv[1], errors='ignore'):
    if '[fwpose]' not in l: continue
    f = l.split('[fwpose]')[1].split(); s = int(f[0]); v = [float(x) for x in f[1:]]
    if len(v) < 30: continue
    g, t, arm = v[0:3], v[3:6], [v[18 + 3 * a: 21 + 3 * a] for a in range(4)]
    tot[s] = tot.get(s, 0) + 1
    if any(d < 0.14 and u > 0.02 for d, u in (seg(h, g, t) for h in H)): blade[s] = blade.get(s, 0) + 1
    for p in P[::2]:
        q = [arm[0][m] + sum(p[a] * (arm[a + 1][m] - arm[0][m]) for a in range(3)) for m in range(3)]
        if any((h[0] - q[0]) ** 2 + (h[1] - q[1]) ** 2 + (h[2] - q[2]) ** 2 < 0.04 ** 2 for h in H): paul[s] = paul.get(s, 0) + 1; break
n = sum(tot.values())
for name, hits in (("blade through a horn", blade), ("pauldron touching a horn", paul)):
    print("%s: %.1f%% of %d frames;" % (name, 100 * sum(hits.values()) / max(1, n), n), " ".join("%s %.0f%%" % (ST[s] if 0 <= s < len(ST) else s, 100 * hits.get(s, 0) / tot[s]) for s in sorted(tot)))
