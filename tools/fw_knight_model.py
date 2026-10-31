#!/usr/bin/env python3
"""
The Fallen Warrior's body: the knight's own hand-made mesh (assets/knight/knight.glb), reshaped,
written to assets/fallen_knight/fw_knight.glb. Same skin, joints and order (the game draws it with
the Fallen Warrior's skeleton, built from the knight's), no clips (it plays the knight's).

  python3 tools/fw_knight_model.py

What changes (model space, glTF units: the knight faces -x, y up, z its left):
  - the helmet's visor drawn out into Griffith's beak (its black eye slit moved with it: never a gap), its dome
    broader; horns grown from its temples (part of the helmet, on the head bone), down its sides
    and sweeping forward past the chin as on the horned war-helm
  - a broader chest, a smooth warp of every part alike (plate, mail, cloth, straps) so no seam
    opens; the shoulders, arms, hands and sword where they were (its blows are read from the bones:
    they land as before)
  - a great pauldron on the left shoulder (an iron dome and neck guard, three lames down the arm)
"""
import json, math, os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets/knight/knight.glb")
OUT = os.path.join(ROOT, "assets/fallen_knight/fw_knight.glb")

d = open(SRC, 'rb').read()
jl = struct.unpack('<I', d[12:16])[0]
J = json.loads(d[20:20 + jl])
BIN = bytearray(d[20 + jl + 8:])
MATS = [m['name'] for m in J['materials']]
FMT = {5126: 'f', 5123: 'H', 5121: 'B', 5125: 'I'}
NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}

def acc_rw(i):
    a = J['accessors'][i]; v = J['bufferViews'][a['bufferView']]
    off = v.get('byteOffset', 0) + a.get('byteOffset', 0); n = NC[a['type']]; f = FMT[a['componentType']]
    st = v.get('byteStride', n * struct.calcsize(f))
    get = lambda k: list(struct.unpack_from('<' + f * n, BIN, off + k * st))
    def put(k, val): struct.pack_into('<' + f * n, BIN, off + k * st, *val)
    return a['count'], get, put

# the joints' rest positions (rest rotations are identity: translations add up)
par = {}
for i, n in enumerate(J['nodes']):
    for c in n.get('children', []): par[c] = i
def wpos(i):
    t = [0.0, 0.0, 0.0]
    while i is not None:
        n = J['nodes'][i]; tr = n.get('translation', [0, 0, 0]); sc = n.get('scale', [1, 1, 1])
        t = [t[k] * sc[k] + tr[k] for k in range(3)]
        i = par.get(i)
    return t
NODE = {n['name']: i for i, n in enumerate(J['nodes'])}
JOINTS = J['skins'][0]['joints']
HEAD = wpos(NODE['Head'])
SH = {s: wpos(NODE['UpperArm-' + s]) for s in ('Left', 'Right')}
HEAD_J = JOINTS.index(NODE['Head'])

def smooth(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0))); return t * t * (3 - 2 * t)

def body_warp(p):
    x, y, z = p
    # the chest broader across (|z| inside the shoulders, between the waist and the collar)
    w = smooth(4.6, 5.8, y) * (1 - smooth(7.4, 7.9, y)) * (1 - smooth(1.5, 2.1, abs(z)))
    z = z * (1 + 0.14 * w)
    x = x - 0.12 * w * smooth(-0.6, -1.4, x)            # its front a little deeper
    return [x, y, z]

# Griffith's beak on the helmet (the slit, its own black strip, warped exactly as the shell round it); its dome broader
def head_warp(p, mat):
    x, y, z = p
    lz = z - 0.04
    if mat in ('head', 'black'):
        front = smooth(-1.45, -1.85, x) * (1 - smooth(0.25, 0.55, abs(lz)))     # the visor's point and its flanks
        eye = 1 - smooth(0.12, 0.42, abs(y - 8.06))
        x -= 0.55 * front * eye + 0.18 * front * (1 - eye) * (y > 7.9)
        y -= 0.06 * front * eye
        brow = smooth(8.25, 8.7, y) * smooth(-0.9, -1.5, x) * (1 - smooth(0.4, 0.75, abs(lz)))     # the forehead: out and up
        x -= 0.2 * brow; y += 0.06 * brow
        dome = smooth(8.3, 8.8, y)
        lz *= 1 + 0.16 * dome
        z = lz + 0.04
        y += 0.06 * dome
    return [x, y, z]

