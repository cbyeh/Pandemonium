#!/usr/bin/env python3
"""Small boss portraits (22x22: they fit the map's 22 px selection box; drawn at 24 and trimmed, full colour, clear around them) for the map's boss nodes and the Codex, pixelled by
hand from the bosses' in-game look (their colours sampled from the shots: docs/media/boss_*.png, the Tower captures
in /tmp/gameplay_caps): the Guardian's silver cross-slit helm in its red hood, gold blades behind; Guardian Unbound's
bone-gold helm and shoulder plates, its lightning; the Fallen Warrior's crimson great-helm with its dark iron horns
and the one pauldron on its left; the Shackled Sun's face in its flames and spikes, its chain; Shadowborne's head in
profile, jaws open, its pale blue crystals. 'o' / 'K' is each one's dark outline."""
import math

def grid(rows, pal):
    rows = [r for r in rows.strip('\n').split('\n')]
    assert len(rows) == 24 and all(len(r) == 24 for r in rows), [(i, len(r)) for i, r in enumerate(rows) if len(r) != 24] or len(rows)
    return [[pal[c] + (255,) if c != '.' else (0, 0, 0, 0) for c in r] for r in rows]

GUARDIAN = dict(W=(232, 234, 240), s=(172, 174, 184), d=(106, 109, 122), k=(14, 11, 13), R=(150, 32, 42), r=(88, 18, 26),
                G=(245, 205, 55), g=(170, 128, 28), o=(10, 7, 7))
GUARDIAN_ART = """
.oGo................oGo.
..oGo..............oGo..
...oGo....orrro...oGo...
....oGo.orRRRRro.oGo....
.....oGorRWWWsRRrGo.....
......orRWWWWWssRro.....
......oRWWWWWWsssRo.....
......oRWWWkWWsssRo.....
......oRWWWkWWssdRo.....
......oRWkkkkkkssRo.....
......oRWWWkWWssdRo.....
......oRsWWkWWsddRo.....
......oRsWWkWsdddRo.....
.....orRssWkWsdddRro....
.oo..orRdssksddddRro.oo.
oWWso.orRdsssdddRro.osdo
oWWWsoorrRddddddRrroosdo
oWWWWsorRRssssssRRrosddo
oWWWWWsoRsWWWsssRoosdddo
oWWWWWWoRsWWssssRosddddo
osWWWWWoRsWsssddRosddddo
.osWWWsoRRssssddRosdddo.
..ossso.rRRsssdRRr.oddo.
...ooo..orrRRRRrro..oo..
"""
UNBOUND = dict(W=(236, 214, 152), s=(192, 160, 95), d=(126, 100, 56), k=(12, 9, 5), R=(160, 34, 32), r=(95, 20, 18),
               G=(255, 236, 70), o=(14, 9, 6))
UNBOUND_ART = """
....G..............G....
....GG............GG....
.....G....oooo....G.....
.....GG..oWWWso..GG.....
......G.oWWWWsso.G......
......GGoWWWWWssoGG.....
oo.....GoWWkWWssoG...ooo
oWWoo...oWWkWWsso...oWso
oWWWWoo.oWkkkkkso.ooWsdo
.oWWWWWooWWkWsdsooWWssdo
..oWWWWWoWWkWsdoWWssdo..
...oWWWso.WkWsd.oWsdo...
....ooo..oskssdo..ooo...
........oRsksRRo........
.......orRRsRRRro.......
......orRRRRRRRRRro.....
.....oWWWWWsssssdo......
....oWWWWWWssssssdo.....
....oWWWsWWsssdssdo.....
....oWWWWsWssdsssdo.....
.....oWWWWsssssddo......
.....oRRRRRRRRRRRo......
....orRRrRRRRrRRRro.....
....orrro.orrro.orro....
"""
FALLEN = dict(C=(176, 44, 56), c=(124, 30, 40), x=(78, 17, 25), k=(12, 5, 6), I=(132, 128, 136), i=(92, 88, 95),
              j=(54, 51, 57), L=(96, 46, 36), o=(8, 4, 4))
# (rows 0-16 drawn as the left half and mirrored: the helm and its horns are symmetric)
FALLEN_ART = """
........................
........oooooooo........
.......oxcCCCCcxo.......
......oxcCCCCCCcxo......
...oooxcCCCCCCCCcxooo...
.ooIIIIxCCCCCCCCxIIIIoo.
oIIIiiixCCCCCCCCxiiiIIIo
oIIijjoxCCCCCCCCxojjiIIo
oIIjo.xkkkkkkkkkkx.ojIIo
oIIjo.xCCCCCCCCCCx.ojIIo
oIIjo.xcCCCCCCCCcx.ojIIo
.oIIjoxcCCCxxCCCcxojIIo.
.oIIIjoxcCCCCCCcxojIIIo.
..oIIIjoxcCCCCcxojIIIo..
...oIIIjoxccccxojIIIo...
....oIIIooxxxxooIIIo....
.....ooooocxxcooooo.....
....oocxxcCCCCcxoiiioo..
..ooCCCcxcCCCCcxoIIIiio.
.oCCCCCcxCCCCCCxiIIIIiio
oCCCCCcxcCCCCCCcxLLLLLLo
oCCCCcxxcCCCCCCcxoLLLLo.
oCCCcxxccCCCCCCccxoLLo..
oCCcxxcccCCCCCCcccxooo..
"""
SHADOW = dict(K=(10, 8, 12), B=(44, 41, 50), b=(78, 74, 88), h=(110, 108, 118), H=(170, 170, 178), M=(150, 32, 40),
              m=(92, 18, 26), T=(238, 232, 215), E=(255, 145, 30), X=(155, 198, 255), x=(96, 136, 215), Z=(220, 238, 255))
