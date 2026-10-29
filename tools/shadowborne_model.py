# Builds Shadowborne's skinned body: assets/dragon_boss/shadowborne.glb and the bind pose the game
# drives it from (src/game/dragon_boss/shadowborne_bind.h). Run with Blender:
#   Blender -b --python tools/shadowborne_model.py -- [--preview]
# One continuous mesh (Blender's Skin modifier round the rig's points: torso, neck, tail, legs) and
# two great wings (arm, forearm, five fingers, a membrane between them), every vertex on one bone
# (tiny3d skins a vertex to one bone). Head, jaw, feet and crystals stay the game's own rigid parts.
import bpy, bmesh, math, os, sys, json, struct
from mathutils import Vector, Matrix
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shadowborne_rig as rig

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_GLB = os.path.join(ROOT, "assets/dragon_boss/shadowborne.glb")
OUT_H = os.path.join(ROOT, "src/game/dragon_boss/shadowborne_bind.h")
PREVIEW = "--preview" in sys.argv
PREVIEW_DIR = "/tmp/sbmodel"

def g2b(p): return Vector((p[0], -p[2], p[1]))         # game (x right, y up, z forward) -> Blender (z up, -y forward)
def b2g(v): return [v.x, v.z, -v.y]

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
P = rig.rest()
WING = {sd: rig.wing_world(P, sd, **rig.REST_WING) for sd in (-1, 1)}
WOPEN = {sd: rig.wing_world(P, sd, 0.0, rig.REST_WING['raise_'], rig.REST_WING['sweep']) for sd in (-1, 1)}

# ---------------- bones: (name, parent, head, tail, up rule) ----------------
# up rules (how the game rebuilds each bone's frame from the rig): 0 the body's up (rolled),
# 1 the world up, 2 forward (legs), 3 the wing's upper side, 4 the root (yaw only)
BONES = [('root', None, [0, 0, 0], [0, 10, 0], 4)]
BONES += [('chest', 'root', P['chest'], P['spine0'], 0), ('spine1', 'chest', P['spine0'], P['spine1'], 0), ('hips', 'spine1', P['spine1'], P['hips'], 0)]
for i in range(rig.NNECK): BONES.append(('neck%d' % i, 'chest' if i == 0 else 'neck%d' % (i - 1), P['neck%d' % i], P['neck%d' % (i + 1)], 1))
for i in range(rig.NTAIL + 1): BONES.append(('tail%d' % i, 'hips' if i == 0 else 'tail%d' % (i - 1), P['tail%d' % i], P['tail%d' % (i + 1)], 1))
for k in range(4):
    par = 'chest' if k < 2 else 'hips'
    BONES.append(('upper%d' % k, par, P['shoulder%d' % k], P['knee%d' % k], 2))
    BONES.append(('lower%d' % k, 'upper%d' % k, P['knee%d' % k], P['foot%d' % k], 2))
WSIDE = {-1: 'L', 1: 'R'}
for sd in (-1, 1):
    W, s = WING[sd], WSIDE[sd]
    BONES.append(('arm' + s, 'chest', W['root'], W['elbow'], 3))
    BONES.append(('fore' + s, 'arm' + s, W['elbow'], W['wrist'], 3))
    for i in range(5): BONES.append(('fing%sa%d' % (s, i), 'fore' + s, W['wrist'], W['knuckles'][i], 3))      # each finger two joints
    for i in range(5): BONES.append(('fing%sb%d' % (s, i), 'fing%sa%d' % (s, i), W['knuckles'][i], W['tips'][i], 3))
assert len(BONES) <= 48, len(BONES)

def bind_ref(rule, name):
    if rule in (0, 1): return [0.0, 1.0, 0.0]
    if rule == 2: return [0.0, 0.0, 1.0]
    return [0.0, 0.0, 1.0]
def wing_side(name): return -1 if (name.startswith('armL') or name.startswith('foreL') or name.startswith('fingL')) else 1

arm_data = bpy.data.armatures.new("shadowborne"); arm = bpy.data.objects.new("shadowborne", arm_data)
sc.collection.objects.link(arm); bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='EDIT')
for name, par, h, t, rule in BONES:
    eb = arm_data.edit_bones.new(name); eb.head = g2b(h); eb.tail = g2b(t)
    ref = WING[wing_side(name)]['up'] if rule == 3 else bind_ref(rule, name)
    eb.align_roll(g2b(ref))
    if par: eb.parent = arm_data.edit_bones[par]
    eb.use_connect = False
bpy.ops.object.mode_set(mode='OBJECT')