for m in J['meshes']:
    for pr in m['primitives']:
        mat = MATS[pr['material']]
        n, get, put = acc_rw(pr['attributes']['POSITION'])
        _, jget, _ = acc_rw(pr['attributes']['JOINTS_0'])
        _, wget, _ = acc_rw(pr['attributes']['WEIGHTS_0'])
        lo, hi = [1e9] * 3, [-1e9] * 3
        for k in range(n):
            p = get(k)
            onhead = jget(k)[0] == HEAD_J and wget(k)[0] > 0.5
            p = head_warp(p, mat) if onhead or mat in ('head', 'black') else body_warp(p)
            put(k, p)
            lo = [min(lo[i], p[i]) for i in range(3)]; hi = [max(hi[i], p[i]) for i in range(3)]
        a = J['accessors'][pr['attributes']['POSITION']]; a['min'] = lo; a['max'] = hi

# ---------------- the horns: grown from the helmet's sides, on the head bone ----------------
import sys
# The horned war-helm's horns (the user's reference): from the temples, a little above the eyes, out
# and slightly up, then wide and down, curling forward to sharp tips just ahead of and below the chin.
# Head-local, the helmet's size (it spans y 0.16..2.04, z +-0.65, its face at x ~-1.0): measured off the
# front view (widest ~1.6 helm half-widths out, drawn in close at the user's word, at half its height; tips ~0.85 of it in, at the chin)
# and the three-quarter one (the tips ahead of the face). A tapered tube, round in section, thick at the root
PATH = [(-0.4, 1.3, 0.3), (-0.34, 1.44, 0.66), (-0.36, 1.24, 0.88), (-0.5, 0.96, 0.88), (-0.94, 0.569, 0.73), (-1.325, 0.357, 0.58), (-1.6, 0.25, 0.43)]
RINGS, SIDES, R0 = 12, 6, 0.25
def cr(pts, t):
    u = t * (len(pts) - 1); k = min(int(u), len(pts) - 2); f = u - k
    p0, p1, p2, p3 = pts[max(k - 1, 0)], pts[k], pts[k + 1], pts[min(k + 2, len(pts) - 1)]
    return [0.5 * (2 * p1[m] + (-p0[m] + p2[m]) * f + (2 * p0[m] - 5 * p1[m] + 4 * p2[m] - p3[m]) * f * f + (-p0[m] + 3 * p1[m] - 3 * p2[m] + p3[m]) * f ** 3) for m in range(3)]
def norm(v): l = math.sqrt(sum(c * c for c in v)) + 1e-9; return [c / l for c in v]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
LIGHT = norm([-0.4, 0.8, 0.45])
pos, nrm, idx, shade = [], [], [], []
for side in (1, -1):
    C = [cr([(p[0] + HEAD[0], p[1] + HEAD[1], side * p[2] + HEAD[2]) for p in PATH], k / (RINGS - 1)) for k in range(RINGS)]
    rings, N = [], None
    for k in range(RINGS):
        T = norm([C[min(k + 1, RINGS - 1)][m] - C[max(k - 1, 0)][m] for m in range(3)])
        N = norm(cross(cross(T, N if N else [0.0, 0.0, 1.0]), T))     # carried along the curve (no twist)
        Bn = cross(T, N)
        r = R0 * (1 - k / (RINGS - 1)) ** 0.7
        rings.append([[C[k][m] + r * (math.cos(2 * math.pi * q / SIDES) * N[m] + math.sin(2 * math.pi * q / SIDES) * Bn[m]) for m in range(3)] for q in range(SIDES)])
    for k in range(RINGS - 1):
        for q in range(SIDES):
            a, b = rings[k][q], rings[k][(q + 1) % SIDES]
            c, e = rings[k + 1][(q + 1) % SIDES], rings[k + 1][q]
            n = norm([(a[m] + b[m] + c[m] + e[m]) * 0.25 - (C[k][m] + C[k + 1][m]) * 0.5 for m in range(3)])     # facing out from the axis
            base = len(pos)
            for v in (a, b, c, e): pos.append(v); nrm.append(n)
            shade += [0.5 + 0.55 * max(0.0, sum(n[m] * LIGHT[m] for m in range(3)))] * 4
            f = norm(cross([b[m] - a[m] for m in range(3)], [e[m] - a[m] for m in range(3)]))
            idx += ([base, base + 1, base + 2, base, base + 2, base + 3] if sum(f[m] * n[m] for m in range(3)) > 0 else [base, base + 2, base + 1, base, base + 3, base + 2])
