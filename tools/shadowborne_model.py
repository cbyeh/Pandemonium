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

def g2b(p): return Vector((p[0], -p[2], p[1]))
# A mesh colour attribute's floats are linear (the importer turns them back to gamma, ^0.4545): what
# the game is to draw, in its own 0-255 values, goes in as (v / 255)^2.2 (as they were, they came out
# ~2.3x bright). (bmesh's colour layers take them as they are)
def lin(c): return tuple((min(1.0, max(0.0, x)) ** 2.2) if i < 3 else x for i, x in enumerate(c))         # game (x right, y up, z forward) -> Blender (z up, -y forward)
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
SPINE, BACK, BELLY = (40, 36, 58), (58, 52, 68), (112, 98, 100)     # the old parts' own     # the old purple-black (lit, a little darker and more purple)
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
        col.data[li].color = lin((*scale_col(r.normalized().z if r.length > 1e-4 else 0.0), 1.0))     # by where it sits round its bone (normals at the branches are unreliable)
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
# The old wing (dragon_boss.c's build_wing, scaled up WING_K): bone spikes for the arm, forearm and
# fingers (the scale map's belly-plate band: growth rings), an ivory claw at the wrist, the membrane
# scalloped between the fingertips in two purples (its own texture, flat across the open wing), with
# its old flat light baked into it (unlit: seen alike from both sides). Its points sit on the bones
# (a finger's knuckle splits its spike and the membrane bends there); the edge on the body, on the arm
SRGB = lambda r, g, b, a=1.0: (r / 255, g / 255, b / 255, a)
BONE_COL = SRGB(150, 140, 146); IVORY = SRGB(232, 226, 210)
open_pos = {}
for sd in (-1, 1):
    O, s = WOPEN[sd], WSIDE[sd]
    k = rig.WING_K
    wx, wy, wz = rig.wing_frame(sd, rig.REST_WING['raise_'], rig.REST_WING['sweep'])
    def mk(p, bone):
        v = wing_bm.verts.new(g2b(carry(p, bone, sd))); wvg[v] = bone; open_pos[v] = p; return v
    fa = ['fing%sa%d' % (s, i) for i in range(5)]; fb = ['fing%sb%d' % (s, i) for i in range(5)]
    def wl(p):      # a point in the open wing's own frame
        d = rig.sub(p, O['root']); return (rig.dot(d, wx), rig.dot(d, wy), rig.dot(d, wz))
    def shade(c, n):    # the old flat light (proc_mesh: 0.4 + 0.75 |n . key|, seen from either side)
        l = math.sqrt(rig.dot(n, n)) or 1.0; dd = abs(n[0] * -0.45 + n[1] * 0.8 + n[2] * 0.4) / l
        f = 0.4 + 0.75 * dd; return (min(1.0, c[0] * f), min(1.0, c[1] * f), min(1.0, c[2] * f), 1.0)
    # --- the membrane
    root = mk(O['root'], 'arm' + s); back = mk(O['back'], 'arm' + s)
    elbow = mk(O['elbow'], 'fore' + s); wrist = mk(O['wrist'], 'fore' + s)
    kn = [mk(O['knuckles'][i], fb[i]) for i in range(5)]
    tips = [mk(O['tips'][i], fb[i]) for i in range(5)]
    mids = []
    for i in range(4):      # the edge between two fingertips sags in toward the wrist
        m = [(O['tips'][i][c] + O['tips'][i + 1][c]) * 0.5 * 0.78 + O['wrist'][c] * 0.22 for c in range(3)]
        mids.append(mk(m, fb[i]))
    m45 = mk([(O['tips'][4][c] + O['back'][c]) * 0.5 * 0.8 + O['elbow'][c] * 0.2 for c in range(3)], 'fore' + s)
    MEMB_C, MEMB_2 = (52, 42, 62), (64, 46, 72)
    tris = []
    for i in range(4):
        c1, c2 = (MEMB_C, MEMB_2) if i & 1 else (MEMB_2, MEMB_C)     # (the old pair of triangles a panel, each its purple)
        tris += [((wrist, kn[i], kn[i + 1]), c1), ((kn[i], tips[i], mids[i]), c1), ((kn[i], mids[i], kn[i + 1]), c1), ((kn[i + 1], mids[i], tips[i + 1]), c2)]
    tris += [((wrist, tips[4], elbow), MEMB_C), ((elbow, tips[4], m45), MEMB_2), ((elbow, m45, back), MEMB_C), ((elbow, back, root), MEMB_2)]
    # (the last panel's tip-to-elbow triangle crosses the knuckle: split it there as the others)
    tris[16] = ((wrist, kn[4], elbow), MEMB_C); tris.insert(17, ((kn[4], tips[4], elbow), MEMB_C))
    for tri, col in tris:
        if sd < 0: tri = (tri[0], tri[2], tri[1])
        try: f = wing_bm.faces.new(tri)
        except ValueError: continue
        f.material_index = 1
        P3 = [open_pos[v] for v in (f.verts[0], f.verts[1], f.verts[2])]
        n = rig.cross(rig.sub(P3[1], P3[0]), rig.sub(P3[2], P3[0]))
        c3 = shade(SRGB(*col), n)
        for lp in f.loops:
            x_, y_, z_ = wl(open_pos[lp.vert]); lp[uvl].uv = (x_ * 0.55 / 32.0, z_ * 0.55 / 32.0); lp[cl] = c3
    # --- the bones: spikes (four-sided, tapering), a finger's split at its knuckle
    def spike(pa, pb, bone, r0, r1, col, along):
        d = rig.norm(rig.sub(pb, pa)); a_ = rig.norm(rig.cross(d, wy if abs(rig.dot(d, wy)) < 0.9 else wz)); c_ = rig.cross(d, a_)
        L = math.sqrt(rig.dot(rig.sub(pb, pa), rig.sub(pb, pa)))
        ring = lambda p, r: [mk(rig.add(p, rig.add(rig.mul(a_, r * math.cos(j * math.pi / 2)), rig.mul(c_, r * math.sin(j * math.pi / 2)))), bone) for j in range(4)]
        A = ring(pa, r0); B = ring(pb, r1) if r1 > 0.01 else None; T = mk(pb, bone) if B is None else None
        for j in range(4):
            j1 = (j + 1) % 4; u0, u1 = (92 + 2 * j) / 128.0, (94 + 2 * j) / 128.0; v1 = L * along / 32.0
            q = (A[j], A[j1], B[j1], B[j]) if B else (A[j], A[j1], T)
            uvs = [(u0, 0), (u1, 0), (u1, v1), (u0, v1)] if B else [(u0, 0), (u1, 0), ((u0 + u1) / 2, v1)]
            if sd < 0: q = q[::-1]; uvs = uvs[::-1]
            f = wing_bm.faces.new(q); f.material_index = 3      # (sb_bone: unlit, the old flat light baked in, like the horns)
            nrm = rig.cross(rig.sub(open_pos[f.verts[1]], open_pos[f.verts[0]]), rig.sub(open_pos[f.verts[2]], open_pos[f.verts[0]]))
            if sd < 0: nrm = rig.mul(nrm, -1.0)
            c3 = shade(col, nrm)
            for lp, uvv in zip(f.loops, uvs): lp[uvl].uv = uvv; lp[cl] = c3
    spike(O['root'], O['elbow'], 'arm' + s, 2.6 * k, 2.1 * k * 0.97, BONE_COL, 0.9)
    spike(O['elbow'], O['wrist'], 'fore' + s, 2.1 * k, 0.6 * k, BONE_COL, 0.9)
    claw = rig.add(O['wrist'], rig.add(rig.add(rig.mul(wx, 6 * k), rig.mul(wy, 3 * k)), rig.mul(wz, 6 * k)))
    spike(O['wrist'], claw, 'fore' + s, 1.0 * k, 0.0, IVORY, 0.9)
    for i in range(5):
        r0 = 1.2 * k; rk = r0 * (1 - rig.KNUCKLE)
        spike(O['wrist'], O['knuckles'][i], fa[i], r0, rk, BONE_COL, 0.9)
        spike(O['knuckles'][i], O['tips'][i], fb[i], rk, 0.0, BONE_COL, 0.9)
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
from mathutils.bvhtree import BVHTree
BODY_BVH = BVHTree.FromObject(body, bpy.context.evaluated_depsgraph_get())
def snug(ring, tip, axis_a, axis_z):
    # A crystal on the body moved along its outward line until its base sits 1.5 inside the surface
    # (they were placed for the old tubes: on the new body some hovered off it)
    base = [sum(q_[c] for q_ in ring) / 4.0 for c in range(3)]
    q = rig.add(axis_a, rig.mul(axis_z, rig.dot(rig.sub(base, axis_a), axis_z)))
    d = rig.sub(base, q); b = math.sqrt(rig.dot(d, d))
    if b < 1e-3: return ring, tip
    d = rig.mul(d, 1.0 / b)
    hit = BODY_BVH.ray_cast(g2b(q), g2b(d) - g2b([0, 0, 0]), 200.0)
    if hit[0] is None: return ring, tip
    h = (hit[0] - g2b(q)).length
    sh = rig.mul(d, (h - 1.5) - b)
    return [rig.add(q_, sh) for q_ in ring], rig.add(tip, sh)