SHADOW_ART = """
........................
.............X..........
.........H..XZ...X......
........HH.XZx..XZ...X..
.......HhKXZxK.XZx..XZ..
....KKKhBKXxBKKXxBK.XxK.
..KKBBBBBBBBBBBBBBBBBBK.
.KBbbBBEEBBBBBBBBBBBBBBK
KBbBBBBBBBBBBBBBBBBBBBBK
KbBBBBBBBBBBBBBBBBBBBBBK
.KTKTKTKTMMBBBBBBBBBBBBK
..T.T.T.MMmMBBBBBBBBBBBK
....MMMMmmmmKBBBBBBBBBBK
...KMmmmmmmKBBBBBBBBBBBK
..KTMTMTMMKBBBBBBBBBBBBK
.KbBBBBBBBBBBBBBBBBBBBBK
KbBBBBBBBBKKBBBBBBBBBBBK
.KKKKKKKKK..KBBBBBBBBBBK
.............KBBBBBBBBBK
..............KBBBBBBBBK
...............KBBBBBBBK
...............KBBBBBBBK
................KBBBBBBK
................KBBBBBBK
"""

def trim(px): return [r[1:23] for r in px[1:23]]

def shadowborne22():
    """the 24 px head trimmed to 22, its outline where it meets the clear lit in its wings' purple-grey (the model's
    membrane colour), so the black head holds on the dark map (a wing behind was tried: no room above the neck at 22)"""
    px = trim(grid(SHADOW_ART, SHADOW))
    V = (128, 118, 160)     # the rim: its wing membrane's purple-grey
    rim = [[False] * 22 for _ in range(22)]
    for j in range(22):
        for i in range(22):
            p = px[j][i]
            if p[3] and p[:3] == SHADOW['K'] and any(not (0 <= j + v < 22 and 0 <= i + u < 22) or not px[j + v][i + u][3] for u, v in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                rim[j][i] = True
    for j in range(22):
        for i in range(22):
            if rim[j][i]: px[j][i] = V + (255,)
    return px

def sun():
    """the disc and its rings laid out by angle (12 gold spikes, orange wisps between, as the model), the face by hand"""
    Y, y, O, q, F, f, K = (252, 208, 72), (214, 150, 40), (246, 124, 42), (196, 74, 26), (250, 224, 184), (236, 176, 132), (70, 28, 10)
    N = 22
    px = [[(0, 0, 0, 0)] * N for _ in range(N)]
    cx = cy = 10.5
    for j in range(24):
        for i in range(24):
            dx, dy = i - cx, j - cy; r = math.hypot(dx, dy); a = math.atan2(dy, dx)
            sp = abs(((a / (2 * math.pi) * 12 + 0.5) % 1) - 0.5)        # 0 on a spike's line
            if r <= 6.7: px[j][i] = F + (255,) if r < 5.8 else f + (255,)
            elif r <= 7.6: px[j][i] = O + (255,)
            elif r <= 10.9 - sp * 13 and r <= 10.9: px[j][i] = (Y if sp < 0.12 else y) + (255,)
            elif r <= 8.9 and abs(sp - 0.5) < 0.16: px[j][i] = q + (255,)       # a wisp between two spikes
    for j in range(N):     # the outline: a dark pixel round the edge, where it meets the clear
        for i in range(N):
            if px[j][i][3]: continue
            if any(0 <= j + v < N and 0 <= i + u < N and px[j + v][i + u][3] and px[j + v][i + u][:3] != K for u, v in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                px[j][i] = K + (200,)
    face = {  # (x, y): colour, the face by hand: brows, eyes (white, brown iris), nose, cheeks, red lips
        'b': (150, 90, 50), 'w': (252, 246, 236), 'e': (150, 78, 26), 'n': (222, 160, 118), 'c': (240, 165, 140), 'L': (214, 58, 48)}
    FACE = ["......................"] * 7 + [
        ".......bb....bb.......",
        ".......we....ew.......",
        "......................",
        "..........n...........",
        ".......c..n...c.......",
        ".........LLL..........",
    ] + ["......................"] * 9
    for j, row in enumerate(FACE):
        for i, ch in enumerate(row):
            if ch != '.': px[j][i] = face[ch] + (255,)
    for i, j in ((18, 18), (19, 19), (20, 20), (21, 21), (19, 20), (21, 20)):     # the chain off its lower right
        px[j][i] = (120, 120, 126, 255) if (i + j) % 2 else (70, 70, 76, 255)
    return px

PORTRAITS = [   # (name, map/Codex use, pixels)
    ("Guardian", "of the Shackled Sun: each circle's first boss", trim(grid(GUARDIAN_ART, GUARDIAN))),
    ("Guardian Unbound", "each circle's last boss", trim(grid(UNBOUND_ART, UNBOUND))),
    ("Fallen Warrior", "a Bonus boss (Fights, Elites)", trim(grid(FALLEN_ART, FALLEN))),
    ("Shackled Sun", "a Bonus boss (Fights, Elites)", sun()),
    ("Shadowborne", "a Bonus boss (Fights, Elites)", shadowborne22()),
]