# The forehead: four flat facets meeting at one point above the eyes (a low diamond pyramid, the
# reference's brow), proud of the helmet over its brow, down to the beak
FH = {'top': (-0.62, 1.96, 0.0), 'l': (-0.86, 1.36, 0.6), 'r': (-0.86, 1.36, -0.6), 'bot': (-1.56, 0.96, 0.0), 'pt': (-1.34, 1.44, 0.0)}
FH = {k: (v[0] + HEAD[0], v[1] + HEAD[1], v[2] + HEAD[2]) for k, v in FH.items()}
for a, b in (('top', 'l'), ('r', 'top'), ('l', 'bot'), ('bot', 'r')):
    tri = [FH[a], FH['pt'], FH[b]]
    n = norm(cross([tri[1][m] - tri[0][m] for m in range(3)], [tri[2][m] - tri[0][m] for m in range(3)]))
    if n[0] > 0: tri = [tri[0], tri[2], tri[1]]; n = [-v for v in n]      # facing out, the front (-x)
    base = len(pos)
    for v in tri: pos.append(list(v)); nrm.append(n)
    shade += [0.5 + 0.55 * max(0.0, sum(n[m] * LIGHT[m] for m in range(3)))] * 3
    idx += [base, base + 1, base + 2]

# the helmet's own UVs and colour: a plain spot of its texture (its vertex 9, the crown)
hp = [pr for m in J['meshes'] for pr in m['primitives'] if MATS[pr['material']] == 'head'][0]
_, uvget, _ = acc_rw(hp['attributes']['TEXCOORD_0']); _, cget, _ = acc_rw(hp['attributes']['COLOR_0'])
uv0, col0 = uvget(9), cget(9)

def add_view(data, target=None):
    while len(BIN) % 4: BIN.append(0)
    off = len(BIN); BIN.extend(data)
    v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
    if target: v["target"] = target
    J['bufferViews'].append(v); return len(J['bufferViews']) - 1
def add_acc(fmt, ctype, typ, rows, target=34962, minmax=False):
    data = b''.join(struct.pack('<' + fmt, *r) for r in rows)
    a = {"bufferView": add_view(data, target), "componentType": ctype, "count": len(rows), "type": typ}
    if minmax: a["min"] = [min(r[i] for r in rows) for i in range(3)]; a["max"] = [max(r[i] for r in rows) for i in range(3)]
    J['accessors'].append(a); return len(J['accessors']) - 1
def add_prim(pos, nrm, idx, joint, mat, uv, col):
    J['meshes'][0]['primitives'].append({"attributes": {
        "POSITION": add_acc('3f', 5126, 'VEC3', pos, minmax=True),
        "NORMAL": add_acc('3f', 5126, 'VEC3', nrm),
        "TEXCOORD_0": add_acc('2f', 5126, 'VEC2', [uv] * len(pos)),
        "COLOR_0": add_acc('4f', 5126, 'VEC4', col if isinstance(col[0], list) else [col] * len(pos)),
        "JOINTS_0": add_acc('4B', 5121, 'VEC4', [[joint, 0, 0, 0]] * len(pos)),
        "WEIGHTS_0": add_acc('4f', 5126, 'VEC4', [[1.0, 0, 0, 0]] * len(pos)),
        }, "indices": add_acc('H', 5123, 'SCALAR', [[i] for i in idx], target=34963), "material": MATS.index(mat)})
add_prim(pos, nrm, idx, HEAD_J, 'head', uv0, [[min(1.0, col0[m] * shade[k]) for m in range(3)] + [col0[3]] for k in range(len(pos))])
nhorn = len(pos)