# ---------------- the body: Skin modifier round the rig's points ----------------
NODES, RAD = [], []
def node(p, r): NODES.append(Vector(g2b(p))); RAD.append(r); return len(NODES) - 1
EDGES, SEGS = [], []     # SEGS: (a, b, bone) the skin's segments, for the weights and UVs
def seg(a, b, bone): EDGES.append((a, b)); SEGS.append((a, b, bone))
n_chest = node(P['chest'], 18.0); n_s0 = node(P['spine0'], 17.5); n_s1 = node(P['spine1'], 16.5); n_hips = node(P['hips'], 15.5)
seg(n_chest, n_s0, 'chest'); seg(n_s0, n_s1, 'spine1'); seg(n_s1, n_hips, 'hips')
prev = n_chest
for i in range(rig.NNECK):
    nn = node(P['neck%d' % (i + 1)], [9.8, 8.6, 7.4, 6.4][i])
    seg(prev, nn, 'neck%d' % i); prev = nn
prev = n_hips
for i in range(rig.NTAIL):
    nn = node(P['tail%d' % (i + 1)], max(14.5 - 1.8 * (i + 1), 2.2) * 0.92)
    seg(prev, nn, 'tail%d' % i); prev = nn
for k in range(4):
    front = k < 2
    sh = node(P['shoulder%d' % k], 7.5 if front else 9.0)        # (a haunch: the hind thigh heavier)
    kn = node(P['knee%d' % k], 5.0 if front else 5.6)
    ft = node([P['foot%d' % k][0], 3.0, P['foot%d' % k][2]], 3.8 if front else 4.0)
    seg(sh, kn, 'upper%d' % k); seg(kn, ft, 'lower%d' % k)
# Metaballs along every segment (a chain spaced a quarter of its radius has its surface at 0.70 of
# it): one closed surface, the joints blended like muscle (the Skin modifier tore at the branches).
# The legs are their own (balls of one family merge: they swelled the belly round stubby legs); they
# sink into the torso, and only ever take their own leg's bones
BELLY_K = 0.74       # the torso's height to its width (a slimmer belly; the hit capsule is the rig's, not this)
def balls(name, segs):
    mb = bpy.data.metaballs.new(name); mb.resolution = mb.render_resolution = 2.0; mb.threshold = 0.6
    o = bpy.data.objects.new(name, mb); sc.collection.objects.link(o)
    for a_, b_, bone_ in segs:
        pa, pb = NODES[a_], NODES[b_]; L = (pb - pa).length
        n = max(2, int(L / (min(RAD[a_], RAD[b_]) * 0.25)) + 1)
        for i in range(n + 1):
            t = i / n; e = mb.elements.new(); r_ = RAD[a_] + (RAD[b_] - RAD[a_]) * t
            e.co = pa + (pb - pa) * t; e.radius = r_ / 0.70; e.stiffness = 2.0
            if bone_ in ('chest', 'spine1', 'hips'):     # the torso oval, its belly lifted (the back where it was: the crystals sit on it)
                e.type = 'ELLIPSOID'; e.size_x = e.size_y = 1.0; e.size_z = BELLY_K; e.co = e.co + Vector((0, 0, r_ * (1.0 - BELLY_K)))
    dg = bpy.context.evaluated_depsgraph_get()
    m = bpy.data.meshes.new_from_object(o.evaluated_get(dg)); bpy.data.objects.remove(o); return m
LEG = lambda bn: bn.startswith(('upper', 'lower'))
me = balls("bodyballs", [s_ for s_ in SEGS if not LEG(s_[2])])
legm = [balls("legballs%d" % k, [s_ for s_ in SEGS if s_[2] in ('upper%d' % k, 'lower%d' % k)]) for k in range(4)]
bm_ = bmesh.new(); bm_.from_mesh(me)
for lm in legm: bm_.from_mesh(lm)
bm_.to_mesh(me); bm_.free()
body = bpy.data.objects.new("body", me); sc.collection.objects.link(body)
bpy.context.view_layer.objects.active = body; body.select_set(True)
BODY_TRIS = 420
dec = body.modifiers.new("dec", 'DECIMATE'); dec.ratio = min(1.0, BODY_TRIS / max(1, sum(len(p_.vertices) - 2 for p_ in me.polygons)))
bpy.ops.object.modifier_apply(modifier="dec")
bpy.ops.object.shade_smooth()

def seg_info(p, a, b):
    pa, pb = NODES[a], NODES[b]; d = pb - pa; L = d.length
    t = max(0.0, min(1.0, (p - pa).dot(d) / (L * L)))
    q = pa + d * t
    return (p - q).length - (RAD[a] + (RAD[b] - RAD[a]) * t), t, q, d / L
def owner(p, legs=None):
    best = None
    for a, b, bone in SEGS:
        if legs is not None and LEG(bone) != legs: continue
        g, t, q, d = seg_info(p, a, b)
        if best is None or g < best[0]: best = (g, a, b, bone, t, q, d)
    return best

