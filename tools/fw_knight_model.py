#!/usr/bin/env python3
"""
The Fallen Warrior's body: the knight's own hand-made mesh (assets/knight/knight.glb), reshaped,
written to assets/fallen_knight/fw_knight.glb. Same skin, joints and order (the game draws it with
the Fallen Warrior's skeleton, built from the knight's), no clips (it plays the knight's).

  python3 tools/fw_knight_model.py

What changes (model space, glTF units: the knight faces -x, y up, z its left):
  - the helmet's visor drawn out into Griffith's beak (its black eye slit moved with it: never a gap), its dome
    broader; horns grown from its sides (part of the helmet, on the head bone), down and forward
    under the chin as on the horned war-helm
  - a broader chest, a smooth warp of every part alike (plate, mail, cloth, straps) so no seam
    opens; the shoulders, its one pauldron (mail, on the sword arm), arms, hands and sword where they were
    (its blows are read from the bones: they land as before)
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
HORN = next((a.split('=')[1] for a in sys.argv if a.startswith('--horn=')), 'B')
# (head-local, the helmet's size: it spans y 0.16..1.98, z +-0.65): from high on its side, out,
# down and forward, the tips curling in under the chin; the horned war-helm's proportions (B)
HORNS = {
    'A': ([(-0.28, 1.3, 0.52), (-0.6, 1.22, 0.98), (-0.9, 0.74, 1.08), (-1.12, 0.2, 0.92), (-1.3, -0.26, 0.68)], 0.28),
    'B': ([(-0.25, 1.5, 0.55), (-0.62, 1.42, 1.08), (-1.0, 0.8, 1.22), (-1.3, 0.05, 1.04), (-1.5, -0.6, 0.72)], 0.34),
    # B drawn in: its roots sunk into the helm's side (attached), the whole of it narrower (its z x 0.85, 0.75)
    'B1': ([(-0.3, 1.38, 0.36), (-0.62, 1.36, 0.92), (-1.0, 0.8, 1.04), (-1.3, 0.05, 0.88), (-1.5, -0.6, 0.61)], 0.32),
    'B2': ([(-0.3, 1.38, 0.34), (-0.62, 1.36, 0.81), (-1.0, 0.8, 0.92), (-1.3, 0.05, 0.78), (-1.5, -0.6, 0.54)], 0.32),
    # as the reference: springing high off the dome's sides, hugging the helm down its flanks, past
    # the jaw to tips curling in below the chin (R2: the tips reaching further forward)
    'R':  ([(-0.2, 1.75, 0.42), (-0.45, 1.55, 0.78), (-0.7, 1.0, 0.86), (-0.95, 0.35, 0.8), (-1.15, -0.25, 0.66), (-1.25, -0.62, 0.5)], 0.3),
    'R2': ([(-0.2, 1.75, 0.42), (-0.45, 1.55, 0.78), (-0.72, 1.0, 0.86), (-1.02, 0.36, 0.8), (-1.32, -0.2, 0.62), (-1.52, -0.52, 0.44)], 0.3),
    # measured off the reference (the helm's half-width ~118 px = 0.75 here): thick nearly all its
    # length (50 px at the root plate, 43 at mid-face, 25 at three-quarters, then a point), its root
    # inside the dome (fully attached), close down the helm's flanks, the tips ~0.3 of its height
    # under the chin. M2 a little thicker
    'M':  ([(-0.15, 1.62, 0.3), (-0.42, 1.58, 0.74), (-0.66, 1.1, 0.84), (-0.9, 0.45, 0.82), (-1.1, -0.12, 0.68), (-1.22, -0.42, 0.52)], 0.17),
    'M2': ([(-0.15, 1.62, 0.3), (-0.42, 1.58, 0.76), (-0.66, 1.1, 0.87), (-0.9, 0.45, 0.85), (-1.1, -0.12, 0.7), (-1.22, -0.42, 0.53)], 0.21),
    'M3': ([(-0.15, 1.62, 0.3), (-0.42, 1.58, 0.78), (-0.66, 1.1, 0.9), (-0.9, 0.45, 0.88), (-1.1, -0.12, 0.72), (-1.22, -0.42, 0.55)], 0.22),
    'C': ([(-0.22, 1.62, 0.56), (-0.66, 1.56, 1.22), (-1.1, 0.82, 1.4), (-1.46, -0.08, 1.18), (-1.7, -0.86, 0.78)], 0.4),
}
def horn_points(side):
    pts = HORNS[HORN][0]
    return [(x + HEAD[0], y + HEAD[1], side * z + HEAD[2]) for x, y, z in pts]
def cr(pts, t):
    u = t * (len(pts) - 1); k = min(int(u), len(pts) - 2); f = u - k
    p0, p1, p2, p3 = pts[max(k - 1, 0)], pts[k], pts[k + 1], pts[min(k + 2, len(pts) - 1)]
    return [0.5 * (2 * p1[m] + (-p0[m] + p2[m]) * f + (2 * p0[m] - 5 * p1[m] + 4 * p2[m] - p3[m]) * f * f + (-p0[m] + 3 * p1[m] - 3 * p2[m] + p3[m]) * f ** 3) for m in range(3)]
def norm(v): l = math.sqrt(sum(c * c for c in v)) + 1e-9; return [c / l for c in v]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
# The horns as crescent blades (the reference's): between a convex outer edge and a concave inner one
# that leaves the helm's side in one continuous arc (its upper part lying on the helm: attached), the two
# meeting at the tip; each section a diamond (a cut ridge down both faces), its facets' shading baked
# into their colours (the knight is lit by the ambient alone). Head-local, the helmet's size
# (the helmet's half-width, measured: 0.56 high up, 0.66 at the brow, 0.52 at the jaw: the inner edge just inside it)
# from the helmet's sides (back at their middle, not ahead), down along them, angling forward only
# slowly, most of it in the lower half, to the tips ahead of the chin
# (a C seen from the front, held in close to the helm and ending at the jaw: further out and longer, his
# clips' bowed head brought it onto the left shoulder)
INNER = [(-0.2, 1.62, 0.5), (-0.3, 1.35, 0.64), (-0.42, 0.95, 0.72), (-0.62, 0.45, 0.7), (-0.95, -0.05, 0.56), (-1.28, -0.42, 0.38)]
OUTER = [(-0.05, 1.76, 0.6), (-0.2, 1.62, 0.92), (-0.38, 1.15, 1.0), (-0.62, 0.55, 0.96), (-0.98, 0.02, 0.78), (-1.28, -0.42, 0.38)]
RINGS = 12
LIGHT = norm([-0.4, 0.8, 0.45])
pos, nrm, idx, shade = [], [], [], []
for side in (1, -1):
    def hl(p): return (p[0] + HEAD[0], p[1] + HEAD[1], side * p[2] + HEAD[2])
    END = 0.7       # ends at the jaw, cut blunt (past the chin, his clips' bowed head brought it onto the left shoulder)
    I = [cr([hl(p) for p in INNER], END * k / (RINGS - 1)) for k in range(RINGS)]
    O = [cr([hl(p) for p in OUTER], END * k / (RINGS - 1)) for k in range(RINGS)]
    rings = []
    for k in range(RINGS):
        f = END * k / (RINGS - 1)
        mid = [(I[k][m] + O[k][m]) * 0.5 for m in range(3)]
        T = norm([(I[min(k + 1, RINGS - 1)][m] + O[min(k + 1, RINGS - 1)][m]) - (I[max(k - 1, 0)][m] + O[max(k - 1, 0)][m]) for m in range(3)])
        Wd = norm([O[k][m] - I[k][m] for m in range(3)])
        B = norm(cross(T, Wd))                                   # through the blade (its faces' normal)
        th = 0.2 * (1 - f ** 1.6) + 0.005                        # its thickness, front to back
        rings.append([O[k], [mid[m] + B[m] * th for m in range(3)], I[k], [mid[m] - B[m] * th for m in range(3)]])
    for k in range(RINGS - 1):
        for q in range(4):
            a, b = rings[k][q], rings[k][(q + 1) % 4]
            c, e = rings[k + 1][(q + 1) % 4], rings[k + 1][q]
            n = norm(cross([b[m] - a[m] for m in range(3)], [e[m] - a[m] for m in range(3)]))
            if side < 0: n = [-v for v in n]
            base = len(pos)
            for v in (a, b, c, e): pos.append(v); nrm.append(n)
            sh = 0.5 + 0.55 * max(0.0, sum(n[m] * LIGHT[m] for m in range(3)))
            shade += [sh] * 4
            idx += ([base, base + 1, base + 2, base, base + 2, base + 3] if side > 0 else [base, base + 2, base + 1, base, base + 3, base + 2])
    cap = rings[-1]     # the cut end
    n = norm([cap[0][m] - cap[2][m] for m in range(3)]); n = norm(cross([cap[1][m] - cap[3][m] for m in range(3)], n))
    base = len(pos)
    for v in cap: pos.append(v); nrm.append(n)
    shade += [0.5 + 0.55 * max(0.0, abs(sum(n[m] * LIGHT[m] for m in range(3))))] * 4
    idx += [base, base + 1, base + 2, base, base + 2, base + 3, base, base + 2, base + 1, base, base + 3, base + 2]
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

# ---------------- a pauldron on the left shoulder: solid stacked lames and a flange at the neck, pinned to the chest ----------------
# Round the upper arm's own axis (shoulder to elbow; the arms hang in an A), over its top and outer
# side: a big top plate over the shoulder with a raised flange on its neck edge, then three lames
# down the arm, each overlapping the one below, flaring out at its lower edge (on the arm's bone)
S = SH['Left']; E = wpos(NODE['LowerArm-Left'])
U = norm([E[m] - S[m] for m in range(3)])
def perp(v): d = sum(v[m] * U[m] for m in range(3)); return norm([v[m] - d * U[m] for m in range(3)])
O = perp([0.0, float(next((a.split('=')[1] for a in sys.argv if a.startswith('--pup=')), -0.2)), 1.0])         # its outer side (its left: +z), a little up: the arc's middle (out on the arm, clear of the horn)
V = cross(U, O)
ppos, pnrm, pidx, pshade = [], [], [], []
def quad(a, b, c, e):
    # one flat facet, both sides drawn (its shading baked in: the knight is lit by the ambient alone)
    n = norm(cross([b[m] - a[m] for m in range(3)], [e[m] - a[m] for m in range(3)]))
    base = len(ppos)
    for v in (a, b, c, e): ppos.append(v); pnrm.append(n)
    pshade.extend([0.5 + 0.55 * abs(sum(n[m] * LIGHT[m] for m in range(3)))] * 4)
    pidx.extend([base, base + 1, base + 2, base, base + 2, base + 3, base, base + 2, base + 1, base, base + 3, base + 2])
def plate(rows, thick):
    # a solid plate: its outer face (the rows), an inner one set in toward the arm, and its edges
    inner = [[[p[m] - (p[m] - S[m]) / (sum((p[k] - S[k]) ** 2 for k in range(3)) ** 0.5) * thick for m in range(3)] for p in r] for r in rows]
    nr, nc = len(rows), len(rows[0])
    for i in range(nr - 1):
        for j in range(nc - 1):
            quad(rows[i][j], rows[i][j + 1], rows[i + 1][j + 1], rows[i + 1][j])
            quad(inner[i][j], inner[i + 1][j], inner[i + 1][j + 1], inner[i][j + 1])
    for j in range(nc - 1):         # the top and bottom edges (the rims)
        quad(rows[0][j], inner[0][j], inner[0][j + 1], rows[0][j + 1])
        quad(rows[-1][j], rows[-1][j + 1], inner[-1][j + 1], inner[-1][j])
    for i in range(nr - 1):         # its ends
        quad(rows[i][0], rows[i + 1][0], inner[i + 1][0], inner[i][0])
        quad(rows[i][-1], inner[i][-1], inner[i + 1][-1], rows[i + 1][-1])
def arc(t, r, half, n=6):      # points round the arm at t along it, radius r, from 0.45 of half in front of the outer side to half behind it
    out = []                    # (-V is forward: wrapped round to the front it met the horn's bulge)
    for k in range(n):
        ph = -0.45 * half + 1.45 * half * k / (n - 1)
        out.append([S[m] + U[m] * t + (O[m] * math.cos(ph) + V[m] * math.sin(ph)) * r for m in range(3)])
    return out
K = float(next((a.split('=')[1] for a in sys.argv if a.startswith('--pk=')), 0.7))       # its size
# Lying squarely over the shoulder (it stood up round the hanging arm: into the horns): a cap over the
# shoulder's top and two lames stepping down over its outside, each a band of an ellipsoid round the
# shoulder joint, the outer side only (from the front of the shoulder round to its back)
CEN = [S[0], S[1] + float(next((a.split("=")[1] for a in sys.argv if a.startswith("--py=")), -0.02)), S[2] + float(next((a.split("=")[1] for a in sys.argv if a.startswith("--pz=")), 0.45))]     # out along the shoulder: its inner edge clear of the horn
def band(ph0, ph1, grow, n_th=7):
    rows = []
    for ph in (ph0, (ph0 + ph1) * 0.5, ph1):
        row = []
        for k in range(n_th):
            th = -1.75 + 3.5 * k / (n_th - 1)             # round the shoulder: front (-) to back (+), about its outside (+z)
            sx, sz = math.sin(ph) * math.sin(-th), math.sin(ph) * math.cos(th)
            row.append([CEN[0] + 0.62 * K * grow * sx, CEN[1] + 0.42 * K * grow * math.cos(ph), CEN[2] + 0.72 * K * grow * sz])
        rows.append(row)
    return rows
for ph0, ph1, grow in ((float(next((a.split("=")[1] for a in sys.argv if a.startswith("--ptop=")), 0.35)), 0.62, 1.0), (0.52, 1.02, 1.05), (0.92, 1.38, 1.1)):
    plate(band(ph0, ph1, grow), 0.07)
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
        for q in ppos[::2]:
            for h in hv:
                d = sum((q[m] - h[m]) ** 2 for m in range(3))
                if d < gap: gap = d; worst = (yaw, pitch)
rest = min(sum((q[m] - h[m]) ** 2 for m in range(3)) for q in ppos for h in pos) ** 0.5
print("horn to pauldron: at rest %.2f, closest %.2f (glTF units) at yaw %.2f pitch %.2f" % (rest, gap ** 0.5, *worst))
kp = [pr for m in J['meshes'] for pr in m['primitives'] if MATS[pr['material']] == 'knight'][0]
_, kuv, _ = acc_rw(kp['attributes']['TEXCOORD_0']); _, kcol, _ = acc_rw(kp['attributes']['COLOR_0'])
c0 = kcol(0)
add_prim(ppos, pnrm, pidx, JOINTS.index(NODE['Spine1']), 'knight', kuv(0), [[min(1.0, c0[m] * pshade[k]) for m in range(3)] + [c0[3]] for k in range(len(ppos))])     # pinned to the chest: the arm rising high (the Holy Slam's tell) carried it into the horns

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
# a test aid's sample points (HLE_LOG_FWCLIP): the horns' in the head bone's space, the pauldron's in
# Spine1's (it's pinned to the chest) (glTF units; rest rotations are identity, so a bone's space is offset only)
AL = wpos(NODE['Spine1'])
with open(os.path.join(ROOT, "src/game/fallen_knight/fw_clip_points.h"), 'w') as f:
    f.write("// Generated by tools/fw_knight_model.py: sample points of the Fallen Warrior's horns (head bone) and\n// pauldron (pinned to Spine1), in their bones' spaces, glTF units (test aid HLE_LOG_FWCLIP)\n")
    hs = pos[:nhorn:3]; ps = ppos[::4]
    f.write("#define FW_CLIP_NH %d\n#define FW_CLIP_NP %d\n" % (len(hs), len(ps)))
    f.write("static const float FW_CLIP_H[FW_CLIP_NH][3] = {\n" + "".join("    { %.3ff, %.3ff, %.3ff },\n" % tuple(v[m] - HEAD[m] for m in range(3)) for v in hs) + "};\n")
    f.write("static const float FW_CLIP_P[FW_CLIP_NP][3] = {\n" + "".join("    { %.3ff, %.3ff, %.3ff },\n" % tuple(v[m] - AL[m] for m in range(3)) for v in ps) + "};\n")
