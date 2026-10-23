#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 3dSam (Clipper)
"""Clipper's built-in sources for GRAIN and SAMPLE, synthesized here (no recording, no third party):

  VOX    a small choir, "oo" opening to "ah" (the vowel moves through the first third: GRAIN's POS scans it)
  GLASS  a glass harmonica: beating inharmonic partials, the top fading slowly (POS: bright to pure)
  PAD    a seven-saw ensemble, its low-pass opening through the first half (POS: dark to bright)
  AIR    breath through resonances on the note's harmonics: a flute / whistle of air

  tools/gen_clipper_sets.py [OUT_DIR]   (default assets/clipper-sets): one mono 16-bit WAV a set and sets.json
  (name, file, rate, root, loop start / end in samples, the last one played). tools/gen_samples.py reads them
  into the Clipper build (IMA ADPCM, ~25 KB of flash). Needs numpy and scipy; the WAVs are in the tree, so the
  build does not run this. Deterministic (fixed seeds).

Each sound sustains on a loop whose seam is cross-faded (equal power over 90 ms): SAMPLE holds a note on it.
Stored at 16 kHz with nothing above ~6.3 kHz: a note two octaves over the root still stays under 22 kHz.
"""
import json
import sys
import wave
from pathlib import Path

import numpy as np
from scipy.signal import butter, resample_poly, sosfiltfilt, sosfilt

FS = 48000                       # synthesis rate
OUT_SR = 16000                   # stored rate
TOP = 6300.0                     # nothing above (Hz)
ROOT = Path(__file__).resolve().parents[1]


def hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def smooth_noise(rng, n, rate_hz, fs=FS):
    """slow random curve, -1..1, about rate_hz wiggles a second"""
    k = max(2, int(n / fs * rate_hz) + 3)
    pts = rng.uniform(-1, 1, k)
    x = np.linspace(0, k - 1, n)
    i = np.floor(x).astype(int).clip(0, k - 2)
    f = x - i
    f = f * f * (3 - 2 * f)
    return pts[i] * (1 - f) + pts[i + 1] * f


def vib(rng, n, rate, depth_ct, onset_s):
    """vibrato in cents: a slightly wandering rate, fading in"""
    t = np.arange(n) / FS
    r = rate * (1 + 0.06 * smooth_noise(rng, n, 0.7))
    ph = 2 * np.pi * np.cumsum(r) / FS + rng.uniform(0, 2 * np.pi)
    fade = np.clip(t / onset_s, 0, 1) ** 2
    return depth_ct * fade * np.sin(ph) + 3.0 * smooth_noise(rng, n, 3.0)


def additive(f0_t, amp_fn, nmax, rng):
    """harmonics of a time-varying f0 (array, Hz), amp_fn(k, fk) -> per-sample amplitude; band-limited to TOP"""
    phase0 = 2 * np.pi * np.cumsum(f0_t) / FS
    out = np.zeros_like(f0_t)
    for k in range(1, nmax + 1):
        fk = k * f0_t
        if fk.min() > TOP:
            break
        a = amp_fn(k, fk) * np.clip((TOP - fk) / 400.0, 0, 1)
        out += a * np.sin(k * phase0 + rng.uniform(0, 2 * np.pi))
    return out


def fenv(f, formants):
    """vowel envelope at f: sum of resonance peaks (freq, bandwidth, dB)"""
    e = np.zeros_like(f)
    for fc, bw, db in formants:
        e += 10 ** (db / 20) / (1 + ((f - fc) / (bw / 2)) ** 2)
    return e


A_ = [(800, 110, 0), (1150, 120, -6), (2800, 160, -26), (3500, 180, -32), (4950, 200, -48)]
O_ = [(450, 90, 0), (800, 100, -10), (2830, 150, -28), (3500, 170, -32), (4950, 200, -50)]
U_ = [(350, 80, 0), (650, 90, -16), (2700, 150, -34), (3300, 170, -40), (4950, 200, -56)]


