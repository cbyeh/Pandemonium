"""Find a seamless loop in a music WAV and write it as a `smpl` loop (audioconv64 reads it).

Searches the track's beat grid for a jump B -> A a whole number of bars long, whose
surrounding audio matches (spectral match over the window around both points),
bakes a short crossfade before B into the audio before A, cuts the track at B (or keeps the
rest as a release tail with --keep-tail) and writes the loop [A, B).
Needs numpy: run with Blender's Python.
  python tools/music_loop.py in.wav out.wav --bpm 96 [--min-loop 40] [--pick N] [--keep-tail] [--seam seam.wav]
"""
import argparse, struct, wave
import numpy as np

HOP, NFFT, BANDS, XFADE = 512, 2048, 48, 0.04


def read(path):
    with wave.open(path) as w:
        assert w.getsampwidth() == 2
        sr, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, ch)
    return sr, x


def write(path, sr, x, loop=None):
    with wave.open(path, "wb") as w:
        w.setnchannels(x.shape[1]); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(x.astype(np.int16).tobytes())
    if loop:  # RIFF smpl chunk, one forward loop; lastSampleOffset is inclusive
        a, b = loop
        smpl = struct.pack("<9I", 0, 0, int(1e9 / sr), 60, 0, 0, 0, 1, 0)
        smpl += struct.pack("<6I", 0, 0, a, b - 1, 0, 0)
        with open(path, "r+b") as f:
            f.seek(0, 2)
            f.write(b"smpl" + struct.pack("<I", len(smpl)) + smpl)
            size = f.tell() - 8
            f.seek(4); f.write(struct.pack("<I", size))


def features_raw(mono):
    n = (len(mono) - NFFT) // HOP
    idx = np.arange(NFFT)[None, :] + HOP * np.arange(n)[:, None]
    return np.abs(np.fft.rfft(mono[idx] * np.hanning(NFFT), axis=1))


OHOP, ONFFT = 64, 512


def onset_flux(mono):
    """Onset strength every OHOP samples; frame i is centred on sample i * OHOP + ONFFT / 2."""
    n = (len(mono) - ONFFT) // OHOP
    idx = np.arange(ONFFT)[None, :] + OHOP * np.arange(n)[:, None]
    m = np.log1p(np.abs(np.fft.rfft(mono[idx] * np.hanning(ONFFT), axis=1)))
    return np.r_[0, np.maximum(np.diff(m, axis=0), 0).sum(axis=1)]


def beat_phase(flux, beat, start, end):
    """Sample of the first beat at or after `start`: where the onsets in [start, end) line up best."""
    at = lambda p: flux[np.round((np.arange(p, end - ONFFT, beat) - ONFFT / 2) / OHOP).astype(int)].mean()
    phases = start + np.arange(0, beat, 8)
    return phases[int(np.argmax([at(p) for p in phases]))]


