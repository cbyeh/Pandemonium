"""Poses the knight's rig by hand (IK legs and arms, aimed feet, hands and sword) for
held poses that tools/compose_anims.py splices into clips ("pose:NAME" sources).

  Blender -b -P tools/pose_blender.py -- assets/knight/knight.glb /tmp/out tools/poses/NAME.json "$(cat tools/poses/NAME_params.json)"

writes /tmp/out_0..3.png (side, front, three-quarter, front-left) and the pose's bone
transforms to tools/poses/NAME.json. Armature space: the knight faces -X, Z up; the
params place the hips, the ankles and knee poles, the hands and elbow poles, and aim the
feet and the sword (its blade runs along the hand's local -X)."""
import bpy, sys, math, json, os, struct, tempfile
from mathutils import Vector, Matrix, Quaternion
argv = sys.argv[sys.argv.index('--')+1:]
glb, out_png, out_json, P = argv[0], argv[1], argv[2], json.loads(argv[3])
bpy.ops.wm.read_factory_settings(use_empty=True); sc = bpy.context.scene; sc.render.fps = 30
bpy.ops.import_scene.gltf(filepath=glb)
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
idle = bpy.data.actions['Idle']; arm.animation_data.action = idle
sc.frame_set(int(idle.frame_range[0])); bpy.context.view_layer.update()
arm.animation_data.action = None
pb = arm.pose.bones
def upd(): bpy.context.view_layer.update()
def clear(b):
    for c in list(b.constraints): b.constraints.remove(c)
def rot_arm(name, axis, deg):     # rotate a bone about an armature-space axis through its head
    b = pb[name]; m = b.matrix.copy(); h = m.translation.copy()
    r = Matrix.Rotation(math.radians(deg), 4, axis)
    m = Matrix.Translation(h) @ r @ Matrix.Translation(-h) @ m; b.matrix = m; upd()
def empty(name, loc):
    e = bpy.data.objects.new(name, None); sc.collection.objects.link(e); e.location = loc; return e
# hips down (and turned/tilted)
w = pb['Waist']; m = w.matrix.copy(); m.translation.z = P.get('hip_z', m.translation.z); m.translation.x += P.get('hip_dx', 0); w.matrix = m; upd()
rot_arm('Waist', 'Y', P.get('hip_tilt', 0))
# legs by IK: ankle targets and knee poles
def leg_ik(side, ankle, pole):
    c = pb['LowerLeg-' + side].constraints.new('IK'); c.target = empty('t' + side, ankle); c.pole_target = empty('p' + side, pole)
    c.chain_count = 2; c.pole_angle = math.radians(P.get('pole_angle_' + side[0], 0))
if 'ankle_L' in P: leg_ik('Left', P['ankle_L'], P['pole_L']); leg_ik('Right', P['ankle_R'], P['pole_R'])     # else the standing legs
upd()
bpy.context.view_layer.objects.active = arm; arm.select_set(True)
bpy.ops.object.mode_set(mode='POSE'); bpy.ops.pose.select_all(action='SELECT'); bpy.ops.pose.visual_transform_apply()
for s in ('Left', 'Right'): clear(pb['LowerLeg-' + s])
upd()
def aim(name, d, roll_up=None):    # point a bone along d (armature space), keeping its head
    b = pb[name]; m = b.matrix.copy(); h = m.translation.copy()
    cur = (m.to_3x3() @ Vector((0, 1, 0))).normalized(); d = Vector(d).normalized()
    q = cur.rotation_difference(d); r = q.to_matrix().to_4x4()
    b.matrix = Matrix.Translation(h) @ r @ Matrix.Translation(-h) @ m; upd()
if 'foot_L' in P: aim('Foot-Left', P['foot_L']); aim('Foot-Right', P['foot_R'])
rot_arm('Spine1', 'Y', P.get('spine', 0)); rot_arm('Spine1', 'X', P.get('spine_side', 0)); rot_arm('Head', 'Y', P.get('head', 0))
if 'head_turn' in P: rot_arm('Head', 'Z', P['head_turn'])
if 'head_side' in P: rot_arm('Head', 'X', P['head_side'])     # level it again after the body's lean
# arms by IK to hand targets
def arm_ik(side, hand, pole):
    c = pb['LowerArm-' + side].constraints.new('IK'); c.target = empty('h' + side, hand); c.pole_target = empty('q' + side, pole)
    c.chain_count = 2; c.pole_angle = math.radians(P.get('arm_pole_' + side[0], 0))