def vowel_mix(t, dur):
    """weights of U, O, A over time: oo .. oh .. ah through the first third, then ah"""
    x = np.clip(t / (0.34 * dur), 0, 1)
    x = x * x * (3 - 2 * x)
    wu = np.clip(1 - 2 * x, 0, 1)
    wa = np.clip(2 * x - 1, 0, 1)
    wo = 1 - wu - wa
    return wu, wo, wa


def vox(rng, dur=0.92, root=57):
    n = int(dur * FS)
    t = np.arange(n) / FS
    wu, wo, wa = vowel_mix(t, dur)
    out = np.zeros(n)
    for v, dt in enumerate([-9, -4, 0, 3, 7, 11]):
        cents = dt + vib(rng, n, 5.1 + 0.35 * v % 1.1, 13, 0.25)
        f0 = hz(root + (12 if v % 3 == 2 else 0)) * 2 ** (cents / 1200)   # two of six an octave up (altos)

        kk = np.arange(1, 60)
        f_mean = kk * float(np.mean(f0))
        norm = [np.sqrt(np.sum((fenv(f_mean, V) / kk ** 1.15 * (f_mean < TOP)) ** 2)) for V in (U_, O_, A_)]

        def amp(k, fk):                              # each vowel as loud as the others (ah is quieter: its F1 is high)
            src = 1.0 / k ** 1.15
            return src * (wu * fenv(fk, U_) / norm[0] + wo * fenv(fk, O_) / norm[1] + wa * fenv(fk, A_) / norm[2])
        sh = 1 + 0.12 * smooth_noise(rng, n, 4.0)
        out += additive(f0, amp, 60, rng) * sh * (0.7 if v % 3 == 2 else 1.0)
    br = rng.standard_normal(n)                     # breath: noise through the formants
    spec = np.fft.rfft(br)
    fr = np.fft.rfftfreq(n, 1 / FS)
    br = np.fft.irfft(spec * (fenv(fr, A_) * 0.7 + fenv(fr, O_) * 0.3) * (fr < TOP), n)
    out = out / np.max(np.abs(out)) + 0.035 * br / np.std(br)
    return out, root


def glass(rng, dur=0.8, root=72):
    """synthesized straight at OUT_SR, its sustain exactly periodic over the loop (N samples): every partial and
    every beat a whole number of turns in it, so the loop needs no cross-fade (two sines cross-faded out of phase
    dip and smear)"""
    f0 = hz(root)
    m = round(f0 * 0.44)                             # the root's turns in the loop (~0.44 s)
    N = round(m * OUT_SR / f0)                       # the loop, samples (the root off by < 0.5 cent)
    T = N / OUT_SR
    n = int(dur * OUT_SR)
    ls = n - N
    t = np.arange(n) / OUT_SR
    parts = [(1.0, 1.0, 1), (2.005, 0.42, 1), (2.99, 0.30, 0), (4.02, 0.18, 1), (5.41, 0.11, 0),
             (6.79, 0.065, 1), (8.21, 0.04, 0), (10.5, 0.02, 1)]
    x = np.clip(t / (ls / OUT_SR), 0, 1)
    x = x * x * (3 - 2 * x)                          # 0 .. 1 by the loop's start, then held
    out = np.zeros(n)
    for i, (r, a, beat) in enumerate(parts):
        k = round(f0 * r * T)                        # turns in the loop
        if k / T > TOP:
            continue
        level = 1.0 - (0.15 + 0.6 * i / len(parts)) * x   # the top fades towards the loop, then holds
        for s in ((-1, 1) if beat else (0,)):
            kk = k + s                               # a beating pair: one turn apart over the loop (~2.3 Hz)
            out += (0.5 if beat else 1.0) * a * level * np.sin(2 * np.pi * kk / T * t + rng.uniform(0, 2 * np.pi))
    fi = int(0.05 * OUT_SR)
    out[:fi] *= (np.arange(fi) / fi) ** 1.5
    return out, root, ls