# one bone a vertex; the scale colours by how a vertex faces (the spine dark, the belly pale)
SPINE, BACK, BELLY = (34, 26, 52), (54, 42, 68), (100, 82, 96)     # the old purple-black (lit, a little darker and more purple)
def scale_col(nz):
    up = max(0.0, min(1.0, (nz - 0.45) / 0.55)); dn = max(0.0, min(1.0, (-nz - 0.05) / 0.75)); dn = dn * dn * (3 - 2 * dn)
    return tuple((BACK[m] + (SPINE[m] - BACK[m]) * up + (BELLY[m] - BACK[m]) * dn) / 255.0 for m in range(3))
# every face outward from the bone axis it skins round (Blender's own recalculation left the Skin
# modifier's branch hubs mixed: culled, they were holes)
bm_ = bmesh.new(); bm_.from_mesh(me)
bmesh.ops.recalc_face_normals(bm_, faces=bm_.faces)
vol = sum(f_.calc_center_median().dot(f_.normal) * f_.calc_area() for f_ in bm_.faces) / 3.0
if vol < 0.0:
    for f_ in bm_.faces: f_.normal_flip()
bm_.to_mesh(me); bm_.free(); me.update()
print("SHADOWBORNE volume", round(abs(vol)))
bm_ = bmesh.new(); bm_.from_mesh(me); bm_.verts.ensure_lookup_table()
piece = [-1] * len(bm_.verts); npieces = 0
for v0 in bm_.verts:
    if piece[v0.index] >= 0: continue
    stack = [v0]; piece[v0.index] = npieces
    while stack:
        v = stack.pop()
        for e in v.link_edges:
            w = e.other_vert(v)
            if piece[w.index] < 0: piece[w.index] = npieces; stack.append(w)
    npieces += 1
bm_.free()
votes = {}
for vi, v in enumerate(me.vertices):
    votes.setdefault(piece[vi], []).append(LEG(owner(v.co)[3]))
piece_leg = {k: sum(vs) * 2 > len(vs) for k, vs in votes.items()}     # a piece is a leg if most of it is nearest a leg
print("SHADOWBORNE pieces", npieces, "legs", sum(piece_leg.values()))
vowner = [owner(v.co, piece_leg[piece[vi]]) for vi, v in enumerate(me.vertices)]
groups = {}
for vi, o in enumerate(vowner):
    groups.setdefault(o[3], []).append(vi)
for bone, vis in groups.items():
    vg = body.vertex_groups.new(name=bone); vg.add(vis, 1.0, 'REPLACE')
col = me.color_attributes.new("Col", 'BYTE_COLOR', 'CORNER')
uv = me.uv_layers.new(name="UV")
# along each chain: a running distance (torso and tail one chain from the chest back; the neck from the chest; legs from the shoulder)
CHAIN_START = {}
def along(o):
    g, a, b, bone, t, q, d = o
    base = 0.0
    if bone in ('chest', 'spine1', 'hips') or bone.startswith('tail'):
        order = ['chest', 'spine1', 'hips'] + ['tail%d' % i for i in range(rig.NTAIL)]
        for s_ in SEGS:
            if s_[2] == bone: break
        idx = order.index(bone)
        for nm in order[:idx]:
            sa = [s_ for s_ in SEGS if s_[2] == nm][0]; base += (NODES[sa[1]] - NODES[sa[0]]).length
    elif bone.startswith('neck'):
        for i in range(int(bone[4:])):
            sa = [s_ for s_ in SEGS if s_[2] == 'neck%d' % i][0]; base += (NODES[sa[1]] - NODES[sa[0]]).length
    elif bone.startswith('lower'):
        sa = [s_ for s_ in SEGS if s_[2] == 'upper' + bone[5:]][0]; base += (NODES[sa[1]] - NODES[sa[0]]).length
    return base + t * (NODES[b] - NODES[a]).length
DENS = {'neck': 1.7 * 1.1, 'tail': 2.0, 'upper': 3.2, 'lower': 3.2}
for poly in me.polygons:
    us = []
    for li in poly.loop_indices:
        vi = me.loops[li].vertex_index; v = me.vertices[vi]; o = vowner[vi]
        g, a, b, bone, t, q, d = o
        upv = Vector((0, 0, 1)) if not bone.startswith(('upper', 'lower')) else Vector((0, -1, 0))
        xv = d.cross(upv); xv = xv.normalized() if xv.length > 1e-4 else Vector((1, 0, 0)); yv = xv.cross(d)
        r = v.co - q
        ang = math.atan2(r.dot(yv), r.dot(xv)) / (2 * math.pi)
        dens = next((DENS[k] for k in DENS if bone.startswith(k)), 1.7)
        us.append([ang, along(o) * dens / 32.0])
        col.data[li].color = (*scale_col(r.normalized().z if r.length > 1e-4 else 0.0), 1.0)     # by where it sits round its bone (normals at the branches are unreliable)
    u0 = us[0][0]
    for k, (u, vv) in enumerate(us):
        while u - u0 > 0.5: u -= 1.0
        while u - u0 < -0.5: u += 1.0
        uv.data[poly.loop_indices[k]].uv = (u, vv)

