"""Builds the Fallen Warrior's Holy Slam gathering sound: the light called down, as a threat.

It was the knight's heal chime, which read as the boss healing: none of it is used. Here, at
22 kHz, over the ~1.2 s from the sword going up to the slam (fallen_knight.c holyslam_update:
1 s held, then 0.22 s of the strike):
  the draw     boss_land1's rumble played backwards, so the floor's roar is pulled in to the slam
  the swell    noise in a band rising 400 Hz -> 2.5 kHz, growing to the slam
  the drone    a saturated 55 Hz sine rising to 70 Hz, growing with it
All cut off in 40 ms at the slam. Written to assets/audio/sfx/hostile/holy_charge_22k.wav.

At the Bonus bosses' level (boss_sfx_levels.process).

Then the slam itself (Holy Slam's and the triangle slam's), Halfmoon Slash's impact
(tools/halfmoon_sfx.py) made heavier, with no blade: a crack and a hard thump dropping 150 -> 38 Hz
on the first sample, a big drum (a skin's pitch dropping 110 -> 70 Hz, its slap) struck with it and
again, softer and lower, 0.11 s on, a sub boom ringing out and the floor's rumble, 1.4 s. Written to
assets/audio/sfx/hostile/slam_impact_22k.wav at -12 LUFS, over the Guardian's -14.5: the Fallen
Warrior's biggest blows.

Needs numpy (Blender's bundled Python has it):
  /Applications/Blender.app/Contents/Resources/*/python/bin/python3.* tools/holy_slam_sfx.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boss_sfx_levels import OUT, ROOT, limit, loud, process, read, write    # noqa: E402
from halfmoon_sfx import band, env, sweep_band                        # noqa: E402

SLAM = 1.22     # s from the sword going up to the slam


if __name__ == "__main__":
    land, sr = read(os.path.join(ROOT, "boss", "boss_land1_22k.wav"))
    n = int((SLAM + 0.04) * sr)
    t = np.arange(n) / sr
    rng = np.random.default_rng(7)

    draw = np.zeros(n)
    tail = land[int(0.15 * sr):][::-1][-n:]       # its hit left out: the slam's own sound brings that
    draw[n - len(tail):] = tail
    draw = np.tanh(2.0 * draw / np.abs(draw).max())
    swell = sweep_band(rng.standard_normal(n), sr, 400.0, 2500.0, SLAM, width=0.6)
    swell *= (t / SLAM) ** 2 / np.abs(swell).max()
    f = 55.0 * (70.0 / 55.0) ** np.clip(t / SLAM, 0, 1)
    drone = np.tanh(3.0 * np.sin(2 * np.pi * np.cumsum(f) / sr)) * np.clip(t / SLAM, 0, 1) ** 1.5

    x = 0.8 * draw + 0.5 * swell + 0.6 * drone
    x *= np.clip((SLAM + 0.04 - t) / 0.04, 0.0, 1.0)      # cut at the slam, no click
    y = process(x / np.abs(x).max(), sr)
    out = os.path.join(OUT, "holy_charge_22k.wav")
    write(out, y, sr)
    print(f"{out}: {loud(y, sr):.1f} LUFS, peak {20 * np.log10(np.abs(y).max()):.1f} dB, {n / sr:.2f} s")

    # the slam
    LEVEL = -12.0
    n = int(1.4 * sr)
    t = np.arange(n) / sr

    def glide(f0, f1, over, t=t):
        return np.sin(2.0 * np.pi * np.cumsum(f0 * (f1 / f0) ** np.clip(t / over, 0.0, 1.0)) / sr)

    def drum(at, f0, f1):
        u = np.maximum(t - at, 0.0)
        skin = np.tanh(2.0 * glide(f0, f1, 0.05, u)) * env(u, 0.001, 0.2) * (t >= at)
        slap = band(rng.standard_normal(n), sr, 150, 1200) * np.exp(-u / 0.04) * (t >= at)
        return skin + 0.5 * slap / np.abs(slap).max()

    crack = band(rng.standard_normal(n), sr, 1000, 6000) * np.exp(-t / 0.006)
    thump = np.tanh(4.5 * glide(150.0, 38.0, 0.12)) * env(t, 0.002, 0.4)
    boom = glide(50.0, 36.0, 0.8) * env(t, 0.01, 0.6)
    rumble = band(rng.standard_normal(n), sr, 20, 250) * env(t, 0.03, 0.6)
    x = (0.6 * crack / np.abs(crack).max() + 1.6 * thump + 0.8 * drum(0.0, 110.0, 70.0) + 0.5 * drum(0.11, 90.0, 60.0)
         + 0.9 * boom + 0.5 * rumble / np.abs(rumble).max())
    x *= np.clip((1.4 - t) / 0.15, 0.0, 1.0)      # no click at the end
    x /= np.abs(x).max()
    for _ in range(12):     # to the level, through the limiter
        d = LEVEL - loud(limit(x), sr)
        if abs(d) < 0.2:
            break
        x *= 10.0 ** (d / 20.0)
    y = limit(x)
    out = os.path.join(OUT, "slam_impact_22k.wav")
    write(out, y, sr)
    print(f"{out}: {loud(y, sr):.1f} LUFS, peak {20 * np.log10(np.abs(y).max()):.1f} dB, {n / sr:.2f} s")
