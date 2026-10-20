#!/usr/bin/env python3
"""
Builds new knight animations by splicing and retiming existing clips in a .glb,
then writes them back into the file (replacing clips with the same name).

  python3 tools/compose_anims.py assets/knight/knight.glb

Each clip is a list of segments: (source clip, t0, t1, duration). A segment plays
source time t0..t1 stretched to `duration` seconds (t0 == t1 holds a pose).
Consecutive segments are crossfaded over BLEND seconds. Output is sampled at
FPS with LINEAR interpolation. Needs numpy.
"""
import json, struct, sys
import numpy as np

FPS = 30
BLEND = 0.12

CLIPS = {
    # Crescent Cleave: drop into a low crouch, hold, then a big rising swing
    # (release at 0.8 s in character.c), then stand back up.
    "SkillCleave": [
        ("AttackCharged", 0.39, 1.16, 0.45),
        ("AttackCharged", 1.16, 1.36, 0.30),
        ("AttackCharged", 1.36, 1.94, 0.45),
        ("AttackCharged", 2.72, 2.91, 0.45),
    ],
    # Halfmoon Slash: settle into the charged stance and hold it (2 s charge),
    # then a wide horizontal sweep and a long recovery.
    "SkillHalfmoon": [
        ("AttackCharged", 0.39, 1.16, 0.50),
        ("AttackCharged", 1.16, 1.36, 1.45),
        ("Attack2", 0.33, 0.82, 0.40),
        ("Attack2End", 0.0, 0.83, 0.90),
    ],
}

CT = {5126: 'f', 5123: 'H', 5125: 'I', 5121: 'B', 5122: 'h'}
NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}

def load(path):
    d = open(path, 'rb').read()
    jl = struct.unpack('<I', d[12:16])[0]
    j = json.loads(d[20:20 + jl])
    o = 20 + jl
    bl = struct.unpack('<I', d[o:o + 4])[0]
    return j, bytearray(d[o + 8:o + 8 + bl])

def save(path, j, b):
    while len(b) % 4: b.append(0)
    j['buffers'][0]['byteLength'] = len(b)
    js = json.dumps(j, separators=(',', ':')).encode()
    while len(js) % 4: js += b' '
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(b))
    out += struct.pack('<II', len(js), 0x4E4F534A) + js
    out += struct.pack('<II', len(b), 0x004E4942) + bytes(b)
    open(path, 'wb').write(out)

def read_acc(j, b, i):
    a = j['accessors'][i]; bv = j['bufferViews'][a['bufferView']]
    n = NC[a['type']]
    off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    arr = np.frombuffer(bytes(b[off:off + a['count'] * n * 4]), dtype='<' + CT[a['componentType']])
    return arr.astype(np.float64).reshape(a['count'], n) if n > 1 else arr.astype(np.float64)

def add_acc(j, b, arr, typ):
    while len(b) % 4: b.append(0)
    data = np.ascontiguousarray(arr, dtype='<f4').tobytes()
    j['bufferViews'].append({'buffer': 0, 'byteOffset': len(b), 'byteLength': len(data)})
    b.extend(data)
    flat = np.asarray(arr, dtype=np.float64).reshape(len(arr), -1)
    j['accessors'].append({'bufferView': len(j['bufferViews']) - 1, 'componentType': 5126,
                           'count': len(arr), 'type': typ,
                           'min': flat.min(0).tolist(), 'max': flat.max(0).tolist()})
    return len(j['accessors']) - 1

def sampler_fn(times, vals, interp):
    def f(t):
        i = np.searchsorted(times, t, side='right') - 1
        i = max(0, min(i, len(vals) - 1))
        if interp == 'STEP' or i == len(vals) - 1:
            return vals[i]
        u = (t - times[i]) / max(1e-9, times[i + 1] - times[i])
        return mix(vals[i], vals[i + 1], u)
    return f

def mix(a, b, u):
    if len(a) == 4:     # quaternion: nlerp along the short arc
        if np.dot(a, b) < 0: b = -b
        q = a * (1 - u) + b * u
        return q / np.linalg.norm(q)
    return a * (1 - u) + b * u

def compose(j, b, segs):
    anims = {a['name']: a for a in j['animations']}
    # channel layout (node, path) taken from the first source clip
    first = anims[segs[0][0]]
    layout = [(c['target']['node'], c['target']['path']) for c in first['channels']]
    fns = {}
    for name in {s[0] for s in segs}:
        a = anims[name]
        for c in a['channels']:
            s = a['samplers'][c['sampler']]
            fns[(name, c['target']['node'], c['target']['path'])] = sampler_fn(
                read_acc(j, b, s['input']), read_acc(j, b, s['output']), s.get('interpolation', 'LINEAR'))
    total = sum(s[3] for s in segs)
    n = int(round(total * FPS)) + 1
    times = np.arange(n) / FPS
    starts = np.cumsum([0] + [s[3] for s in segs])
    out = {k: [] for k in layout}
    for t in times:
        k = min(np.searchsorted(starts, t, side='right') - 1, len(segs) - 1)
        def value(si, node, path, tt):
            src, t0, t1, dur = segs[si]
            u = min(1.0, max(0.0, (tt - starts[si]) / dur)) if dur > 0 else 0.0
            return fns[(src, node, path)](t0 + (t1 - t0) * u)
        for node, path in layout:
            v = value(k, node, path, t)
            # crossfade into this segment from the end of the previous one
            if k > 0 and t - starts[k] < BLEND:
                prev = value(k - 1, node, path, starts[k])
                v = mix(prev, v, (t - starts[k]) / BLEND)
            out[(node, path)].append(v)
    t_acc = add_acc(j, b, times, 'SCALAR')
    channels, samplers = [], []
    for node, path in layout:
        vals = np.array(out[(node, path)])
        v_acc = add_acc(j, b, vals, 'VEC4' if path == 'rotation' else 'VEC3')
        samplers.append({'input': t_acc, 'output': v_acc, 'interpolation': 'LINEAR'})
        channels.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': path}})
    return {'channels': channels, 'samplers': samplers}

def main(path):
    j, b = load(path)
    j['animations'] = [a for a in j['animations'] if a['name'] not in CLIPS]
    for name, segs in CLIPS.items():
        a = compose(j, b, segs)
        a['name'] = name
        j['animations'].append(a)
        print(f"{name}: {sum(s[3] for s in segs):.2f} s")
    save(path, j, b)

if __name__ == '__main__':
    main(sys.argv[1])