# ---------------- the great wings ----------------
# Built open (fold 0), each vertex on its bone, then carried to the bind pose (folded) by that
# bone's own move: open, the wing is exactly as designed; the membrane stretches between fingers
def bone_frame(head, tail, ref):
    yv = rig.norm(rig.sub(tail, head)); xv = rig.norm(rig.cross(ref, yv)); zv = rig.cross(xv, yv)
    return Matrix(((xv[0], yv[0], zv[0]), (xv[1], yv[1], zv[1]), (xv[2], yv[2], zv[2])))
def carry(p, bone, sd):
    """a point placed with the wing open, on `bone`, to where it is in the bind pose"""
    O, W = WOPEN[sd], WING[sd]
    s = WSIDE[sd]
    def hw(Wd):
        if bone == 'arm' + s: return Wd['root'], Wd['elbow']
        if bone == 'fore' + s: return Wd['elbow'], Wd['wrist']
        i = int(bone[-1])
        return (Wd['wrist'], Wd['knuckles'][i]) if bone[-2] == 'a' else (Wd['knuckles'][i], Wd['tips'][i])
    if bone == 'chest': return p
    h0, t0 = hw(O); h1, t1 = hw(W)
    M0, M1 = bone_frame(h0, t0, O['up']), bone_frame(h1, t1, W['up'])
    v = Vector(p) - Vector(h0)
    return list(Vector(h1) + (M1 @ M0.transposed()) @ v)