def place(crs, origin, x, y, z, sx, sy, sz, bone, onbody=False):
    # each crystal's five points shared (its corners light and dark in turn, the tip lit: the facets'
    # old light and dark as a gradient; split corners cost a vertex each)
    for ring, tip in crs:
        W_ = lambda p_: rig.add(origin, rig.add(rig.add(rig.mul(x, p_[0] * sx), rig.mul(y, p_[1] * sy)), rig.mul(z, p_[2] * sz)))
        wr, wt = [W_(q_) for q_ in ring], W_(tip)
        if onbody: wr, wt = snug(wr, wt, origin, z)
        def mkv(w):
            v = cry_bm.verts.new(g2b(w)); cvg[v] = bone; return v
        R_ = [mkv(q_) for q_ in wr]; T_ = mkv(wt)
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
FLANK_CR = []
for sd_ in (-1, 1): FLANK_CR += crystal_tris([sd_ * MESH * 0.62, MESH * 0.6, 7], [sd_ * MESH * 1.15, MESH * 1.02, 2], 2.2)     # the flank ridges (not under the wings: they pierced them)
TSEG_CR = crystal_tris([0, MESH * 0.85, 6], [0, MESH * 1.8, -2], 3.5)
for (a, b_, r, over, bone) in ((P['chest'], P['spine0'], 18.0, 1.2, 'chest'), (P['spine0'], P['spine1'], 17.5, 1.2, 'spine1'), (P['spine1'], P['hips'], 16.0, 1.25, 'hips')):
    place(SEG_CR, *seg_frame(a, b_, r, over), bone, True)