if P.get('arm_R') != 'keep': arm_ik('Right', P['hand_R'], P['apole_R'])     # 'keep': the stance's sword arm, the blade on the shoulder
if P.get('arm_L') == 'mirror':      # the right arm's pose flipped (its hand mirror_dz higher)
    S = Matrix.Diagonal((1, -1, 1, 1))
    dz = Vector((0, 0, P.get('mirror_dz', 0)))
    arm_ik('Left', S @ Vector(P['hand_R']) + dz, S @ Vector(P['apole_R']) + dz)
else:
    arm_ik('Left', P['hand_L'], P['apole_L'])
upd()
bpy.ops.pose.visual_transform_apply()
for s in ('Left', 'Right'): clear(pb['LowerArm-' + s])
upd()
def aim_axis(name, local, d, local2=None, d2=None):   # turn a bone so its local axis points along d (and local2 toward d2)
    b = pb[name]; m = b.matrix.copy(); h = m.translation.copy(); r3 = m.to_3x3()
    cur = (r3 @ Vector(local)).normalized(); d = Vector(d).normalized()
    q = cur.rotation_difference(d)
    if local2 is not None:
        c2 = q @ (r3 @ Vector(local2)); c2 = (c2 - d * c2.dot(d)).normalized(); t2 = Vector(d2); t2 = (t2 - d * t2.dot(d)).normalized()
        q = c2.rotation_difference(t2) @ q
    b.matrix = Matrix.Translation(h) @ q.to_matrix().to_4x4() @ Matrix.Translation(-h) @ m; upd()
if 'sword_turn' in P:     # the kept sword arm's hand turned a little (armature axis, degrees): the blade off the helmet
    rot_arm('Hand-Right', P['sword_turn'][0], P['sword_turn'][1])
if P.get('arm_R') != 'keep': aim_axis('Hand-Right', (-0.983, 0.13, -0.129), P['sword'], (0, 1, 0), P.get('sword_up', (0, 0, 1)))
if P.get('arm_L') == 'mirror':      # its turn at the wrist mirrored too
    S = Matrix.Diagonal((1, -1, 1, 1))
    m = S @ pb['Hand-Right'].matrix @ S
    m.translation = pb['Hand-Left'].matrix.translation
    pb['Hand-Left'].matrix = m; upd()
else:
    aim_axis('Hand-Left', (0, 1, 0), P['hand_L_dir'], (1, 0, 0), P.get('hand_L_side', (0, 0, 1)))
me = bpy.data.objects['Knight']; g = me.vertex_groups['Hand-Right'].index
dg = bpy.context.evaluated_depsgraph_get(); ev = me.evaluated_get(dg); mm = ev.to_mesh()
pts = [me.matrix_world @ v.co for v, v0 in zip(mm.vertices, me.data.vertices) if any(x.group == g and x.weight > 0.5 for x in v0.groups)]
print('sword lowest z', round(min(p.z for p in pts), 2), 'tip', tuple(round(x, 2) for x in max(pts, key=lambda p: (p - pb['Hand-Right'].head).length)))
gh = me.vertex_groups['Head'].index
hv = [me.matrix_world @ v.co for v, v0 in zip(mm.vertices, me.data.vertices) if any(x.group == gh and x.weight > 0.3 for x in v0.groups)]
bv = [p for p in pts if (p - pb['Hand-Right'].head).length > 1.2]       # the blade, not the hand round its grip
bh = pb['Hand-Right'].head; bt = max(pts, key=lambda p: (p - bh).length); ax = (bt - bh)
def segd(q): u = max(0.0, min(1.0, (q - bh).dot(ax) / ax.length_squared)); return (q - (bh + ax * u)).length
pm = max(pts, key=lambda p: (p - bt).length)     # the pommel's end: with the tip, the sword's axis
print('pommel', tuple(round(x, 2) for x in pm), 'tip', tuple(round(x, 2) for x in bt))
print('blade to helmet', round(min(segd(q) for q in hv), 2), '(from its axis; the blade is ~0.2 thick)')
for gname in ['Foot-Left', 'Foot-Right', 'LowerLeg-Left', 'LowerLeg-Right', 'Hand-Left']:
    gi = me.vertex_groups[gname].index
    zs = [(me.matrix_world @ v.co).z for v, v0 in zip(mm.vertices, me.data.vertices) if any(x.group == gi and x.weight > 0.3 for x in v0.groups)]
    print('lowest', gname, round(min(zs), 2))