wing_bm = bmesh.new(); wvg = {}      # vertex -> bone
uvl = wing_bm.loops.layers.uv.new("UV"); cl = wing_bm.loops.layers.color.new("Col")
wing_faces_memb = []
BONE_COL = (150 / 255, 140 / 255, 146 / 255, 1.0); MEMB = (44 / 255, 30 / 255, 60 / 255, 1.0); MEMB2 = (52 / 255, 34 / 255, 70 / 255, 1.0)     # shadow, not flesh
LIMB = (54 / 255, 42 / 255, 68 / 255, 1.0)
open_pos = {}
MEMB_ALPHA = {}
for sd in (-1, 1):
    O, s = WOPEN[sd], WSIDE[sd]
    wx, wy, wz = rig.wing_frame(sd, rig.REST_WING['raise_'], rig.REST_WING['sweep'])
    def local_xy(p):      # the membrane's UVs: flat in the open wing's plane
        d = rig.sub(p, O['root']); return (rig.dot(d, wx) * 0.55 / 32.0, rig.dot(d, wz) * 0.55 / 32.0)
    def mk(p, bone):
        v = wing_bm.verts.new(g2b(carry(p, bone, sd))); wvg[v] = bone; open_pos[v] = p; return v
    fa = ['fing%sa%d' % (s, i) for i in range(5)]; fb = ['fing%sb%d' % (s, i) for i in range(5)]
    root = mk(O['root'], 'chest'); back = mk(O['back'], 'chest')
    elbow = mk(O['elbow'], 'fore' + s); wrist = mk(O['wrist'], 'fore' + s)
    kn = [mk(O['knuckles'][i], fb[i]) for i in range(5)]
    tips = [mk(O['tips'][i], fb[i]) for i in range(5)]
    for v_ in tips: MEMB_ALPHA[v_] = 0.5
    for v_ in kn: MEMB_ALPHA[v_] = 0.8
    W0 = O['wrist']
    ref0 = rig.norm(rig.sub(O['tips'][0], W0))
    def vein_uv(p):     # veins run out from the wrist along the fingers: u round the wrist (a panel half the map), v out from it
        d = rig.sub(p, W0); x_, z_ = rig.dot(d, wx), rig.dot(d, wz)
        r0x, r0z = rig.dot(ref0, wx), rig.dot(ref0, wz)
        ang = math.atan2(x_ * r0z - z_ * r0x, x_ * r0x + z_ * r0z)
        return (-ang / 0.32 * 0.5 * sd, math.sqrt(rig.dot(d, d)) / 34.0)
    def lerp3(a, b, t): return [a[c] + (b[c] - a[c]) * t for c in range(3)]
    # Each panel between two fingers: in from the wrist to the knuckles, then out to the tips, the
    # trailing edge scalloped (two notches, the first deeper: torn, Kalameet), its middle billowed
    # down a little (the membrane isn't flat); it folds at the knuckles
    tris = []
    for i in range(4):
        e1 = lerp3(lerp3(O['tips'][i], O['tips'][i + 1], 0.33), lerp3(O['knuckles'][i], O['knuckles'][i + 1], 0.33), 0.34)
        e2 = lerp3(lerp3(O['tips'][i], O['tips'][i + 1], 0.67), lerp3(O['knuckles'][i], O['knuckles'][i + 1], 0.67), 0.22)
        bl = lerp3(lerp3(O['knuckles'][i], O['knuckles'][i + 1], 0.5), lerp3(O['tips'][i], O['tips'][i + 1], 0.5), 0.45)
        span = math.sqrt(rig.dot(rig.sub(O['tips'][i], O['tips'][i + 1]), rig.sub(O['tips'][i], O['tips'][i + 1])))
        bl = rig.add(bl, rig.mul(wy, -0.09 * span))
        m1, m2, B = mk(e1, fb[i]), mk(e2, fb[i + 1]), mk(bl, fb[i])
        MEMB_ALPHA[m1] = MEMB_ALPHA[m2] = 0.42; MEMB_ALPHA[B] = 0.66
        tris += [(wrist, kn[i], kn[i + 1]), (B, kn[i], tips[i]), (B, tips[i], m1), (B, m1, m2), (B, m2, tips[i + 1]), (B, tips[i + 1], kn[i + 1]), (B, kn[i + 1], kn[i])]
    # the last panel back to the body: the scalloped edge from the last fingertip to the flank
    ma = mk(lerp3(lerp3(O['tips'][4], O['back'], 0.4), O['elbow'], 0.18), 'fore' + s)
    mb = mk(lerp3(lerp3(O['tips'][4], O['back'], 0.75), O['elbow'], 0.12), 'chest')
    MEMB_ALPHA[ma] = 0.5; MEMB_ALPHA[mb] = 0.65
    tris += [(wrist, kn[4], elbow), (kn[4], tips[4], ma), (kn[4], ma, elbow), (elbow, ma, mb), (elbow, mb, back), (elbow, back, root)]
    for k_, tri in enumerate(tris):
        if sd < 0: tri = (tri[0], tri[2], tri[1])
        try: f = wing_bm.faces.new(tri)
        except ValueError: continue
        f.material_index = 1
        uvs = [vein_uv(open_pos[lp.vert]) for lp in f.loops]
        u0 = uvs[0][0]
        for lp, (u, v) in zip(f.loops, uvs):
            while u - u0 > 0.5: u -= 1.0
            while u - u0 < -0.5: u += 1.0
            # the shadow thins out toward the edge: near solid at the arm and body, see-through at the tips
            a_ = MEMB_ALPHA.get(lp.vert, 0.9)
            c_ = MEMB if k_ & 1 else MEMB2
            lp[uvl].uv = (u, v); lp[cl] = (c_[0], c_[1], c_[2], a_)
    # The wing's limbs: the arm and forearm tapered six-sided tubes in scaled skin, the finger
    # joints thinner bone; each segment domed at both ends past its joint (they overlap there, as
    # the knees did, so a bent joint shows no gap)
    def limb(pa, pb, bone, r0, r1, sides, col, scaled, midring=True, caps=True):
        d = rig.norm(rig.sub(pb, pa)); a_ = rig.norm(rig.cross(d, wy if abs(rig.dot(d, wy)) < 0.9 else wz)); c_ = rig.cross(d, a_)
        L = math.sqrt(rig.dot(rig.sub(pb, pa), rig.sub(pb, pa)))
        def ring(p, r): return [mk(rig.add(p, rig.add(rig.mul(a_, r * math.cos(j * 2 * math.pi / sides)), rig.mul(c_, r * math.sin(j * 2 * math.pi / sides)))), bone) for j in range(sides)]
        rings = [ring(pa, r0), ring(rig.lerp(pa, pb, 0.5), (r0 + r1) * 0.5 * 1.06), ring(pb, r1)] if midring else [ring(pa, r0), ring(pb, r1)]
        cap0 = mk(rig.sub(pa, rig.mul(d, r0 * 0.7)), bone); cap1 = mk(rig.add(pb, rig.mul(d, max(r1, 0.4) * 0.7)), bone)
        vs = [0.0, 0.5, 1.0] if midring else [0.0, 1.0]
        def face(q, uvs):
            if sd < 0: q = q[::-1]; uvs = uvs[::-1]
            f = wing_bm.faces.new(q); f.material_index = 0
            for lp, uvv in zip(f.loops, uvs): lp[uvl].uv = uvv if scaled else (0.0, 0.0); lp[cl] = col
        for r in range(len(rings) - 1):
            for j in range(sides):
                j1 = (j + 1) % sides
                u0, u1 = j / sides, (j + 1) / sides
                v0, v1 = vs[r] * L * 3.2 / 32.0, vs[r + 1] * L * 3.2 / 32.0
                face((rings[r][j], rings[r][j1], rings[r + 1][j1], rings[r + 1][j]), [(u0, v0), (u1, v0), (u1, v1), (u0, v1)])
        for j in range(sides if caps else 0):
            j1 = (j + 1) % sides
            face((cap0, rings[0][j1], rings[0][j]), [(0.5, 0.0), ((j + 1) / sides, 0.0), (j / sides, 0.0)])
            face((cap1, rings[-1][j], rings[-1][j1]), [(0.5, 1.0), (j / sides, 1.0), ((j + 1) / sides, 1.0)])
    k = rig.WING_K
    q = k / 1.75       # (the limbs as thick for their length as at the first size)
    limb(O['root'], O['elbow'], 'arm' + s, 4.6 * q, 3.6 * q, 5, LIMB, True)
    limb(O['elbow'], O['wrist'], 'fore' + s, 3.4 * q, 2.6 * q, 5, LIMB, True)
    for i in range(5):
        limb(O['wrist'], O['knuckles'][i], fa[i], 2.5 * q, 1.9 * q, 4, BONE_COL, False, False, False)     # the finger bones: thick, ridged
        limb(O['knuckles'][i], O['tips'][i], fb[i], 1.9 * q, 0.35, 4, BONE_COL, False, False, False)
    # the hooked claw at the wrist (the thumb), curving forward and down
    c0 = O['wrist']; c1 = rig.add(c0, rig.add(rig.mul(wy, 5.5 * q), rig.mul(wz, 4.0 * q))); c2 = rig.add(c1, rig.add(rig.mul(wy, -1.0 * q), rig.mul(wz, 6.5 * q)))
    c3 = rig.add(c2, rig.add(rig.mul(wy, -4.5 * q), rig.mul(wz, 2.0 * q)))
    limb(c0, c1, 'fore' + s, 2.3 * q, 1.8 * q, 4, BONE_COL, False, False, False)
    limb(c1, c2, 'fore' + s, 1.8 * q, 1.1 * q, 4, BONE_COL, False, False, False)
    limb(c2, c3, 'fore' + s, 1.1 * q, 0.2, 3, BONE_COL, False, False, False)