# ---------------- a pauldron on the left shoulder (the user's reference) ----------------
# A big hammered dome over the shoulder with a guard standing up at its neck edge, flaring out (dark
# iron), and three dark brownish red lames stepping down the upper arm, each
# tucked under the one above, a ridge down its middle; all on the arm's bone (they swing with it)
NC['MAT4'] = 16
_, ibm, _ = acc_rw(J['skins'][0]['inverseBindMatrices'])
def bind(j):        # a joint's bind matrix's inverse (column-major): model space to the bone's own
    return ibm(JOINTS.index(NODE[j]))
def to_bone(M, p): return [M[r] * p[0] + M[4 + r] * p[1] + M[8 + r] * p[2] + M[12 + r] for r in range(3)]
def joint_at(j):    # the joint in model space (its bind's rotation is orthonormal: -R^T t)
    M = bind(j); return [-(M[4 * c] * M[12] + M[4 * c + 1] * M[13] + M[4 * c + 2] * M[14]) for c in range(3)]
S, E = joint_at('UpperArm-Left'), joint_at('LowerArm-Left')
U = norm([E[m] - S[m] for m in range(3)]); ARM = math.sqrt(sum((E[m] - S[m]) ** 2 for m in range(3)))
def dot(a, b): return sum(a[m] * b[m] for m in range(3))
ppos, pnrm, pidx, pshade = [], [], [], []
def quad(a, b, c, e, away=None, mul=1.0):
    # one flat facet (its shading baked in: the knight is lit by the ambient alone): facing away, or both sides drawn
    n = norm(cross([b[m] - a[m] for m in range(3)], [e[m] - a[m] for m in range(3)]))
    if away is not None and dot(n, away) < 0: a, b, c, e = a, e, c, b; n = [-v for v in n]
    base = len(ppos)
    for v in (a, b, c, e): ppos.append(v); pnrm.append(n)
    pshade.extend([mul * (0.3 + 0.85 * (abs(dot(n, LIGHT)) if away is None else max(0.0, dot(n, LIGHT))))] * 4)     # (one-sided: its shadowed side dark)
    pidx.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    if away is None: pidx.extend([base, base + 2, base + 1, base, base + 3, base + 2])
def plate(rows, thick, outward, both=False, rowmul=None):
    # a solid plate: its outer face (the rows), facing out(p), an inner one set in by thick (drawn only where
    # it shows: both), and its rims, each facing out
    inner = [[[p[m] - outward(p)[m] * thick for m in range(3)] for p in r] for r in rows]
    nr, nc = len(rows), len(rows[0])
    mid = [sum(p[m] for r in rows for p in r) / (nr * nc) for m in range(3)]
    out = lambda *q: [sum(v[m] for v in q) / len(q) - mid[m] for m in range(3)]
    for i in range(nr - 1):
        for j in range(nc - 1):
            f = (rows[i][j], rows[i][j + 1], rows[i + 1][j + 1], rows[i + 1][j])
            quad(*f, away=outward(f[0]), mul=rowmul[i] if rowmul else 1.0)
            if both: quad(inner[i][j], inner[i + 1][j], inner[i + 1][j + 1], inner[i][j + 1], away=[-v for v in outward(f[0])])
    for j in range(nc - 1):
        quad(rows[0][j], inner[0][j], inner[0][j + 1], rows[0][j + 1], away=out(rows[0][j], rows[0][j + 1]), mul=0.45)
        quad(rows[-1][j], rows[-1][j + 1], inner[-1][j + 1], inner[-1][j], away=out(rows[-1][j], rows[-1][j + 1]), mul=0.45)
    for i in range(nr - 1):
        quad(rows[i][0], rows[i + 1][0], inner[i + 1][0], inner[i][0], away=out(rows[i][0], rows[i + 1][0]), mul=0.45)
        quad(rows[i][-1], inner[i][-1], inner[i + 1][-1], rows[i + 1][-1], away=out(rows[i][-1], rows[i + 1][-1]), mul=0.45)
