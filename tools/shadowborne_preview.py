# Renders Shadowborne's skinned body (tools/shadowborne_model.py's .blend) posed as the game poses it:
# each bone's frame rebuilt from rig points (the rest pose, the wings open, a paw raised)
#   Blender -b /tmp/sbmodel/shadowborne_model.blend --python tools/shadowborne_preview.py
import bpy, math, os, sys
from mathutils import Vector, Matrix
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shadowborne_rig as rig
def g2b(p): return Vector((p[0], -p[2], p[1]))
sc = bpy.context.scene; arm = bpy.data.objects["shadowborne"]; body = bpy.data.objects["shadowborne_body"]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src/game/dragon_boss/shadowborne_bind.h")).read()
     .split("static const SbBind SB_BIND[SB_BONES] = {")[1].split("};")[0]
     .replace("{", "(").replace("}", ")").replace("f,", ",").replace("f )", " )").replace('"', "'").join(["BIND = [", "]"]))
BIND = [(b[0], b[1], b[2], b[3], b[4]) for b in BIND]
def frame(h, t, ref):
    y = (g2b(t) - g2b(h)).normalized(); x = g2b(ref).cross(y).normalized(); z = x.cross(y)
    return Matrix(((x[0], y[0], z[0]), (x[1], y[1], z[1]), (x[2], y[2], z[2])))
def pose(targets):
    """targets: bone -> (head, tail, ref) in game units; others keep their bind frame relative to the root"""
    for pb in arm.pose.bones: pb.matrix_basis = Matrix()
    bpy.context.view_layer.update()
    for name, h, t, ref, rule in BIND:
        if name not in targets: continue
        pb = arm.pose.bones[name]; RB = pb.bone.matrix_local.to_3x3()
        H, T, Rf = targets[name]
        Rw = frame(H, T, Rf) @ frame(h, t, ref).transposed() @ RB
        M = Rw.to_4x4(); M.translation = g2b(H)
        pb.matrix = M; bpy.context.view_layer.update()
def rig_targets(P, wings):
    T = {}
    T['chest'] = (P['chest'], P['spine0'], [0, 1, 0]); T['spine1'] = (P['spine0'], P['spine1'], [0, 1, 0]); T['hips'] = (P['spine1'], P['hips'], [0, 1, 0])
    for i in range(rig.NNECK): T['neck%d' % i] = (P['neck%d' % i], P['neck%d' % (i + 1)], [0, 1, 0])
    for i in range(rig.NTAIL + 1): T['tail%d' % i] = (P['tail%d' % i], P['tail%d' % (i + 1)], [0, 1, 0])
    for k in range(4):
        T['upper%d' % k] = (P['shoulder%d' % k], P['knee%d' % k], [0, 0, 1]); T['lower%d' % k] = (P['knee%d' % k], P['foot%d' % k], [0, 0, 1])
    for sd, s in ((-1, 'L'), (1, 'R')):
        W = wings[sd]
        T['arm' + s] = (W['root'], W['elbow'], W['up']); T['fore' + s] = (W['elbow'], W['wrist'], W['up'])
        for i in range(5):
            T['fing%sa%d' % (s, i)] = (W['wrist'], W['knuckles'][i], W['up']); T['fing%sb%d' % (s, i)] = (W['knuckles'][i], W['tips'][i], W['up'])
    return T
# preview look: vertex colours, a warm key
sc.render.engine = 'BLENDER_WORKBENCH'; sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'VERTEX'
sc.display.shading.show_backface_culling = True
sc.render.resolution_x, sc.render.resolution_y = 800, 500
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam")); sc.collection.objects.link(cam); sc.camera = cam; cam.data.lens = 35
def shoot(name, loc, look=(0, 30, -20)):
    cam.location = g2b(loc); d = g2b(look) - cam.location; cam.rotation_mode = 'QUATERNION'; cam.rotation_quaternion = d.to_track_quat('-Z', 'Y')
    sc.render.filepath = "/tmp/sbmodel/%s.png" % name; bpy.ops.render.render(write_still=True)
P = rig.rest()
poses = {
    'rest': ({sd: rig.wing_world(P, sd, **rig.REST_WING) for sd in (-1, 1)}, None),
    'open': ({sd: rig.wing_world(P, sd, 0.0, 0.55, 0.0) for sd in (-1, 1)}, None),
    'half': ({sd: rig.wing_world(P, sd, 0.5, 0.4, 0.0) for sd in (-1, 1)}, None),
    'flap': ({sd: rig.wing_world(P, sd, 0.15, 0.9, 0.0, 0.0, [0.5, 0.45, 0.4, 0.35, 0.3]) for sd in (-1, 1)}, None),
}
for nm, (wings, _) in poses.items():
    pose(rig_targets(P, wings))
    shoot(nm + "_34", (210, 120, 230)); shoot(nm + "_side", (330, 60, -20)); shoot(nm + "_top", (0, 330, -30))
# a front paw raised (the stomp): shoulder up, the foot lifted forward
Q = rig.rest(); Q['shoulder1'] = rig.add(Q['shoulder1'], [0, 6, 0]); Q['foot1'] = rig.add(Q['shoulder1'], [-4, 12, 20])
Q['knee1'] = rig.knee(Q['shoulder1'], Q['foot1'], [0.3, -0.4, -1.0])
pose(rig_targets(Q, {sd: rig.wing_world(Q, sd, **rig.REST_WING) for sd in (-1, 1)}))
shoot("paw_34", (210, 100, 230))