wme = bpy.data.meshes.new("wings"); wing_bm.to_mesh(wme)
wobj = bpy.data.objects.new("wings", wme); sc.collection.objects.link(wobj)
bonenames = sorted(set(wvg.values()))
for bn in bonenames:
    vg = wobj.vertex_groups.new(name=bn); vg.add([v.index for v, b in wvg.items() if b == bn], 1.0, 'REPLACE')
wing_bm.free()

# ---------------- the crystals: down the back, neck and tail, the tail's blade, the wing joints ----------------
# As the game placed them on its old segments (put_link: a segment's frame, its mesh MESH across),
# each rigid on its bone; unlit, white and grey facets the game tints (blue, violet in the dark)
MESH = 16.0
cry_bm = bmesh.new(); cvg = {}; ccl = cry_bm.loops.layers.color.new("Col"); cuv = cry_bm.loops.layers.uv.new("UV")
def crystal_tris(base, tip, r):
    d = rig.norm(rig.sub(tip, base)); u = [1.0, 0.0, 0.0] if abs(d[1]) > 0.9 else [0.0, 1.0, 0.0]
    a_ = rig.norm(rig.cross(d, u)); c_ = rig.cross(d, a_)
    ring = [rig.add(base, rig.add(rig.mul(a_, ca * r), rig.mul(c_, sa * r))) for ca, sa in ((1, 0), (0, 1), (-1, 0), (0, -1))]
    return [(ring, tip)]
def place(crs, origin, x, y, z, sx, sy, sz, bone):
    # each crystal's five points shared (its corners light and dark in turn, the tip lit: the facets'
    # old light and dark as a gradient; split corners cost a vertex each)
    for ring, tip in crs:
        def mkv(p_):
            w = rig.add(origin, rig.add(rig.add(rig.mul(x, p_[0] * sx), rig.mul(y, p_[1] * sy)), rig.mul(z, p_[2] * sz)))
            v = cry_bm.verts.new(g2b(w)); cvg[v] = bone; return v
        R_ = [mkv(q_) for q_ in ring]; T_ = mkv(tip)
        for i in range(4):
            f = cry_bm.faces.new((R_[i], R_[(i + 1) & 3], T_)); f.material_index = 2
            for lp in f.loops:
                g = 1.0 if lp.vert is T_ else (1.0 if R_.index(lp.vert) % 2 == 0 else 0.55)
                lp[ccl] = (g, g, g, 1.0); lp[cuv].uv = (0.0, 0.0)