ev.to_mesh_clear()
kn = pb['LowerLeg-Right'].head; el = pb['LowerArm-Right'].head; wr = pb['Hand-Right'].head
seg = wr - el; u = max(0.0, min(1.0, (kn - el).dot(seg) / seg.length_squared)); pt = el + seg * u
print('forearm over the front knee: at', round(u, 2), 'of it,', round((pt - kn).length, 2), 'from the joint, height above', round(pt.z - kn.z, 2))
for side in ('Left', 'Right'):
    if 'hand_' + side[0] not in P or P.get('arm_R') == 'keep' and side == 'Right' or P.get('arm_L') == 'mirror' and side == 'Left': continue
    print('reach', side, 'hand off by', round((pb['Hand-' + side].head - Vector(P['hand_' + side[0]])).length, 2),
          'shoulder', tuple(round(x, 2) for x in pb['UpperArm-' + side].head), 'elbow', tuple(round(x, 2) for x in pb['LowerArm-' + side].head))
if 'sword_roll' in P: 
    b = pb['Hand-Right']; d = (b.matrix.to_3x3() @ Vector((0,1,0))).normalized(); h = b.matrix.translation.copy()
    b.matrix = Matrix.Translation(h) @ Matrix.Rotation(math.radians(P['sword_roll']), 4, d) @ Matrix.Translation(-h) @ b.matrix; upd()
bpy.ops.object.mode_set(mode='OBJECT')
for b in ['Waist','UpperLeg-Left','LowerLeg-Left','Foot-Left','UpperLeg-Right','LowerLeg-Right','Foot-Right','Hand-Right','Head']:
    p = pb[b]; print(f'{b:15s} head {tuple(round(v,2) for v in p.head)} tail {tuple(round(v,2) for v in p.tail)}')
# key the pose as an action
act = bpy.data.actions.new('KneelPose'); arm.animation_data.action = act
for p in pb:
    for f in (1, 2):
        p.keyframe_insert('location', frame=f); p.keyframe_insert('rotation_quaternion', frame=f); p.keyframe_insert('scale', frame=f)
sc.frame_set(1)
# renders: side, front, three-quarter
sc.render.engine = 'BLENDER_WORKBENCH'; sc.render.resolution_x = 560; sc.render.resolution_y = 560
sh = sc.display.shading; sh.light = 'STUDIO'; sh.show_cavity = True; sh.show_shadows = True; sh.show_object_outline = True
sh.color_type = 'SINGLE'; sh.single_color = (0.75, 0.72, 0.68)
bpy.ops.mesh.primitive_plane_add(size=30, location=(0, 0, 0))     # the floor
fl = bpy.context.active_object; mat = bpy.data.materials.new('floor'); mat.diffuse_color = (0.35, 0.12, 0.12, 1); fl.data.materials.append(mat)
sh.color_type = 'MATERIAL'
for o in bpy.data.objects:
    if o.type == 'MESH' and o is not fl:
        m2 = bpy.data.materials.new('k'); m2.diffuse_color = (0.8, 0.78, 0.72, 1); o.data.materials.clear(); o.data.materials.append(m2)
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = P.get('scale', 15)
for k, yaw in enumerate(P.get('views', (0, 270, 240, 300))):
    r = math.radians(yaw); ca = P.get('cam_at', (0, 0, 0))     # the view's centre (a close-up: cam_at and a small scale)
    cam.location = (ca[0] + 42*math.sin(r), ca[1] - 42*math.cos(r), ca[2] + 3.5 + 42*math.tan(math.radians(12))); cam.rotation_euler = (math.radians(78), 0, r)
    sc.render.filepath = f'{out_png}_{k}.png'; bpy.ops.render.render(write_still=True)
# the pose's bone transforms, as glTF writes them (the knight.glb nodes' local TRS)
for a in list(bpy.data.actions):
    if a.name != 'KneelPose': bpy.data.actions.remove(a)
tmp = tempfile.mktemp(suffix='.glb')
bpy.ops.export_scene.gltf(filepath=tmp, export_format='GLB', export_animations=True, export_animation_mode='ACTIONS', export_force_sampling=True)
d = open(tmp, 'rb').read(); os.remove(tmp)
jl = struct.unpack('<I', d[12:16])[0]; j = json.loads(d[20:20 + jl]); bin_ = d[20 + jl + 8:]
def first(i):
    a = j['accessors'][i]; v = j['bufferViews'][a['bufferView']]; n = {'VEC3': 3, 'VEC4': 4}[a['type']]
    o = v.get('byteOffset', 0) + a.get('byteOffset', 0)
    return [round(x, 6) for x in struct.unpack('<%df' % n, bin_[o:o + 4 * n])]
pose = {}
an = j['animations'][0]
for c in an['channels']:
    pose.setdefault(j['nodes'][c['target']['node']]['name'], {})[c['target']['path']] = first(an['samplers'][c['sampler']]['output'])
json.dump(pose, open(out_json, 'w'), indent=1, sort_keys=True)
print('wrote', out_json, len(pose), 'bones')
