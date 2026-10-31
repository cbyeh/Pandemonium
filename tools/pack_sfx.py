"""Makes the game's sounds that come from the Dark Fantasy Soulslike SFX Pack (Gomo Sounds,
mauriciopazp.itch.io/dark-fantasy-soulslike-sfx-pack). Its license forbids sharing the sounds as
files, so they aren't in the repo (.gitignore): get the pack and run this before building.

  python tools/pack_sfx.py path/to/Dark_Fantasy_Soulslike_SFX_Pack.zip    (or the unzipped folder)

Needs numpy (Blender's bundled Python has it) and ffmpeg. Each sound is cut to 22 kHz mono, its
attack set where the game's timing expects it, its trailing silence cut, given a stone-hall reverb
as long as the game's original sounds' (the bosses' ring ~1.3 s, the knight's ~0.8 s) and levelled:
the Bonus bosses' at the Guardian's (tools/boss_sfx_levels.py), the knight's skills at his sword
hits as heard (the file's loudness with the volume the game plays it at). The Fallen Warrior's are
made heavier. It also remakes, from their dry sources in tools/sfx_src, the CC0 and made sounds
that get the same reverb (those are in the repo).
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


def _ir(rt60, pre=0.02):
    rng = np.random.default_rng(1); n = int(SR * rt60 * 1.2); t = np.arange(n) / SR
    h = rng.standard_normal(n) * np.exp(-6.9 * t / rt60); out = np.zeros(n); s = 0.0
    for i in range(n):      # darker as it decays
        a = np.exp(-2 * np.pi * (6000 * np.exp(-2.5 * t[i]) + 800) / SR); s = (1 - a) * h[i] + a * s; out[i] = s
    out = np.concatenate([np.zeros(int(pre * SR)), out])
    return out / np.sqrt((out ** 2).sum())


HALL = _ir(2.2)     # the bosses': their sounds ring ~1.3 s (-10 to -40 dB), the Guardian's 0.8-2.0
ROOM = _ir(1.6)     # the knight's: ~0.8 s, his 0.6-1.1


def verb(x, ir=HALL, wet_db=-6.0):
    n = len(x) + len(ir) - 1; N = 1 << (n - 1).bit_length()
    w = np.fft.irfft(np.fft.rfft(x, N) * np.fft.rfft(ir, N), N)[:n]
    return trim_tail(np.concatenate([x, np.zeros(len(ir) - 1)]) + w * 10 ** (wet_db / 20))


def heard(db, vol):     # the file's loudness that plays at `db` when the game plays it at `vol`
    return db - 20 * np.log10(vol)


def cut(x, secs):   # its first `secs`, faded out over the last 0.15
    y = x[:int(secs * SR)].copy(); f = min(len(y), int(0.15 * SR)); y[-f:] *= np.linspace(1, 0, f)
    return y


def pitch(x, k):    # k times the pitch (and speed)
    src = os.path.join(TMP, "p.wav"); L.write(src, np.clip(x, -1, 1), SR)
    return ffmpeg(src, f"asetrate=22050*{k},aresample=22050")


def heavy(x):       # deeper and heavier: pitched down 0.82x, the low end lifted
    src = os.path.join(TMP, "h.wav"); L.write(src, np.clip(x, -1, 1), SR)
    return ffmpeg(src, "asetrate=22050*0.82,aresample=22050,bass=g=5:f=110,lowpass=f=7000")


def main():
    P = pack_root(sys.argv[1])
    pk = lambda name: ffmpeg(os.path.join(P, name + ".wav"))
    orig = lambda name: L.read(os.path.join(SFX, name + "_22k.wav"))[0]
    dry = lambda name: L.read(os.path.join(ROOT, "tools", "sfx_src", name + "_22k.wav"))[0]
    hits = np.mean([L.loud(orig(f"character/char_attack_hit{i}"), SR) for i in range(1, 7)])  # the knight's sword hits
    step = L.loud(orig("boss/boss_step1"), SR)
    out, kept = {}, {}

    # The Bonus bosses' (the attack where the old sound's was: fire 0.01 s, cast 0.03, nova 0.28, beam 0.12, blast 0.24)
    out["hostile/fire_burst"] = boss(verb(trim_tail(at(pk("Sorcery/fire_sorcery_explode_01"), 220))))     # the Fallen Warrior's lunge
    out["hostile/fire_sun"] = boss(verb(trim_tail(at(pk("Sorcery/fire_sorcery_explode_02"), 220))))
    out["hostile/fire_dragon"] = boss(verb(trim_tail(at(pk("Sorcery/fire_sorcery_explode_03"), 220))))
    out["hostile/dragon_breath"] = boss(verb(trim_tail(at(pk("Enemies/ancient_dragon_breathe_fire_03"), 220))))
    out["hostile/wing_flap"] = boss(verb(trim_tail(lead(pk("Enemies/ancient_dragon_flap_wings_01")))), -6)   # (under the attacks)
    out["hostile/wing_flyby"] = boss(verb(trim_tail(lead(pk("Enemies/ancient_dragon_flap_wings_03")))), -3)
    out["hostile/spell_cast"] = boss(verb(trim_tail(at(pk("Sorcery/dark_sorcery_cast_03"), 660))))
    out["hostile/dark_nova"] = boss(verb(trim_tail(at(pk("Sorcery/dark_sorcery_cast_01"), 6160))))
    out["hostile/dark_beam"] = boss(verb(trim_tail(at(pk("Sorcery/dark_sorcery_cast_02"), 2640))))
    out["hostile/rock_blast"] = boss(verb(trim_tail(at(pk("Sorcery/fire_sorcery_explode_02"), 5280))))
    out["hostile/fw_slice"] = boss(heavy(verb(lead(pk("Player/ashen_knight_heavy_attack_03")))))   # the Fallen Warrior's Moon Blade
    # The knight's: skills at his sword hits as heard; hurt at the old grunt's; potion, clank, roll as quiet as chosen
    roll = to_lufs(trim_tail(at(pk("Player/ashen_knight_dodge_roll_01"), 0)), -30.968)
    out["character/roll_swish"] = to_lufs(verb(roll, ROOM), -30.968)
    out["hostile/fw_roll"] = to_lufs(heavy(verb(roll)), step)
    out["character/armor_clank"] = to_lufs(verb(trim_tail(at(pk("Weapons/longsword_hit_armor_01"), 2640)), ROOM), -21.981 - 5)
    out["fx/heal_chime"] = to_lufs(verb(trim_tail(at(pk("Player/ashen_knight_drink_flask_02"), 5060)), ROOM), -24.703 - 5)
    out["fx/mbd_x"] = to_lufs(verb(lead(pk("Player/ashen_knight_heavy_attack_01")), ROOM), heard(hits, 0.55))     # Moon Blade Dance
    out["fx/cleave_beam"] = to_lufs(verb(lead(pk("Player/ashen_knight_light_attack_03")), ROOM), heard(hits, 0.45))  # Crescent Cleave
    # The Shackled Sun's own: its roar (a giant's, pitched up), the swell of a charge, a ram's rush, a darker eclipse
    out["hostile/sun_roar"] = boss(verb(lead(pitch(pk("Enemies/giant_knight_roar_01"), 1.25))))
    out["hostile/sun_swell"] = boss(verb(lead(pk("Sorcery/fire_sorcery_cast_03"))), -2)
    out["hostile/sun_rush"] = boss(verb(cut(lead(pk("Sorcery/fire_sorcery_cast_02")), 0.9)))     # as long as a ram
    out["hostile/sun_eclipse"] = boss(heavy(verb(lead(pk("Sorcery/dark_sorcery_cast_02")))))
    out["hostile/fw_hurt"] = boss(heavy(verb(lead(pk("Player/ashen_knight_hurt_01")))), -3)     # the Fallen Warrior's grunt as it staggers
    # Halfmoon Slash: the heavy attack at the start of the made sound (tools/halfmoon_sfx.py's), the whole at his hits
    hm = dry("halfmoon_slash_made")
    h3 = verb(lead(pk("Player/ashen_knight_heavy_attack_03")), ROOM)
    h3 = h3 * 10 ** ((L.loud(hm, SR) - 2 - L.loud(h3, SR)) / 20)
    mix = np.zeros(max(len(hm), len(h3))); mix[:len(hm)] += hm; mix[:len(h3)] += h3
    out["fx/halfmoon_slash"] = to_lufs(verb(mix, ROOM, -9), heard(hits, 1.0))

    # In the repo: the CC0 and made sounds, the same reverb over their dry sources
    kept["fx/spell_cast"] = to_lufs(verb(dry("spell_cast_rubberduck"), ROOM), heard(hits, 0.8))     # the perfect dodge
    kept["hostile/spell_wave"] = boss(verb(dry("spell_wave_rubberduck_boss")))     # Moonlight Splitter's cue
    for n in ("chain1", "chain2", "war_cry", "slam_impact"):
        kept["hostile/" + n] = boss(verb(dry(n)))
    kept["hostile/halfmoon_slash"] = boss(verb(dry("halfmoon_slash_boss")))       # Moonlight Splitter's crescents
    kept["boss/dragon_roar"] = to_lufs(verb(dry("dragon_roar")), L.loud(dry("dragon_roar"), SR))
    kept["hostile/dragon_growl"] = to_lufs(verb(dry("dragon_growl")), L.loud(dry("dragon_growl"), SR))
    kept["hostile/char_swing1"] = boss(verb(heavy(orig("character/char_swing1")), HALL, -9))    # the Fallen Warrior's swing
    for i in range(1, 5):   # its footfalls: the knight's, heavier, at the Guardian's
        kept[f"hostile/fw_step{i}"] = to_lufs(verb(heavy(orig(f"character/char_footstep_run{i}")), ROOM, -9), step)

    for name, y in list(out.items()) + list(kept.items()):
        L.write(os.path.join(SFX, name + "_22k.wav"), y, SR)
        print(f"{name:24s} {len(y) / SR:4.2f}s {L.loud(y, SR):6.1f} LUFS{'' if name in out else '  (in the repo)'}")


if __name__ == "__main__":
    main()