def seg_frame(a, b_, r, over):
    z = rig.sub(b_, a); l = math.sqrt(rig.dot(z, z)); z = rig.mul(z, 1.0 / l)
    up = [0.0, 0.0, 1.0] if abs(z[1]) > 0.95 else [0.0, 1.0, 0.0]
    x = rig.norm(rig.cross(up, z)); y = rig.cross(z, x)
    return a, x, y, z, r / MESH, r / MESH, l * over / MESH
SEG_CR = crystal_tris([0, MESH * 0.88, 3], [0, MESH * 1.95, -5], 4.0) + crystal_tris([0, MESH * 0.8, 11], [0, MESH * 1.55, 6], 3.0)
# (the old flank ridges dropped: small, they cost what the spine row does)
TSEG_CR = crystal_tris([0, MESH * 0.85, 6], [0, MESH * 1.8, -2], 3.5)
for (a, b_, r, over, bone) in ((P['chest'], P['spine0'], 18.0, 1.2, 'chest'), (P['spine0'], P['spine1'], 17.5, 1.2, 'spine1'), (P['spine1'], P['hips'], 16.0, 1.25, 'hips')):
    place(SEG_CR, *seg_frame(a, b_, r, over), bone)
for i in range(rig.NNECK): place(SEG_CR, *seg_frame(P['neck%d' % i], P['neck%d' % (i + 1)], 11.0 - i * 1.2, 1.25), 'neck%d' % i)
for i in range(rig.NTAIL): place(SEG_CR if i < 2 else TSEG_CR, *seg_frame(P['tail%d' % i], P['tail%d' % (i + 1)], 14.5 - i * 1.8, 1.2), 'tail%d' % i)
TIP = crystal_tris([0, 0, 0], [0, 0, 16.0], 3.4)
for sd_ in (-1, 1): TIP += crystal_tris([0, 0, 3.0], [sd_ * 6.0, 1.0, 8.0], 1.6)
place(TIP, *seg_frame(P['tail%d' % rig.NTAIL], P['tail%d' % (rig.NTAIL + 1)], MESH, 16.0 / math.sqrt(rig.dot(rig.sub(P['tail%d' % (rig.NTAIL + 1)], P['tail%d' % rig.NTAIL]), rig.sub(P['tail%d' % (rig.NTAIL + 1)], P['tail%d' % rig.NTAIL])))), 'tail%d' % rig.NTAIL)
WCR = crystal_tris([0, 0, 0], [1, 8, 4], 2.0)
for sd_ in (-1, 1):
    W = WING[sd_]; s_ = WSIDE[sd_]
    for j, (pt, nxt, bone, k) in enumerate(((W['elbow'], W['wrist'], 'fore' + s_, 1.0), (W['wrist'], W['knuckles'][0], 'fore' + s_, 1.1))):
        d = rig.sub(nxt, pt); x = rig.norm(rig.cross(W['up'], d)); z = rig.cross(x, W['up'])
        kk = rig.WING_K * k; place(WCR, pt, x, W['up'], z, kk, kk, kk, bone)
cme = bpy.data.meshes.new("crystals"); cry_bm.to_mesh(cme)
cobj = bpy.data.objects.new("crystals", cme); sc.collection.objects.link(cobj)
idx = {v: i for i, v in enumerate(cry_bm.verts)}
for bn in sorted(set(cvg.values())):
    vg = cobj.vertex_groups.new(name=bn); vg.add([idx[v] for v, b in cvg.items() if b == bn], 1.0, 'REPLACE')
cry_bm.free()

# ---------------- materials (tiny3d reads fast64's data: written into the .glb below) ----------------
for o, names in ((body, ['sb_body']), (wobj, ['sb_body', 'sb_memb']), (cobj, ['sb_body', 'sb_memb', 'sb_crystal'])):
    for n in names:
        m = bpy.data.materials.get(n) or bpy.data.materials.new(n)
        o.data.materials.append(m)
# join into one skinned mesh on the armature
bpy.ops.object.select_all(action='DESELECT')
wobj.select_set(True); cobj.select_set(True); body.select_set(True); bpy.context.view_layer.objects.active = body
bpy.ops.object.join()
body.name = "shadowborne_body"
body.parent = arm
mod = body.modifiers.new("arm", 'ARMATURE'); mod.object = arm
tris = sum(len(p.vertices) - 2 for p in body.data.polygons)
print("SHADOWBORNE tris", tris, "verts", len(body.data.vertices), "bones", len(BONES))