def pad(rng, dur=0.82, root=60):
    n = int(dur * FS)
    t = np.arange(n) / FS
    cut = 700 * (4.2 ** np.clip(t / (0.45 * dur), 0, 1) ** 1.2)      # 700 Hz .. ~2.9 kHz, then held
    out = np.zeros(n)
    for v, dt in enumerate([-21, -13, -6, 0, 6, 13, 21]):
        cents = dt + 4 * smooth_noise(rng, n, 1.5)
        f0 = hz(root) * 2 ** (cents / 1200)

        def amp(k, fk):
            return (1.0 / k) / np.sqrt(1 + (fk / cut) ** 4)                # 12 dB / octave over the cut
        out += additive(f0, amp, 40, rng) * (0.8 if v in (0, 6) else 1.0)
    out += 0.25 * additive(np.full(n, hz(root - 12)), lambda k, fk: (k == 1) * 1.0, 1, rng)   # a little sub
    return out, root


def air(rng, dur=0.62, root=72):
    n = int(dur * FS)
    t = np.arange(n) / FS
    f0 = hz(root) * 2 ** (vib(rng, n, 4.6, 6, 0.3) / 1200)
    w = rng.standard_normal(n)
    out = np.zeros(n)
    for k, (q, g) in enumerate([(70, 1.0), (55, 0.45), (45, 0.22), (40, 0.1)], start=1):   # resonances on the harmonics
        fc = f0 * k
        if fc.max() > TOP:
            break
        y = np.zeros(n)                              # a two-pole resonator, its frequency moving with f0
        y1 = y2 = 0.0
        for i in range(n):
            th = 2 * np.pi * fc[i] / FS
            rr = 1 - th / (2 * q) * 2
            b = (1 - rr * rr) * 0.5
            yi = b * w[i] + 2 * rr * np.cos(th) * y1 - rr * rr * y2
            y2, y1 = y1, yi
            y[i] = yi
        out += g * y / np.std(y)
    breath = sosfilt(butter(2, [300, 5500], btype="band", fs=FS, output="sos"), rng.standard_normal(n))
    swell = 0.8 + 0.2 * smooth_noise(rng, n, 2.5)
    out = (out + 0.28 * breath / np.std(breath)) * swell
    return out, root


def finish(x, loop_from):
    """to OUT_SR, nothing above TOP, a 5 ms fade in, the loop [ls, le] with its seam cross-faded, -1 dBFS"""
    x = sosfiltfilt(butter(8, TOP, fs=FS, output="sos"), x)
    x = sosfiltfilt(butter(2, 40, btype="high", fs=FS, output="sos"), x)
    y = resample_poly(x, OUT_SR // 1000, FS // 1000)
    n = len(y)
    fi = int(0.005 * OUT_SR)
    y[:fi] *= np.linspace(0, 1, fi)
    ls, le = int(loop_from * n), n - 1
    L = int(0.09 * OUT_SR)
    j = np.arange(L)
    w = np.sin(0.5 * np.pi * (j + 1) / L)
    k = le - L + 1 + j
    y[k] = y[k] * np.cos(0.5 * np.pi * (j + 1) / L) + y[ls - L + j] * w   # y[le] flows into y[ls]
    y *= 0.89 * 32767 / np.max(np.abs(y))
    return np.round(y).astype(np.int16), ls, le


def main(out_dir):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    meta = []
    for name, fn, loop_from, seed in (("VOX", vox, 0.5, 11), ("GLASS", glass, None, 12), ("PAD", pad, 0.55, 13),
                                      ("AIR", air, 0.3, 14)):
        r = fn(np.random.default_rng(seed))
        if loop_from is None:                        # (already at OUT_SR, its loop exact)
            x, root, ls = r
            y = np.round(x * 0.89 * 32767 / np.max(np.abs(x))).astype(np.int16)
            le = len(y) - 1
        else:
            x, root = r
            y, ls, le = finish(x, loop_from)
        f = out_dir / f"{name.lower()}.wav"
        with wave.open(str(f), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(OUT_SR)
            w.writeframes(y.tobytes())
        meta.append(dict(name=name, file=f.name, rate=OUT_SR, root=root, ls=ls, le=le))
        print(f"{name}: {len(y)} samples, {len(y) / OUT_SR:.2f} s, root {root}, loop {ls}..{le}")
    (out_dir / "sets.json").write_text(json.dumps(meta, indent=1) + "\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ROOT / "assets" / "clipper-sets")
