"""Lists every sound each Bonus boss move plays, with a player for each, in one HTML page.

Reads the code, so it stays true as moves change: the sound table in guardian_scene.c (which
file a SCENE1_SFX_ id plays), then every function of the Bonus bosses, following their small
sound helpers (sfx_boom, swing_sfx...) and splitting the main update's switch by its cases.
The page embeds the .wav sources, so it opens anywhere:

  python3 tools/boss_sfx_map.py [out.html]      (default: build/boss_sfx_map.html)

Each sound also shows how loud it plays in game: the file's loudness as heard (EBU R128, the
loudest 400 ms, LUFS) plus its playback gain in dB (the call's volume x BONUS_SFX_VOL x the
distance falloff at a fight's 40 away, as the Guardian's; capped per channel), all read from the code.
Ones well over or under the Guardian's are marked.

To change a sound: replace the .wav it names (every move listed under that file changes), or
change the SCENE1_SFX_ id at the code line shown (only that move). hostile/ files are made by
tools/boss_sfx_levels.py from fx/ and boss/: swap the source there and rerun it.
"""
import array
import math
import wave
import base64
import html
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BOSSES = [
    ("The Shackled Sun", "src/game/sun_boss/sun_boss.c"),
    ("Shadowborne", "src/game/dragon_boss/dragon_boss.c"),
    ("Fallen Warrior", "src/game/fallen_knight/fallen_knight.c"),
]
FUNC = re.compile(r"^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w\s\*]*?\b([a-z_]\w*)\s*\(([^;{]*)\)\s*(?:\{.*)?$")
SFX = re.compile(r"SCENE1_SFX_\w+")
CALL = re.compile(r"\b([a-z_]\w*)\s*\(")
CASE = re.compile(r"^\s*case\s+(\w+)\s*:")
VOLCALL = re.compile(r"\bsfx\s*\(([^;]*?),\s*([0-9.]+)f?\s*\)")
GUARDIAN = ["src/game/boss/boss_sfx.c", "src/game/boss/boss_attacks.c"]     # his (and Unbound's) sounds
FIGHT_RANGE = 40.0


def constant(path, name):
    m = re.search(name + r"\s*=?\s*([0-9.]+)f", open(os.path.join(ROOT, path)).read())
    return float(m.group(1))


def biquad(x, b, a):
    y, x1, x2, y1, y2 = [], 0.0, 0.0, 0.0, 0.0
    for v in x:
        o = b[0] * v + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y.append(o)
    return y