# ---------------- export ----------------
bpy.ops.object.select_all(action='DESELECT'); arm.select_set(True); body.select_set(True)
kw = dict(filepath=OUT_GLB, export_format='GLB', use_selection=True, export_skins=True, export_animations=False, export_yup=True)
try: bpy.ops.export_scene.gltf(**kw, export_vertex_color='ACTIVE')
except TypeError: bpy.ops.export_scene.gltf(**kw, export_colors=True)

# fast64 material data, after the Guardian's own (textured, lit; the membrane two-sided)
def f3d(tex, w, h, cull):
    cc1 = {"name": "", "A": 1, "B": 8, "C": 4, "D": 7, "A_alpha": 7, "B_alpha": 7, "C_alpha": 7, "D_alpha": 6}
    cc2 = {"name": "", "A": 0, "B": 8, "C": 6, "D": 7, "A_alpha": 7, "B_alpha": 7, "C_alpha": 7, "D_alpha": 0}
    texd = {"name": "", "tex_set": True, "tex": {"name": tex, "type": "Image"}, "tex_format": 1,
            "S": {"clamp": False, "mirror": False, "low": 0.0, "high": 0.0, "mask": int(math.log2(w)), "shift": 0},
            "T": {"clamp": False, "mirror": False, "low": 0.0, "high": 0.0, "mask": int(math.log2(h)), "shift": 0},
            "use_tex_reference": False, "autoprop": True}
    rdp = {"g_zbuffer": True, "g_shade": True, "g_cull_front": False, "g_cull_back": cull, "g_fog": False, "g_lighting": True,
           "g_tex_gen": False, "g_shade_smooth": True, "g_mdsft_cycletype": 0, "g_mdsft_text_filt": 2, "set_rendermode": False}
    return {"f3d_mat": {"combiner1": cc1, "combiner2": cc2, "tex0": texd,
                        "rdp_settings": rdp, "draw_layer": {"oot": 0, "sm64": 1}}}
def f3d_crystal():     # unlit: its facets' shade times the tint the game sets (prim)
    m = f3d("scales.i8.png", 128, 32, True)
    m["f3d_mat"]["combiner1"] = {"name": "", "A": 4, "B": 8, "C": 3, "D": 7, "A_alpha": 7, "B_alpha": 7, "C_alpha": 7, "D_alpha": 6}
    m["f3d_mat"]["rdp_settings"]["g_lighting"] = False; m["f3d_mat"].pop("tex0")
    return m
def f3d_shadow():      # the membrane: the smoke texture times its shade, see-through by its vertex alpha (blended)
    m = f3d("membrane_shadow.i8.png", 32, 64, False)
    m["f3d_mat"]["combiner1"].update({"A_alpha": 7, "B_alpha": 7, "C_alpha": 7, "D_alpha": 4})
    m["f3d_mat"]["draw_layer"] = {"oot": 0, "sm64": 5}
    return m
EXTRAS = {"sb_crystal": f3d_crystal(), "sb_body": f3d("scales.i8.png", 128, 32, True), "sb_memb": f3d_shadow()}
d = open(OUT_GLB, 'rb').read()
jl = struct.unpack('<I', d[12:16])[0]; js = json.loads(d[20:20 + jl]); rest_ = d[20 + jl:]
for m in js.get('materials', []):
    if m['name'] in EXTRAS: m['extras'] = EXTRAS[m['name']]
    m.pop('pbrMetallicRoughness', None)
jb = json.dumps(js, separators=(',', ':')).encode(); jb += b' ' * ((4 - len(jb) % 4) % 4)
out = struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(jb) + len(rest_)) + struct.pack('<I4s', len(jb), b'JSON') + jb + rest_
open(OUT_GLB, 'wb').write(out)

# the bind pose for the game: each bone's head, tail and the reference its frame is built from
with open(OUT_H, 'w') as f:
    f.write("// Generated by tools/shadowborne_model.py: Shadowborne's bones in the bind pose (game units, the\n"
            "// root's frame), how the game rebuilds each one's frame (rule) and from what (the reference)\n")
    f.write("#define SB_BONES %d\n" % len(BONES))
    f.write("typedef struct { const char *name; float head[3], tail[3], ref[3]; int rule; } SbBind;\n")
    f.write("static const SbBind SB_BIND[SB_BONES] = {\n")
    for name, par, h, t, rule in BONES:
        ref = WING[wing_side(name)]['up'] if rule == 3 else bind_ref(rule, name)
        f.write('    { "%s", { %.3ff, %.3ff, %.3ff }, { %.3ff, %.3ff, %.3ff }, { %.4ff, %.4ff, %.4ff }, %d },\n' % (name, *h, *t, *ref, rule))
    f.write("};\n")
print("wrote", OUT_GLB, OUT_H)
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(PREVIEW_DIR if PREVIEW else "/tmp", "shadowborne_model.blend"))