# The dome: a bowl over the shoulder, its crown up and out from the joint (square to the arm), reaching
# further down the arm than in toward the neck, its rim flared into a lip
OA = norm([(0.0 if m != 1 else 1.0) - U[1] * U[m] for m in range(3)]); OB = cross(U, OA)       # round the arm: up-out, and across
CEN = [S[m] + 0.04 * OA[m] for m in range(3)]
DA, DX = 0.58, 0.7
def dome(ph, th, k=1.0):     # th = 0 down the arm, pi in at the neck
    ru = 0.54 + 0.2 * math.cos(th)
    return [CEN[m] + k * (DA * math.cos(ph) * OA[m] + math.sin(ph) * (DX * math.sin(th) * OB[m] + ru * math.cos(th) * U[m])) for m in range(3)]
dent = lambda i, j: 1.0 + 0.03 * math.sin(i * 2.3 + j * 1.7) * math.sin(j * 0.9 + i * 3.1)     # hammered
NT = 12
plate([[dome(0.04 + 1.2 * i / 4, 2 * math.pi * j / NT, dent(i, j % NT) * (1.08 if i == 4 else 1.0)) for j in range(NT + 1)] for i in range(5)], 0.07, lambda p: norm([p[m] - CEN[m] for m in range(3)]))
# the guard: standing on the dome's neck-side rim, square to the shoulder line, its top rolled out
guard = []
for i, (up, out) in enumerate(((0.0, 0.0), (0.22, -0.01), (0.4, 0.08))):
    row = []
    for j in range(7):
        u = -1.0 + 2.0 * j / 6
        h = 1.0 - 0.5 * u * u
        b0 = dome(1.0, math.pi - 0.95 * u)
        row.append([b0[m] + up * h * OA[m] - (up * h * 0.6 - out * h) * U[m] for m in range(3)])
    guard.append(row)
plate(guard, 0.05, lambda p: U, both=True)
ncap = len(ppos)
# the lames: bands round the upper arm's outer side, each flaring at its lower edge over the next
def lame(t0, t1, r):
    rows = []
    for t, rr in ((t0, r), ((t0 + t1) * 0.5, r + 0.02), (t1, r + 0.06)):
        row = []
        for j in range(7):
            th = -2.0 + 4.0 * j / 6
            ridge = 0.035 * max(0.0, 1.0 - abs(th) / 0.45)
            c = [S[m] + U[m] * t * ARM for m in range(3)]
            row.append([c[m] + (rr + ridge) * (math.cos(th) * OA[m] + math.sin(th) * OB[m]) for m in range(3)])
        rows.append(row)
    return rows
def to_axis(p):
    t = dot([p[m] - S[m] for m in range(3)], U); return norm([p[m] - S[m] - U[m] * t for m in range(3)])
for i in range(3):
    plate(lame(0.34 + 0.17 * i, 0.54 + 0.17 * i, 0.39 + 0.025 * i), 0.05, to_axis, rowmul=(0.6, 1.05))     # each in the shadow of the one above, lit at its rolled lower edge
# the horns never through it: the closest the two come, the head turned and tipped as far as the life layer takes it
def rot(p, yaw, pitch):
    x, y, z = p[0] - HEAD[0], p[1] - HEAD[1], p[2] - HEAD[2]
    c, s_ = math.cos(yaw), math.sin(yaw); x, z = x * c + z * s_, -x * s_ + z * c
    c, s_ = math.cos(pitch), math.sin(pitch); x, y = x * c - y * s_, x * s_ + y * c
    return (x + HEAD[0], y + HEAD[1], z + HEAD[2])
gap = 1e9
for yaw in (-0.85, -0.45, 0.0, 0.25, 0.45):         # (the head's look and flinch, and its clips' own tilt, relative to the chest; toward the pauldron (+) the life layer turns it at most 0.45)
    for pitch in (-0.4, 0.0, 0.4):
        hv = [rot(p, yaw, pitch) for p in pos[::3]]
        for q in ppos[:ncap:2]:
            for h in hv:
                d = sum((q[m] - h[m]) ** 2 for m in range(3))
                if d < gap: gap = d; worst = (yaw, pitch)
