"""Brings the Bonus bosses' spell, fire, beam, chain and cry sounds up to the Guardian's level.

Loudness is measured as heard (EBU R128 momentary: K-weighted, the loudest 400 ms). The Guardian's
own attack sounds (assets/audio/sfx/boss) are -11 to -16 LUFS on it, -14.5 in the middle. A plain
RMS reads his thuds' brief transients as loud (-10 dB) and these bright spells as quiet: matching
that RMS left them 5-6 dB over him. Each is compressed if it needs lifting (its peaks are near
full scale), made up or turned down to the target and soft-limited under -1 dB, then written to
assets/audio/sfx/hostile/ under the same name. The originals stay: some are the knight's skills.

Needs numpy (Blender's bundled Python has it):
  /Applications/Blender.app/Contents/Resources/*/python/bin/python3.* tools/boss_sfx_levels.py
"""
import os
import wave

import numpy as np

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "audio", "sfx")
OUT = os.path.join(ROOT, "hostile")
TARGET_DB = -14.5       # LUFS, momentary max: the middle of the Guardian's attack sounds
SOURCES = ["fx/fire_breath", "fx/dark_beam", "fx/dark_nova", "fx/rock_blast", "fx/chain1", "fx/chain2",
           "fx/spell_cast", "fx/spell_wave", "fx/halfmoon_slash", "boss/war_cry", "character/char_swing1"]


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


def biquad(x, b, a):
    y = np.empty_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        y[i] = b[0] * v + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
        x2, x1, y2, y1 = x1, v, y1, y[i]
    return y


def k_weight(x, sr):
    """ITU-R BS.1770 K-weighting (its high shelf, then its high-pass), at any sample rate"""
    k = np.tan(np.pi * 1681.974450955533 / sr)
    q, vh = 0.7071752369554196, 10.0 ** (3.999843853973347 / 20.0)
    vb, a0 = vh ** 0.4996667741545416, 1.0 + k / q + k * k
    x = biquad(x, [(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0],
               [1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0])
    k, q = np.tan(np.pi * 38.13547087602444 / sr), 0.5003270373238773
    a0 = 1.0 + k / q + k * k
    return biquad(x, [1.0, -2.0, 1.0], [1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0])


def loud(x, sr):
    """EBU R128 momentary loudness, the loudest 400 ms, LUFS."""
    n = int(sr * 0.4)
    y = k_weight(np.concatenate([x, np.zeros(n)]), sr)
    c = np.cumsum(np.concatenate([[0.0], y * y]))
    return -0.691 + 10.0 * np.log10(((c[n:] - c[:-n]) / n).max() + 1e-12)


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
    lx = loud(x, sr)
    if abs(lx - TARGET_DB) < 0.5:
        return x
    if lx > TARGET_DB:      # louder than him already: just turned down
        return x * 10.0 ** ((TARGET_DB - lx) / 20.0)
    y = compress(x, sr, 20.0 * np.log10(np.abs(x).max()) - 8.0, 4.0)
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
        print(f"{s:24s} {loud(x, sr):6.1f} -> {loud(y, sr):6.1f} LUFS (peak {20 * np.log10(np.abs(y).max()):5.1f})")