def features(mono):
    mag = features_raw(mono)
    edges = np.unique(np.geomspace(2, mag.shape[1] - 1, BANDS + 1).astype(int))
    f = np.log1p(np.add.reduceat(mag, edges[:-1], axis=1))
    f -= f.mean(axis=1, keepdims=True)
    return f / (np.linalg.norm(f, axis=1, keepdims=True) + 1e-9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp"); ap.add_argument("out")
    ap.add_argument("--min-loop", type=float, default=40.0, help="shortest loop, seconds")
    ap.add_argument("--bpm", type=float, required=True, help="the track's tempo (a 4/4 bar is 4 beats)")
    ap.add_argument("--phrase", type=int, default=4, help="the loop is a multiple of this many bars")
    ap.add_argument("--at", type=float, help="start the loop on the beat nearest this time, seconds")
    ap.add_argument("--after", type=float, default=0.0, help="start the loop no earlier than this, seconds (the beat grid is measured from here)")
    ap.add_argument("--grid", type=float, help="a beat falls at this time, seconds (skips measuring the grid)")
    ap.add_argument("--bars", type=int, help="loop exactly this many bars")
    ap.add_argument("--top", type=int, default=5, help="list this many candidates")
    ap.add_argument("--pick", type=int, default=0, help="use the Nth best candidate")
    ap.add_argument("--window", type=float, default=8.0, help="matched audio around the jump, seconds")
    ap.add_argument("--keep-tail", action="store_true", help="keep audio after B (played when not looping)")
    ap.add_argument("--seam", help="write the 6 s before B then 6 s after A, as heard in game")
    args = ap.parse_args()

    sr, x = read(args.inp)
    mono = x.astype(np.float64).mean(axis=1)
    F = features(mono)
    n = len(F)
    half = int(args.window * sr / HOP) // 2
    loud = np.convolve(np.abs(mono), np.ones(HOP), "same")[::HOP][:n]
    quiet = loud < 0.05 * np.median(loud)  # fades: never jump from or into them

    # The beat grid: the loop is a whole number of bars, and both ends sit on a beat
    # Measured only from --after on: free-time intros would throw the grid off
    beat = 60.0 / args.bpm * sr
    flux = onset_flux(mono)
    phase = args.grid * sr if args.grid is not None else beat_phase(flux, beat, int(args.after * sr), len(mono))
    beats = np.arange(phase, len(mono) - NFFT, beat)

    best = []
    if args.at is not None:  # pin the loop start to the beat nearest a given time
        beats = beats[[int(np.argmin(np.abs(beats - args.at * sr)))]]
    for bars in range(args.phrase, int(len(mono) / (4 * beat)) + 1, args.phrase):  # whole phrases only
        if bars * 4 * beat < args.min_loop * sr or (args.bars and bars != args.bars):
            continue
        L = int(round(bars * 4 * beat))
        for A in beats:
            A = int(round(A)); B = A + L
            fa, fb = A // HOP, B // HOP
            if fa - half < 0 or fb + half > n or quiet[fa] or quiet[fb]:
                continue
            # similarity of the whole window around A and around B, frame by frame
            score = float(np.einsum("ij,ij->", F[fa - half:fa + half], F[fb - half:fb + half])) / (2 * half)
            best.append((score, A, B))
    best.sort(reverse=True)
    # Distinct candidates only: a start a beat away from a better one is the same seam
    picked = []
    for c in best:
        if all(abs(c[1] - p[1]) > 2 * beat or abs((c[2] - c[1]) - (p[2] - p[1])) > beat for p in picked):
            picked.append(c)
    for i, (sc, a, b) in enumerate(picked[:args.top]):
        print(f"  #{i} {a / sr:7.3f}s -> {b / sr:7.3f}s ({(b - a) / (4 * beat):.0f} bars) score {sc:.3f}")
    score, A, B = picked[args.pick]

    # Fine-tune B so the onsets after it land where the onsets after A do (the tempo is
    # rarely exactly --bpm): +-40 ms, onset strength correlated over the window
    w, R = int(args.window * sr / 2 / OHOP), int(0.04 * sr / OHOP)
    fa, fb = (A - ONFFT // 2) // OHOP, (B - ONFFT // 2) // OHOP
    ref = flux[fa - w:fa + w] - flux[fa - w:fa + w].mean()
    lags = [k for k in range(-R, R + 1) if fb + k + w <= len(flux)]
    k = max(lags, key=lambda k: np.dot(ref, flux[fb + k - w:fb + k + w] - flux[fb + k - w:fb + k + w].mean()))
    if args.grid is not None:  # a known grid needs no nudge
        k = 0
    B += k * OHOP

    # Crossfade the audio before B into the audio before A, so the jump B -> A is continuous
    y = x.astype(np.float64).copy()
    nx = int(XFADE * sr)
    t = np.linspace(0, np.pi / 2, nx)[:, None]
    y[B - nx:B] = y[B - nx:B] * np.cos(t) + x[A - nx:A] * np.sin(t)
    y = np.clip(np.round(y), -32768, 32767)
    if not args.keep_tail:
        y = y[:B]
    write(args.out, sr, y, (A, B))
    print(f"{args.inp}: loop {A / sr:.3f}s -> {B / sr:.3f}s (len {(B - A) / sr:.1f}s of {len(x) / sr:.1f}s) "
          f"score {score:.3f}, B nudged {k * OHOP / sr * 1000:+.0f} ms")

    if args.seam:
        s = int(6 * sr)
        write(args.seam, sr, np.concatenate([y[B - s:B], y[A:A + s]]))


if __name__ == "__main__":
    main()