rest = min(sum((q[m] - h[m]) ** 2 for m in range(3)) for q in ppos[:ncap] for h in pos) ** 0.5
print("horn to pauldron: at rest %.2f, closest %.2f (glTF units) at yaw %.2f pitch %.2f" % (rest, gap ** 0.5, *worst))
kp = [pr for m in J['meshes'] for pr in m['primitives'] if MATS[pr['material']] == 'knight'][0]
_, kuv, _ = acc_rw(kp['attributes']['TEXCOORD_0']); _, kcol, _ = acc_rw(kp['attributes']['COLOR_0'])
c0 = kcol(0)
pcol = [[min(1.0, c0[m] * pshade[k]) for m in range(3)] + [c0[3]] for k in range(len(ppos))]
# its own materials, copies of the knight's (the game tints them): the dome and guard dark iron, the lames dark brownish red
for name in ('iron', 'lames'):
    J['materials'].append(dict(J['materials'][MATS.index('knight')], name=name)); MATS.append(name)
add_prim(ppos[:ncap], pnrm[:ncap], [i for i in pidx if i < ncap], JOINTS.index(NODE['UpperArm-Left']), 'iron', kuv(0), pcol[:ncap])     # on the arm (pinned to the chest, it was left behind the shoulder as the clips swing the arm forward)
add_prim(ppos[ncap:], pnrm[ncap:], [i - ncap for i in pidx if i >= ncap], JOINTS.index(NODE['UpperArm-Left']), 'lames', kuv(0), pcol[ncap:])

# The sword on its own bone (IK_Hand.R, which nothing else is skinned to): the game sets it on the
# hand each frame, and turns the sword in the grip for the lunge's thrust (fallen_knight.c sword_carry)
SWORD_J = JOINTS.index(NODE['IK_Hand.R'])
for m in J['meshes']:
    for pr in m['primitives']:
        if MATS[pr['material']] != 'sword': continue
        n, _, jput = acc_rw(pr['attributes']['JOINTS_0'])
        for k in range(n): jput(k, [SWORD_J, 0, 0, 0])
        n, _, wput = acc_rw(pr['attributes']['WEIGHTS_0'])
        for k in range(n): wput(k, [1.0, 0.0, 0.0, 0.0] if J['accessors'][pr['attributes']['WEIGHTS_0']]['componentType'] == 5126 else [255, 0, 0, 0])

J.pop('animations', None)       # (it plays the knight's own)
J['buffers'][0]['byteLength'] = len(BIN)
while len(BIN) % 4: BIN.append(0)
J['buffers'][0]['byteLength'] = len(BIN)
jb = json.dumps(J, separators=(',', ':')).encode(); jb += b' ' * ((4 - len(jb) % 4) % 4)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, 'wb') as f:
    f.write(struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(jb) + 8 + len(BIN)))
    f.write(struct.pack('<I4s', len(jb), b'JSON')); f.write(jb)
    f.write(struct.pack('<I4s', len(BIN), b'BIN\0')); f.write(BIN)
print("wrote", OUT, "horn verts", nhorn, "pauldron verts", len(ppos))
# a test aid's sample points (HLE_LOG_FWCLIP), each in its bone's own space (glTF units): the horns'
# (head), the pauldron's (the upper arm)
LA = bind('UpperArm-Left')
with open(os.path.join(ROOT, "src/game/fallen_knight/fw_clip_points.h"), 'w') as f:
    f.write("// Generated by tools/fw_knight_model.py: sample points of the Fallen Warrior's horns (head bone), its\n// pauldron (UpperArm-Left), in their bones' spaces, glTF units (test aid HLE_LOG_FWCLIP)\n")
    hs = [to_bone(bind('Head'), v) for v in pos[:nhorn:3]]
    ps = [to_bone(LA, v) for v in ppos[::4]]
    f.write("#define FW_CLIP_NH %d\n#define FW_CLIP_NP %d\n" % (len(hs), len(ps)))
    f.write("static const float FW_CLIP_H[FW_CLIP_NH][3] = {\n" + "".join("    { %.3ff, %.3ff, %.3ff },\n" % tuple(v) for v in hs) + "};\n")
    f.write("static const float FW_CLIP_P[FW_CLIP_NP][3] = {\n" + "".join("    { %.3ff, %.3ff, %.3ff },\n" % tuple(v) for v in ps) + "};\n")
