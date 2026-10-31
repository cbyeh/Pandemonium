"""Makes the game's sounds that come from the Dark Fantasy Soulslike SFX Pack (Gomo Sounds,
mauriciopazp.itch.io/dark-fantasy-soulslike-sfx-pack). Its license forbids sharing the sounds as
files, so they aren't in the repo (.gitignore): get the pack and run this before building.

  python tools/pack_sfx.py path/to/Dark_Fantasy_Soulslike_SFX_Pack.zip    (or the unzipped folder)

Needs numpy (Blender's bundled Python has it) and ffmpeg. Each sound is cut to 22 kHz mono, its
attack set where the game's timing expects it, its trailing silence cut, levelled (the Bonus
bosses' at the Guardian's, as tools/boss_sfx_levels.py), and the dry ones given a short stone-hall
reverb in the style of the game's original sounds. The Fallen Warrior's are made heavier.
"""
import os
import subprocess
import sys
import tempfile
import zipfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boss_sfx_levels as L     # noqa: E402  (read, write, loud, limit, process)

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SFX = os.path.join(ROOT, "assets", "audio", "sfx")
SR = 22050
TMP = tempfile.mkdtemp()


def pack_root(path):
    if os.path.isdir(path):
        return path if os.path.isdir(os.path.join(path, "Sorcery")) else os.path.join(path, "Dark_Fantasy_Soulslike_SFX_Pack")
    zipfile.ZipFile(path).extractall(TMP)
    return os.path.join(TMP, "Dark_Fantasy_Soulslike_SFX_Pack")


def ffmpeg(src, af=None):
    out = os.path.join(TMP, "ff.wav")
    cmd = ["ffmpeg", "-v", "error", "-y", "-i", src] + (["-af", af] if af else []) + ["-ar", str(SR), "-ac", "1", "-c:a", "pcm_s16le", out]
    subprocess.run(cmd, check=True)
    return L.read(out)[0]


def attack(x):      # first 10 ms frame within 6 dB of the loudest
    hop = SR // 100; n = len(x) // hop
    e = 10 * np.log10((x[:n * hop].reshape(n, hop) ** 2).mean(1) + 1e-12)
    return int(np.argmax(e > e.max() - 6)) * hop


