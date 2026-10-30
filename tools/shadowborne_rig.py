# Shadowborne's rig in its rest pose, as src/game/dragon_boss/dragon_boss.c computes it (game units,
# local to the root: x right, y up, z forward), and the great wings' joints for a fold. The model
# (tools/shadowborne_model.py) is built round these points and its bones sit on them
import math

BODY_H, CHEST_Z, HIPS_Z, NNECK, NECK_SEG, NTAIL, TAIL_SEG, LEG_L = 30.0, 26.0, -32.0, 4, 15.0, 7, 17.0, 19.0
WING_K = 2.3                     # the great wings, this much bigger than the old ones

def add(a, b): return [a[i] + b[i] for i in range(3)]
def sub(a, b): return [a[i] - b[i] for i in range(3)]
def mul(a, k): return [a[i] * k for i in range(3)]
def dot(a, b): return sum(a[i] * b[i] for i in range(3))
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
def norm(a):
    l = math.sqrt(dot(a, a)); return [x / l for x in a] if l > 1e-6 else [0.0, 0.0, 0.0]
def lerp(a, b, t): return [a[i] + (b[i] - a[i]) * t for i in range(3)]

UP, F, R = [0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [1.0, 0.0, 0.0]

def knee(a, c, bend):
    d = sub(c, a); l = math.sqrt(dot(d, d)); d = mul(d, 1.0 / l)
    lc = min(l, 2 * LEG_L - 0.01); h = math.sqrt(max(0.0, LEG_L * LEG_L - lc * lc * 0.25))
    bp = norm(sub(bend, mul(d, dot(bend, d))))
    return add(add(a, mul(d, lc * 0.5)), mul(bp, h))

def rest():
    P = {}
    P['chest'] = [0.0, BODY_H + 2.0, CHEST_Z]; P['hips'] = [0.0, BODY_H, HIPS_Z]
    P['spine0'] = lerp(P['chest'], P['hips'], 1 / 3); P['spine1'] = lerp(P['chest'], P['hips'], 2 / 3)
    n = [P['chest']]
    for i in range(NNECK):
        p = 0.6 - 0.22 * i
        n.append(add(n[-1], mul([0.0, math.sin(p), math.cos(p)], NECK_SEG)))
    for i, q in enumerate(n): P['neck%d' % i] = q
    t = [P['hips']]
    for i in range(NTAIL):
        p = -0.12 - 0.05 * i
        q = add(t[-1], mul([0.0, math.sin(p), -math.cos(p)], TAIL_SEG)); q[1] = max(q[1], 3.0); t.append(q)
    d = norm(sub(t[-1], t[-2])); t.append(add(t[-1], mul(d, 16.0)))      # the crystal blade's tip
    for i, q in enumerate(t): P['tail%d' % i] = q
    for k in range(4):
        side = 1.0 if k & 1 else -1.0
        base = P['chest'] if k < 2 else P['hips']
        sh = add(base, mul(R, side * (12.0 if k < 2 else 11.0))); sh[1] -= 6.0 if k < 2 else 4.0
        ft = add(add(sh, mul(F, 6.0 if k < 2 else -2.0)), mul(R, side * 5.0)); ft[1] = 0.0
        bend = add(add(mul(F, -1.0 if k < 2 else 0.8), mul(R, side * 0.5)), mul(UP, 0.2))
        P['shoulder%d' % k] = sh; P['knee%d' % k] = knee(sh, ft, bend); P['foot%d' % k] = ft
    return P

# The wing in its own frame (x out along the arm, y its upper side, z forward), open and folded.
# Each bone keeps its length; its direction turns from open toward folded with `fold`
W_OPEN = {'elbow': [30, 6, 4], 'wrist': [58, 10, -6],
          'tips': [[88, 5, -14], [80, 2, -32], [66, 0, -46], [48, -2, -54], [28, -3, -48]], 'back': [2, -2, -32]}
W_FOLD_DIR = {'arm': [0.70, 0.32, -0.64], 'fore': [0.10, -0.12, -0.99],
              'fingers': [[0.26, -0.40, -0.88], [0.25, -0.46, -0.85], [0.24, -0.52, -0.82], [0.23, -0.58, -0.78], [0.22, -0.64, -0.74]]}

def slerp_dir(a, b, t):
    a, b = norm(a), norm(b); c = max(-1.0, min(1.0, dot(a, b))); th = math.acos(c)
    if th < 1e-4: return a
    s = math.sin(th); return norm(add(mul(a, math.sin((1 - t) * th) / s), mul(b, math.sin(t * th) / s)))

KNUCKLE = 0.55                  # each finger's knuckle, this far from the wrist to its tip

def fold_b(d):                  # a finger's outer segment folds further (curled down under the wing)
    return norm([d[0] * 0.9, d[1] - 0.75, d[2] * 0.8])

def wing_local(fold, curl=None):
    """elbow, wrist, knuckles and fingertips in the wing's frame; `curl` (per finger) bends the outer segments down"""
    k = WING_K
    e0 = mul(W_OPEN['elbow'], k); w0 = mul(W_OPEN['wrist'], k)
    la, lf = math.sqrt(dot(e0, e0)), math.sqrt(dot(sub(w0, e0), sub(w0, e0)))
    elbow = mul(slerp_dir(e0, W_FOLD_DIR['arm'], fold), la)
    wrist = add(elbow, mul(slerp_dir(sub(w0, e0), W_FOLD_DIR['fore'], fold), lf))
    knuckles, tips = [], []
    for i, tp in enumerate(W_OPEN['tips']):
        d0 = sub(mul(tp, k), w0); l = math.sqrt(dot(d0, d0))
        da = slerp_dir(d0, W_FOLD_DIR['fingers'][i], fold)
        kn = add(wrist, mul(da, l * KNUCKLE))
        db = slerp_dir(d0, fold_b(W_FOLD_DIR['fingers'][i]), fold)
        if curl: db = slerp_dir(db, [db[0], db[1] - 0.8, db[2]], curl[i])
        knuckles.append(kn); tips.append(add(kn, mul(db, l * (1 - KNUCKLE))))
    return {'root': [0.0, 0.0, 0.0], 'elbow': elbow, 'wrist': wrist, 'knuckles': knuckles, 'tips': tips, 'back': mul(W_OPEN['back'], k)}

def wing_frame(sd, raise_, sweep, twist=0.0):
    """The wing's axes in body space (as dragon_boss_draw): x out, y up side, z forward"""
    cs, ss, c, sn, ct, st = math.cos(sweep), math.sin(sweep), math.cos(raise_), math.sin(raise_), math.cos(twist), math.sin(twist)
    x1 = [sd * R[m] * cs - F[m] * ss for m in range(3)]; z1 = [F[m] * cs + sd * R[m] * ss for m in range(3)]
    y2 = [-x1[m] * sn + UP[m] * c for m in range(3)]
    wx = [x1[m] * c + UP[m] * sn for m in range(3)]
    wy = [y2[m] * ct - z1[m] * st for m in range(3)]
    wz = [z1[m] * ct + y2[m] * st for m in range(3)]
    return wx, wy, wz

def wing_root(P, sd): return add(add(add(P['chest'], mul(UP, 10.0)), mul(R, sd * 9.0)), mul(F, 2.0))

def wing_world(P, sd, fold, raise_, sweep, twist=0.0, curl=None):
    wx, wy, wz = wing_frame(sd, raise_, sweep, twist)
    root = wing_root(P, sd)
    def to(p): return add(root, add(add(mul(wx, p[0]), mul(wy, p[1])), mul(wz, p[2])))
    L = wing_local(fold, curl)
    return {'root': root, 'elbow': to(L['elbow']), 'wrist': to(L['wrist']), 'knuckles': [to(t) for t in L['knuckles']],
            'tips': [to(t) for t in L['tips']], 'back': to(L['back']), 'up': wy}

REST_WING = dict(fold=1.0, raise_=0.25, sweep=0.0)     # the model's bind pose: folded at rest
