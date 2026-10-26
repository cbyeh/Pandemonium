"""Brings the Bonus bosses' spell, fire, beam, chain and cry sounds up to the Guardian's level.

The Guardian's own sounds (assets/audio/sfx/boss) are about -10 dB at their loudest 0.3 s; these
were -16 to -27 dB, with peaks near full scale, so turning up their volume in the game would only
clip them (and the channel volume is capped). Each is compressed (a fast envelope over a
threshold), made up to the target and soft-limited under -1 dB, then written to
assets/audio/sfx/hostile/ under the same name. The originals stay: some are the knight's skills.

Needs numpy (Blender's bundled Python has it):
  /Applications/Blender.app/Contents/Resources/*/python/bin/python3.* tools/boss_sfx_levels.py
"""
import os
import wave

import numpy as np

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "audio", "sfx")
OUT = os.path.join(ROOT, "hostile")
TARGET_DB = -10.5       # the Guardian's sounds: -8 to -14 dB, about -10.4 on average
SOURCES = ["fx/fire_breath", "fx/dark_beam", "fx/dark_nova", "fx/rock_blast", "fx/chain1", "fx/chain2",
           "fx/spell_cast", "fx/spell_wave", "fx/blade_slash", "fx/heal_chime", "boss/war_cry", "character/char_swing1"]


def read(path):
    w = wave.open(path)
    assert w.getnchannels() == 1 and w.getsampwidth() == 2, path
    return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0, w.getframerate()


def write(path, x, sr):
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes(np.clip(np.rint(x * 32767.0), -32768, 32767).astype(np.int16).tobytes())
    w.close()


def loud(x, sr):
    """RMS of the loudest 0.3 s, dB."""
    n = min(len(x), int(sr * 0.3))
    c = np.cumsum(np.concatenate([[0.0], x * x]))
    return 10.0 * np.log10(((c[n:] - c[:-n]) / n).max() + 1e-12)


def compress(x, sr, thresh_db, ratio, attack=0.004, release=0.09):
    a, r = np.exp(-1.0 / (attack * sr)), np.exp(-1.0 / (release * sr))
    env, g = 0.0, np.empty_like(x)
    t = 10.0 ** (thresh_db / 20.0)
    for i, v in enumerate(np.abs(x)):
        env = (a if v > env else r) * env + (1.0 - (a if v > env else r)) * v
        g[i] = 1.0 if env <= t else (t * (env / t) ** (1.0 / ratio)) / env
    return x * g


def limit(x, ceiling_db=-1.0):
    c = 10.0 ** (ceiling_db / 20.0)
    return c * np.tanh(x / c)


def process(x, sr):
    if loud(x, sr) >= TARGET_DB - 0.5:
        return x
    y = compress(x, sr, loud(x, sr) - 2.0, 4.0)
    for _ in range(12):     # make-up gain to the target, through the limiter
        z = limit(y)
        d = TARGET_DB - loud(z, sr)
        if abs(d) < 0.2:
            break
        y *= 10.0 ** (d / 20.0)
    return limit(y)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for s in SOURCES:
        x, sr = read(os.path.join(ROOT, s + "_22k.wav"))
        y = process(x, sr)
        write(os.path.join(OUT, os.path.basename(s) + "_22k.wav"), y, sr)
        print(f"{s:24s} {loud(x, sr):6.1f} -> {loud(y, sr):6.1f} dB (peak {20 * np.log10(np.abs(y).max()):5.1f})")