def at(x, samples):     # the attack moved to `samples` in (the old sound's, so the game's timing holds)
    d = samples - attack(x)
    if d > 0:
        return np.concatenate([np.zeros(d), x])
    y = x[-d:].copy(); f = min(len(y), SR // 200); y[:f] *= np.linspace(0, 1, f)
    return y


def lead(x):        # the attack 20 ms in: heard as the move happens
    return x[max(0, attack(x) - SR // 50):]


def trim_tail(x, floor_db=-55):
    hop = SR // 100; n = len(x) // hop
    e = 10 * np.log10((x[:n * hop].reshape(n, hop) ** 2).mean(1) + 1e-12)
    y = x[:(np.where(e > floor_db)[0][-1] + 1) * hop].copy()
    f = min(len(y), SR // 50); y[-f:] *= np.linspace(1, 0, f)
    return y


def to_lufs(x, db):
    return L.limit(x * 10 ** ((db - L.loud(x, SR)) / 20))


def boss(x, db=0.0):    # the Guardian's level (boss_sfx_levels), `db` under it
    return L.process(x, SR) * 10 ** (db / 20)


def _ir(rt60=1.1, pre=0.02):
    rng = np.random.default_rng(1); n = int(SR * rt60 * 1.2); t = np.arange(n) / SR
    h = rng.standard_normal(n) * np.exp(-6.9 * t / rt60); out = np.zeros(n); s = 0.0
    for i in range(n):      # darker as it decays
        a = np.exp(-2 * np.pi * (6000 * np.exp(-2.5 * t[i]) + 800) / SR); s = (1 - a) * h[i] + a * s; out[i] = s
    out = np.concatenate([np.zeros(int(pre * SR)), out])
    return out / np.sqrt((out ** 2).sum())


IR = _ir()


def verb(x, wet_db=-9.0, keep_level=False):
    n = len(x) + len(IR) - 1; N = 1 << (n - 1).bit_length()
    w = np.fft.irfft(np.fft.rfft(x, N) * np.fft.rfft(IR, N), N)[:n]
    y = trim_tail(np.concatenate([x, np.zeros(len(IR) - 1)]) + w * 10 ** (wet_db / 20))
    return to_lufs(y, L.loud(x, SR)) if keep_level else y


def heavy(x):       # deeper and heavier: pitched down 0.82x, the low end lifted
    src = os.path.join(TMP, "h.wav"); L.write(src, np.clip(x, -1, 1), SR)
    return ffmpeg(src, "asetrate=22050*0.82,aresample=22050,bass=g=5:f=110,lowpass=f=7000")


def main():
    P = pack_root(sys.argv[1])
    pk = lambda name: ffmpeg(os.path.join(P, name + ".wav"))
    orig = lambda name: L.read(os.path.join(SFX, name + "_22k.wav"))[0]
    out = {}

    # The Bonus bosses' (the attack where the old sound's was: fire 0.01 s, cast 0.03, nova 0.28, beam 0.12, blast 0.24)
    out["hostile/fire_burst"] = boss(trim_tail(at(pk("Sorcery/fire_sorcery_explode_01"), 220)))     # the Fallen Warrior's lunge
    out["hostile/fire_sun"] = boss(trim_tail(at(pk("Sorcery/fire_sorcery_explode_02"), 220)))
    out["hostile/fire_dragon"] = boss(trim_tail(at(pk("Sorcery/fire_sorcery_explode_03"), 220)))
    out["hostile/dragon_breath"] = verb(boss(trim_tail(at(pk("Enemies/ancient_dragon_breathe_fire_03"), 220))), keep_level=True)
    out["hostile/wing_flap"] = boss(trim_tail(lead(pk("Enemies/ancient_dragon_flap_wings_01"))), -6)
    out["hostile/wing_flyby"] = boss(trim_tail(lead(pk("Enemies/ancient_dragon_flap_wings_03"))), -3)
    out["hostile/spell_cast"] = verb(boss(trim_tail(at(pk("Sorcery/dark_sorcery_cast_03"), 660))), keep_level=True)
    out["hostile/dark_nova"] = boss(trim_tail(at(pk("Sorcery/dark_sorcery_cast_01"), 6160)))
    out["hostile/dark_beam"] = verb(boss(trim_tail(at(pk("Sorcery/dark_sorcery_cast_02"), 2640))), keep_level=True)
    out["hostile/rock_blast"] = boss(trim_tail(at(pk("Sorcery/fire_sorcery_explode_02"), 5280)))
    out["hostile/fw_slice"] = boss(heavy(verb(lead(pk("Player/ashen_knight_heavy_attack_03")))))   # the Fallen Warrior's Moon Blade
    # The knight's (at the old sounds' loudness: potion and clank 5 dB under them)
    roll = to_lufs(trim_tail(at(pk("Player/ashen_knight_dodge_roll_01"), 0)), -30.968)
    out["character/roll_swish"] = verb(roll, keep_level=True)
    out["hostile/fw_roll"] = to_lufs(heavy(verb(roll)), L.loud(orig("boss/boss_step1"), SR))
    out["character/armor_clank"] = verb(to_lufs(trim_tail(at(pk("Weapons/longsword_hit_armor_01"), 2640)), -21.981 - 5), keep_level=True)
    out["fx/heal_chime"] = verb(to_lufs(trim_tail(at(pk("Player/ashen_knight_drink_flask_02"), 5060)), -24.703 - 5), keep_level=True)
    out["fx/mbd_x"] = to_lufs(verb(lead(pk("Player/ashen_knight_heavy_attack_01"))), -17.9)          # Moon Blade Dance
    out["fx/cleave_beam"] = to_lufs(verb(lead(pk("Player/ashen_knight_light_attack_03")), -12), -17.9)  # Crescent Cleave
    umph = L.loud(orig("character/umph"), SR)
    out["character/hurt1"] = to_lufs(verb(lead(pk("Player/ashen_knight_hurt_01"))), umph)
    out["character/hurt2"] = to_lufs(verb(lead(pk("Player/ashen_knight_hurt_03"))), umph)
    # Halfmoon Slash: the heavy attack at the start of the made sound (tools/sfx_src: tools/halfmoon_sfx.py's)
    hm = L.read(os.path.join(ROOT, "tools", "sfx_src", "halfmoon_slash_made_22k.wav"))[0]
    h3 = verb(lead(pk("Player/ashen_knight_heavy_attack_03")))
    h3 = h3 * 10 ** ((L.loud(hm, SR) - 2 - L.loud(h3, SR)) / 20)
    mix = np.zeros(max(len(hm), len(h3))); mix[:len(hm)] += hm; mix[:len(h3)] += h3
    out["fx/halfmoon_slash"] = L.limit(mix * 10 ** ((L.loud(hm, SR) - L.loud(mix, SR)) / 20))

    for name, y in out.items():
        L.write(os.path.join(SFX, name + "_22k.wav"), y, SR)
        print(f"{name:24s} {len(y) / SR:4.2f}s {L.loud(y, SR):6.1f} LUFS")


if __name__ == "__main__":
    main()
