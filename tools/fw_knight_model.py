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
def horn_points(side):
    # (head-local, the helmet's size): from its side, out, down and forward, the tips under the chin
    pts = [(-0.28, 1.2, 0.5), (-0.52, 1.12, 0.84), (-0.74, 0.76, 0.92), (-0.93, 0.36, 0.8), (-1.08, 0.0, 0.62)]
    return [(x + HEAD[0], y + HEAD[1], side * z + HEAD[2]) for x, y, z in pts]
def cr(pts, t):
    u = t * (len(pts) - 1); k = min(int(u), len(pts) - 2); f = u - k
    p0, p1, p2, p3 = pts[max(k - 1, 0)], pts[k], pts[k + 1], pts[min(k + 2, len(pts) - 1)]
    return [0.5 * (2 * p1[m] + (-p0[m] + p2[m]) * f + (2 * p0[m] - 5 * p1[m] + 4 * p2[m] - p3[m]) * f * f + (-p0[m] + 3 * p1[m] - 3 * p2[m] + p3[m]) * f ** 3) for m in range(3)]
def norm(v): l = math.sqrt(sum(c * c for c in v)) + 1e-9; return [c / l for c in v]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
RINGS, SIDES = 10, 5
pos, nrm, idx = [], [], []
for side in (1, -1):
    pts = horn_points(side)
    C = [cr(pts, i / (RINGS - 1)) for i in range(RINGS)]
    base = len(pos)
    for i in range(RINGS):
        T = norm([C[min(i + 1, RINGS - 1)][m] - C[max(i - 1, 0)][m] for m in range(3)])
        N = norm(cross(T, [0, 0, 1])); W = cross(N, T)
        f = i / (RINGS - 1); r = 0.24 * (1 - f) ** 1.35 + 0.004; fl = 0.55 + 0.35 * (1 - f) ** 2
        for s in range(SIDES):
            t = s * 2 * math.pi / SIDES
            o = [N[m] * math.cos(t) * r + W[m] * math.sin(t) * r * fl for m in range(3)]
            pos.append([C[i][m] + o[m] for m in range(3)]); nrm.append(norm(o))
    for i in range(RINGS - 1):
        for s in range(SIDES):
            a, b = base + i * SIDES + s, base + i * SIDES + (s + 1) % SIDES
            c, e = a + SIDES, b + SIDES
            idx += ([a, c, b, b, c, e] if side > 0 else [a, b, c, b, e, c])
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
        "COLOR_0": add_acc('4f', 5126, 'VEC4', [col] * len(pos)),
        "JOINTS_0": add_acc('4B', 5121, 'VEC4', [[joint, 0, 0, 0]] * len(pos)),
        "WEIGHTS_0": add_acc('4f', 5126, 'VEC4', [[1.0, 0, 0, 0]] * len(pos)),
        }, "indices": add_acc('H', 5123, 'SCALAR', [[i] for i in idx], target=34963), "material": MATS.index(mat)})
add_prim(pos, nrm, idx, HEAD_J, 'head', uv0, col0)
nhorn = len(pos)

# ---------------- a great pauldron on the sword arm: stacked lames and a flange at the neck ----------------
# Round the upper arm's own axis (shoulder to elbow; the arms hang in an A), over its top and outer
# side: a big top plate over the shoulder with a raised flange on its neck edge, then three lames
# down the arm, each overlapping the one below, flaring out at its lower edge (on the arm's bone)
S = SH['Right']; E = wpos(NODE['LowerArm-Right'])
U = norm([E[m] - S[m] for m in range(3)])
def perp(v): d = sum(v[m] * U[m] for m in range(3)); return norm([v[m] - d * U[m] for m in range(3)])
O = perp([0.0, 0.35, -1.0])        # its outer side, a little up: the arc's middle
V = cross(U, O)
ppos, pnrm, pidx = [], [], []
def sheet(rows):
    # a grid of points (rows of columns), both faces (the plates are thin)
    base = len(ppos); nc = len(rows[0])
    for r in rows:
        for p in r: ppos.append(p); pnrm.append(norm([p[m] - S[m] for m in range(3)]))
    for i in range(len(rows) - 1):
        for j in range(nc - 1):
            a, b = base + i * nc + j, base + i * nc + j + 1; c, e = a + nc, b + nc
            pidx.extend([a, c, b, b, c, e, a, b, c, b, e, c])
def arc(t, r, half, n=7):      # points round the arm at t along it, radius r, +-half rad about the outer side
    out = []
    for k in range(n):
        ph = -half + 2 * half * k / (n - 1)
        out.append([S[m] + U[m] * t + (O[m] * math.cos(ph) + V[m] * math.sin(ph)) * r for m in range(3)])
    return out
LAMES = [(-0.42, 0.18, 1.12, 1.24, 1.75), (0.08, 0.5, 1.0, 1.12, 1.6), (0.38, 0.78, 0.94, 1.04, 1.5), (0.66, 1.02, 0.88, 0.97, 1.4)]
for t0, t1, r0, r1, half in LAMES:
    sheet([arc(t0, r0, half), arc(t1, r1, half)])
top = arc(LAMES[0][0], LAMES[0][2], 1.2)         # the flange: up off the top plate's neck edge
sheet([top, [[p[0], p[1] + 0.55, p[2] + 0.06] for p in top]])
kp = [pr for m in J['meshes'] for pr in m['primitives'] if MATS[pr['material']] == 'knight'][0]
_, kuv, _ = acc_rw(kp['attributes']['TEXCOORD_0']); _, kcol, _ = acc_rw(kp['attributes']['COLOR_0'])
add_prim(ppos, pnrm, pidx, JOINTS.index(NODE['UpperArm-Right']), 'knight', kuv(0), kcol(0))

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