for i in range(rig.NNECK): place(SEG_CR + FLANK_CR, *seg_frame(P['neck%d' % i], P['neck%d' % (i + 1)], 11.0 - i * 1.2, 1.25), 'neck%d' % i, True)
for i in range(rig.NTAIL): place(SEG_CR + FLANK_CR if i < 2 else TSEG_CR, *seg_frame(P['tail%d' % i], P['tail%d' % (i + 1)], 14.5 - i * 1.8, 1.2), 'tail%d' % i, True)
TIP = crystal_tris([0, 0, 0], [0, 0, 16.0], 3.4)
for sd_ in (-1, 1): TIP += crystal_tris([0, 0, 3.0], [sd_ * 6.0, 1.0, 8.0], 1.6)
place(TIP, *seg_frame(P['tail%d' % rig.NTAIL], P['tail%d' % (rig.NTAIL + 1)], MESH, 16.0 / math.sqrt(rig.dot(rig.sub(P['tail%d' % (rig.NTAIL + 1)], P['tail%d' % rig.NTAIL]), rig.sub(P['tail%d' % (rig.NTAIL + 1)], P['tail%d' % rig.NTAIL])))), 'tail%d' % rig.NTAIL)
# (no crystals on the wings: the user's call)
cme = bpy.data.meshes.new("crystals"); cry_bm.to_mesh(cme)
cobj = bpy.data.objects.new("crystals", cme); sc.collection.objects.link(cobj)
idx = {v: i for i, v in enumerate(cry_bm.verts)}
for bn in sorted(set(cvg.values())):
    vg = cobj.vertex_groups.new(name=bn); vg.add([idx[v] for v, b in cvg.items() if b == bn], 1.0, 'REPLACE')
cry_bm.free()

# ---------------- materials (tiny3d reads fast64's data: written into the .glb below) ----------------
for o, names in ((body, ['sb_body']), (wobj, ['sb_body', 'sb_memb', 'sb_crystal', 'sb_bone']), (cobj, ['sb_body', 'sb_memb', 'sb_crystal'])):
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
def f3d_membrane():    # the old membrane: its texture times its baked colour, unlit, opaque, both sides
    m = f3d("membrane.i8.png", 32, 32, False); m["f3d_mat"]["rdp_settings"]["g_lighting"] = False
    return m
EXTRAS = {"sb_bone": f3d("scales.i8.png", 128, 32, True), "sb_crystal": f3d_crystal(), "sb_body": f3d("scales.i8.png", 128, 32, True), "sb_memb": f3d_membrane()}
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