def loudness(path):
    """EBU R128 momentary loudness, the loudest 400 ms (LUFS): as tools/boss_sfx_levels.py"""
    w = wave.open(path)
    sr, s = w.getframerate(), array.array("h", w.readframes(w.getnframes()))
    x = [v / 32768.0 for v in s] + [0.0] * int(sr * 0.4)
    k = math.tan(math.pi * 1681.974450955533 / sr)
    q, vh = 0.7071752369554196, 10.0 ** (3.999843853973347 / 20.0)
    vb, a0 = vh ** 0.4996667741545416, 1.0 + k / q + k * k
    x = biquad(x, [(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0],
               [1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0])
    k, q = math.tan(math.pi * 38.13547087602444 / sr), 0.5003270373238773
    a0 = 1.0 + k / q + k * k
    x = biquad(x, [1.0, -2.0, 1.0], [1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0])
    n, acc, best = int(sr * 0.4), 0.0, 0.0
    for i, v in enumerate(x):
        acc += v * v
        if i >= n:
            acc -= x[i - n] * x[i - n]
        best = max(best, acc)
    return -0.691 + 10.0 * math.log10(best / n + 1e-12)


def sound_table():
    """SCENE1_SFX_* -> assets/... .wav, from guardian_scene.c"""
    src = open(os.path.join(ROOT, "src/scenes/guardian/guardian_scene.c")).read()
    return {m.group(1): "assets/audio/" + m.group(2) + ".wav"
            for m in re.finditer(r"\[(SCENE1_SFX_\w+)\]\s*=\s*\"rom:/audio/([^\"]+)\.wav64\"", src)}


def functions(path):
    """name -> (first line, body lines [(lineno, text)], leading comment)"""
    lines = open(os.path.join(ROOT, path)).read().split("\n")
    out, i = {}, 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if not m or lines[i].startswith((" ", "\t", "#")):
            i += 1
            continue
        name, start = m.group(1), i
        j = i - 1
        comment = []
        while j >= 0 and lines[j].strip().startswith(("//", "/*", "*")):
            comment.insert(0, lines[j].strip().lstrip("/*").strip())
            j -= 1
        depth, body = 0, []
        while i < len(lines):
            depth += lines[i].count("{") - lines[i].count("}")
            body.append((i + 1, lines[i]))
            i += 1
            if depth <= 0 and "{" in "".join(t for _, t in body):
                break
        out[name] = (start + 1, body, " ".join(c for c in comment if c))
    return out


def sounds_of(name, funcs, seen=()):
    """[(sfx id, line, via helper or None, volume or None)] that `name` plays, through helpers
    (not other moves)"""
    res = []
    body = funcs[name][1]
    if len(body) == 1:      # a one-line helper: its body follows the signature
        body = [(body[0][0], body[0][1][body[0][1].index("{"):])]
    else:
        body = body[1:]
    for ln, text in body:
        code = text.split("//")[0]
        vols = {sid: float(m.group(2)) for m in VOLCALL.finditer(code) for sid in SFX.findall(m.group(1))}
        for sid in SFX.findall(code):
            res.append((sid, ln, None, vols.get(sid)))
        for callee in CALL.findall(code):
            if callee in funcs and callee != name and callee not in seen and not callee.endswith("_update"):
                for sid, ln2, via, vol in sounds_of(callee, funcs, seen + (name,)):
                    res.append((sid, ln, via or callee, vol))
    return res


def moves(path):
    """[(move, description, [(sfx id, line, via, volume)])] for one boss file"""
    funcs, out = functions(path), []
    base = os.path.basename(path)[:-2]
    for name, (start, body, comment) in funcs.items():
        if "sfx" in name:
            continue        # the helpers themselves: they show under the moves that use them
        if name.endswith("_update") and name.startswith(base.split("_")[0]) and any(CASE.match(t) for _, t in body):
            # the main update: each case of its switch is a move (its own sounds, not the
            # *_update functions it hands on to, which are listed by themselves)
            label, seg = "(before the switch)", []
            for ln, text in body + [(0, "case END:")]:
                c = CASE.match(text)
                if c:
                    if seg:
                        sub = {name: (0, [(0, "")] + seg, "")}
                        sub.update({k: v for k, v in funcs.items() if k != name})
                        s = sounds_of(name, sub)
                        if s:
                            out.append((f"{name} / {label}", "", s))
                    label, seg = c.group(1), []
                else:
                    seg.append((ln, text))
            continue
        s = sounds_of(name, funcs)
        if s:
            out.append((name, comment, s))
    return out


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "boss_sfx_map.html")
    table = sound_table()
    used, sections = {}, []
    for boss, path in BOSSES:
        rows = []
        for move, desc, snd in moves(path):
            uniq = []
            for sid, ln, via, vol in snd:
                if (sid, via) not in [(u[0], u[2]) for u in uniq]:
                    uniq.append((sid, ln, via, vol))
            for sid, ln, via, vol in uniq:
                used.setdefault(sid, []).append(f"{boss}: {move}")
            rows.append((move, desc, uniq))
        sections.append((boss, path, rows))

    # How loud each plays: file loudness + playback gain (dB), with the Guardian as the reference
    ac = "src/controllers/audio_controller.c"
    bonus_vol = constant("src/scenes/guardian/guardian_scene_sfx.h", "BONUS_SFX_VOL")
    near, cap = constant(ac, "sfxNearGain"), constant(ac, "sfxMaxChannelVol")
    d0, d1 = constant(ac, "sfxMinDist"), constant(ac, "sfxMaxDist")
    g_range = near * (1.0 - min(max((FIGHT_RANGE - d0) / (d1 - d0), 0.0), 1.0))
    file_lufs = {}

    def lufs(sid):
        if sid not in file_lufs:
            wav = table.get(sid)
            file_lufs[sid] = loudness(os.path.join(ROOT, wav)) if wav and os.path.exists(os.path.join(ROOT, wav)) else None
        return file_lufs[sid]

    def in_game(sid, gain):
        f = lufs(sid)
        return None if f is None or gain <= 0 else f + 20.0 * math.log10(min(gain, cap))

    g_ids = sorted({sid for p in GUARDIAN for sid in SFX.findall(open(os.path.join(ROOT, p)).read())
                    if "SFX_BOSS_" in sid and sid in table})
    g_levels = {sid: in_game(sid, g_range) for sid in g_ids}
    ref = sorted(v for v in g_levels.values() if v is not None)
    g_mid = ref[len(ref) // 2]

    def level(sid, vol):
        v = in_game(sid, (vol if vol is not None else 1.0) * bonus_vol * g_range)     # with the Guardian's falloff, at the same 40
        if v is None:
            return ""
        d = v - g_mid
        cls = "hot" if d > 3.0 else "low" if d < -6.0 else "ok"
        tag = f" ({d:+.0f} dB vs the Guardian)" if cls != "ok" else ""
        q = "" if vol is not None else "?"
        return f' <span class={cls}>{v:.1f} LUFS{q}{tag}</span>'

    clips = {}
    for sid in list(used) + g_ids:
        wav = table.get(sid)
        if wav and os.path.exists(os.path.join(ROOT, wav)):
            clips[sid] = base64.b64encode(open(os.path.join(ROOT, wav), "rb").read()).decode()

    def player(sid, vol=None, guardian=False):
        wav = table.get(sid, "?")
        btn = f'<button onclick="play(\'{sid}\')">&#9654;</button>' if sid in clips else "(missing)"
        note = " <i>(made by boss_sfx_levels.py)</i>" if "/hostile/" in wav else ""
        lv = (f' <span class=ok>{g_levels[sid]:.1f} LUFS</span>' if g_levels.get(sid) is not None else "") if guardian else level(sid, vol)
        return f'{btn} <b>{sid[11:].lower()}</b>{lv} <code>{html.escape(wav)}</code>{note}'

    h = ["<!doctype html><meta charset=utf-8><title>Bonus boss sounds</title><style>",
         "body{font:14px sans-serif;margin:2em;max-width:70em} td,th{padding:4px 8px;vertical-align:top;border-bottom:1px solid #ddd}",
         "table{border-collapse:collapse;width:100%} th{text-align:left;background:#f4f4f4} code{font-size:12px;color:#555}",
         "button{cursor:pointer} .d{color:#666;font-size:12px} .ok{color:#2a7} .hot{color:#c22;font-weight:bold} .low{color:#888}</style>",
         "<h1>Bonus boss sounds by move</h1><p>Each move's sounds, in code order. Replace the <code>.wav</code> to change it ",
         "everywhere it's used (see <a href=#files>by file</a>), or change the <code>SCENE1_SFX_</code> id at the code line for that move only. ",
         f"Levels are how loud each plays in game (the file's loudness, LUFS, plus its playback gain). The Guardian's attack sounds play at "
         f"{ref[0]:.1f} to {ref[-1]:.1f} at a fight's {FIGHT_RANGE:.0f} away, <b>{g_mid:.1f}</b> in the middle (<a href=#guardian>his sounds</a>); "
         "<span class=hot>red</span> is 3 dB or more over him, <span class=low>grey</span> 6 dB or more under. "
         "A <code>?</code> means the volume isn't a plain number in the code (taken as 1).</p>"]
    for boss, path, rows in sections:
        h.append(f"<h2>{boss}</h2><table><tr><th>Move</th><th>Sounds</th><th>Code</th></tr>")
        for move, desc, snd in rows:
            cell = "<br>".join(player(sid, vol) + (f" <span class=d>via {via}()</span>" if via else "") for sid, _, via, vol in snd)
            code = "<br>".join(f"<code>{os.path.basename(path)}:{ln}</code>" for _, ln, _, _ in snd)
            d = f"<div class=d>{html.escape(desc[:220])}</div>" if desc else ""
            label = "as a move starts (choose)" if move == "choose" else re.sub(r"_update\b", "", move)
            h.append(f"<tr><td><b>{html.escape(label)}</b>{d}</td><td>{cell}</td><td>{code}</td></tr>")
        h.append("</table>")
    h.append("<h2 id=guardian>The Guardian and Guardian Unbound (the reference)</h2><p class=d>Their sounds, at a fight's "
             f"{FIGHT_RANGE:.0f} away (they fall off with distance: {near:.2f} close, {g_range:.2f} at {FIGHT_RANGE:.0f}). Unbound plays the same ones.</p><table>")
    for sid in g_ids:
        h.append(f"<tr><td>{player(sid, guardian=True)}</td></tr>")
    h.append("</table>")
    h.append("<h2 id=files>By file</h2><table><tr><th>Sound</th><th>Used by</th></tr>")
    for sid in sorted(used, key=lambda k: table.get(k, k)):
        h.append(f"<tr><td>{player(sid)}</td><td class=d>{html.escape(', '.join(dict.fromkeys(used[sid])))}</td></tr>")
    h.append("</table><script>const C={" + ",".join(f'"{k}":"{v}"' for k, v in clips.items()) + "};let a;")
    h.append("function play(k){if(a)a.pause();a=new Audio('data:audio/wav;base64,'+C[k]);a.play();}</script>")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    open(out_path, "w").write("\n".join(h))
    print(f"{out_path}: {sum(len(r) for _, _, r in sections)} moves, {len(clips)} sounds")


if __name__ == "__main__":
    main()
