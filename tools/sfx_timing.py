"""How well the sounds land on what's seen: each screen shake (an impact) against the sound heard
nearest it, from a test log built with HLE_LOG_SFX (it logs [sfx] and [shake] with their times).

A sound is heard at its attack: its start plus its file's first 10 ms within 6 dB of its loudest
(read from the files the scene table names; for a rumble the loudest comes late, the hit doesn't). For each shake, the nearest peak within 0.3 s; per sound, the median of
(peak - shake) and the worst. Shakes with no sound within 0.12 s are listed: an impact unheard.

  python tools/sfx_timing.py out/log.txt [more logs...]
"""
import os
import re
import sys
import wave

import numpy as np

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def sound_table():
    """index -> (name, path) from the enum (comments stripped) and guardian_scene.c's paths."""
    enum = open(os.path.join(ROOT, "src/scenes/guardian/guardian_scene_sfx.h")).read()
    enum = re.sub(r"//.*", "", enum[enum.index("enum"):enum.index("SCENE1_SFX_COUNT")])
    names = re.findall(r"SCENE1_SFX_[A-Z0-9_]+", enum)
    paths = dict(re.findall(r"\[(SCENE1_SFX_[A-Z0-9_]+)\]\s*=\s*\"rom:/([^\"]+)\.wav64\"",
                            open(os.path.join(ROOT, "src/scenes/guardian/guardian_scene.c")).read()))
    return {i: (n[len("SCENE1_SFX_"):], paths.get(n)) for i, n in enumerate(names)}


def peak_ms(path):
    w = wave.open(os.path.join(ROOT, "assets", path + ".wav"))
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, w.getnchannels()).astype(float).mean(1)
    hop = w.getframerate() // 100; n = len(x) // hop
    e = 10 * np.log10((x[:n * hop].reshape(n, hop) ** 2).mean(1) + 1e-9)
    return int(np.argmax(e > e.max() - 6)) * 10


def main():
    table = sound_table()
    peaks = {i: peak_ms(p) for i, (_, p) in table.items() if p}
    by_sound, unheard, shakes_n = {}, [], 0
    for log in sys.argv[1:]:
        sounds, shakes, moves = [], [], []
        for line in open(log, errors="replace"):
            m = re.search(r"\[(move|sun|fk|dragon)\] (.*)", line)
            if m and sounds: moves.append((sounds[-1][0], m.group(0)[:40]))
            m = re.search(r"\[sfx\] (\d+) (\d+) d ", line)
            if m and int(m.group(2)) in peaks:
                sounds.append((int(m.group(1)) + peaks[int(m.group(2))], int(m.group(2))))
            m = re.search(r"\[shake\] (\d+) ([\d.]+)", line)
            if m and float(m.group(2)) >= 3.0:      # (the faint ones are rumbles, not impacts)
                shakes.append((int(m.group(1)), float(m.group(2))))
        shakes_n += len(shakes)
        for t, mag in shakes:
            near = [(p - t, i) for p, i in sounds if abs(p - t) <= 300]
            if not near or min(abs(d) for d, _ in near) > 120:
                last = [mv for at, mv in moves if at <= t + 300][-1:]      # the move it came in
                unheard.append((os.path.basename(os.path.dirname(log)) + f" shake {mag:.0f}" + (" " + last[0] if last else ""), t, near and min(near, key=lambda d: abs(d[0]))))
            if near:
                d, i = min(near, key=lambda d: abs(d[0]))
                by_sound.setdefault(i, []).append(d)
    print(f"{shakes_n} impacts")
    print(f"{'sound':24s} {'n':>4s} {'median':>8s} {'worst':>7s}   (its peak minus the impact, ms)")
    for i, ds in sorted(by_sound.items(), key=lambda kv: -len(kv[1])):
        print(f"{table[i][0]:24s} {len(ds):4d} {int(np.median(ds)):+8d} {max(ds, key=abs):+7d}")
    print(f"\n{len(unheard)} impacts with no sound within 120 ms:")
    for run, t, near in unheard[:40]:
        print(f"  {run} at {t} ms" + (f": nearest {table[near[1]][0]} {near[0]:+d} ms" if near else ": nothing within 300 ms"))


if __name__ == "__main__":
    main()
