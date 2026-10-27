"""Builds Halfmoon Slash's sound (the knight's and the Fallen Warrior's): a big, bassy impact.

blade_slash alone was a light swish for a blow that shakes the hall. Layered here, all at 22 kHz:
  the swish         fx/blade_slash, its lows cut (the blade still reads)
  the blade wind    noise in a band sweeping 3 kHz -> 800 Hz over the 0.55 s the arc is on
                    screen (skill_fx_spawn_arc), swelling in under the swish and fading with
                    the arc: the slash lasts as long as its wave rolls out
  the crack         a 6 ms burst of 1-6 kHz noise on the first sample: the hit's edge
  the thump         a sine dropping 160 -> 48 Hz over 0.09 s (a kick drum's fast drop is its
                    punch), driven hard so its harmonics carry on TV speakers that can't play 48 Hz
  the boom          a sub sine 52 -> 38 Hz, ringing out over ~1.2 s
  the rumble        noise under 250 Hz, swelling in then settling over ~1.5 s (the floor)
Written to assets/audio/sfx/fx/halfmoon_slash_22k.wav at the knight's skill level; the Fallen
Warrior's copy is made from it by tools/boss_sfx_levels.py.

Needs numpy (Blender's bundled Python has it):
  /Applications/Blender.app/Contents/Resources/*/python/bin/python3.* tools/halfmoon_sfx.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boss_sfx_levels import ROOT, limit, loud, read, write    # noqa: E402

LEVEL = -13.0       # LUFS (momentary max): the knight's biggest skill, over his others (-18 to -19) and the Guardian (-14.5)
DUR = 1.8


def band(x, sr, lo, hi):
    """Noise or a layer cut to lo..hi Hz (soft edges), via the FFT"""
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    m = np.ones_like(f)
    if lo > 0:
        m *= 1.0 / (1.0 + (lo / np.maximum(f, 1e-3)) ** 4)
    if hi < sr / 2:
        m *= 1.0 / (1.0 + (f / hi) ** 4)
    return np.fft.irfft(np.fft.rfft(x) * m, len(x))


def sweep_band(x, sr, f0, f1, over, width=0.7):
    """`x` through a band centred f0 -> f1 Hz (exponentially, over `over` s), `width` octaves
    each side: 23 ms frames, each filtered on its own and overlap-added (Hann, half overlap)"""
    n, hop = int(0.023 * sr), int(0.023 * sr) // 2
    win, y = np.hanning(n), np.zeros(len(x) + n)
    f = np.fft.rfftfreq(n, 1.0 / sr)
    for s in range(0, len(x), hop):
        seg = np.zeros(n)
        seg[:len(x[s:s + n])] = x[s:s + n]
        c = f0 * (f1 / f0) ** min(s / sr / over, 1.0)
        m = np.exp(-0.5 * (np.log2(np.maximum(f, 1.0) / c) / width) ** 2)
        y[s:s + n] += np.fft.irfft(np.fft.rfft(seg * win) * m, n)
    return y[:len(x)]


def env(t, attack, decay):
    return np.clip(t / attack, 0.0, 1.0) * np.exp(-np.maximum(t - attack, 0.0) / decay)


def sweep(t, f0, f1, over):
    """A sine gliding f0 -> f1 Hz over `over` s (exponentially), then holding f1"""
    k = np.clip(t / over, 0.0, 1.0)
    f = f0 * (f1 / f0) ** k
    return np.sin(2.0 * np.pi * np.cumsum(f) / SR)


if __name__ == "__main__":
    swish, SR = read(os.path.join(ROOT, "fx", "blade_slash_22k.wav"))
    n = int(DUR * SR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(5)

    sw = np.zeros(n)
    sw[:len(swish)] = band(swish, SR, 500, SR / 2)[:n]
    ARC = 0.55      # the arc's life on screen
    wind = sweep_band(rng.standard_normal(n), SR, 3000.0, 800.0, ARC)
    wind *= np.clip(t / 0.06, 0.0, 1.0) * np.clip((ARC + 0.1 - t) / 0.3, 0.0, 1.0) ** 1.5
    wind /= np.abs(wind).max()
    crack = band(rng.standard_normal(n), SR, 1000, 6000) * np.exp(-t / 0.006)
    crack /= np.abs(crack).max()
    thump = np.tanh(3.5 * sweep(t, 160.0, 48.0, 0.09)) * env(t, 0.002, 0.3)
    boom = sweep(t, 52.0, 38.0, 0.8) * env(t, 0.01, 0.6)
    rumble = band(rng.standard_normal(n), SR, 20, 250) * env(t, 0.03, 0.7)
    rumble /= np.abs(rumble).max()

    x = 0.45 * sw / np.abs(sw).max() + 0.35 * wind + 0.5 * crack + 1.4 * thump + 0.8 * boom + 0.45 * rumble
    x *= np.clip((DUR - t) / 0.15, 0.0, 1.0)      # no click at the end
    x /= np.abs(x).max()
    for _ in range(12):     # to the level, through the limiter
        y = limit(x)
        d = LEVEL - loud(y, SR)
        if abs(d) < 0.2:
            break
        x *= 10.0 ** (d / 20.0)
    y = limit(x)
    out = os.path.join(ROOT, "fx", "halfmoon_slash_22k.wav")
    write(out, y, SR)
    print(f"{out}: {loud(y, SR):.1f} LUFS, peak {20 * np.log10(np.abs(y).max()):.1f} dB, {DUR} s")
