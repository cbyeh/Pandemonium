"""Lists every sound each Bonus boss move plays, with a player for each, in one HTML page.

Reads the code, so it stays true as moves change: the sound table in guardian_scene.c (which
file a SCENE1_SFX_ id plays), then every function of the Bonus bosses, following their small
sound helpers (sfx_boom, swing_sfx...) and splitting the main update's switch by its cases.
The page embeds the .wav sources, so it opens anywhere:

  python3 tools/boss_sfx_map.py [out.html]      (default: build/boss_sfx_map.html)

To change a sound: replace the .wav it names (every move listed under that file changes), or
change the SCENE1_SFX_ id at the code line shown (only that move). hostile/ files are made by
tools/boss_sfx_levels.py from fx/ and boss/: swap the source there and rerun it.
"""
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
    """[(sfx id, line, via helper or None)] that `name` plays, through helpers (not other moves)"""
    res = []
    body = funcs[name][1]
    if len(body) == 1:      # a one-line helper: its body follows the signature
        body = [(body[0][0], body[0][1][body[0][1].index("{"):])]
    else:
        body = body[1:]
    for ln, text in body:
        code = text.split("//")[0]
        for sid in SFX.findall(code):
            res.append((sid, ln, None))
        for callee in CALL.findall(code):
            if callee in funcs and callee != name and callee not in seen and not callee.endswith("_update"):
                for sid, ln2, via in sounds_of(callee, funcs, seen + (name,)):
                    res.append((sid, ln, via or callee))
    return res


def moves(path):
    """[(move, description, [(sfx id, line, via)])] for one boss file"""
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
            for sid, ln, via in snd:
                if (sid, via) not in [(u[0], u[2]) for u in uniq]:
                    uniq.append((sid, ln, via))
            for sid, ln, via in uniq:
                used.setdefault(sid, []).append(f"{boss}: {move}")
            rows.append((move, desc, uniq))
        sections.append((boss, path, rows))

    clips = {}
    for sid in used:
        wav = table.get(sid)
        if wav and os.path.exists(os.path.join(ROOT, wav)):
            clips[sid] = base64.b64encode(open(os.path.join(ROOT, wav), "rb").read()).decode()

    def player(sid):
        wav = table.get(sid, "?")
        btn = f'<button onclick="play(\'{sid}\')">&#9654;</button>' if sid in clips else "(missing)"
        note = " <i>(made by boss_sfx_levels.py)</i>" if "/hostile/" in wav else ""
        return f'{btn} <b>{sid[11:].lower()}</b> <code>{html.escape(wav)}</code>{note}'

    h = ["<!doctype html><meta charset=utf-8><title>Bonus boss sounds</title><style>",
         "body{font:14px sans-serif;margin:2em;max-width:70em} td,th{padding:4px 8px;vertical-align:top;border-bottom:1px solid #ddd}",
         "table{border-collapse:collapse;width:100%} th{text-align:left;background:#f4f4f4} code{font-size:12px;color:#555}",
         "button{cursor:pointer} .d{color:#666;font-size:12px}</style>",
         "<h1>Bonus boss sounds by move</h1><p>Each move's sounds, in code order. Replace the <code>.wav</code> to change it ",
         "everywhere it's used (see <a href=#files>by file</a>), or change the <code>SCENE1_SFX_</code> id at the code line for that move only. ",
         "Volumes in game are the call's level &times; 0.85.</p>"]
    for boss, path, rows in sections:
        h.append(f"<h2>{boss}</h2><table><tr><th>Move</th><th>Sounds</th><th>Code</th></tr>")
        for move, desc, snd in rows:
            cell = "<br>".join(player(sid) + (f" <span class=d>via {via}()</span>" if via else "") for sid, _, via in snd)
            code = "<br>".join(f"<code>{os.path.basename(path)}:{ln}</code>" for _, ln, _ in snd)
            d = f"<div class=d>{html.escape(desc[:220])}</div>" if desc else ""
            label = "as a move starts (choose)" if move == "choose" else re.sub(r"_update\b", "", move)
            h.append(f"<tr><td><b>{html.escape(label)}</b>{d}</td><td>{cell}</td><td>{code}</td></tr>")
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
