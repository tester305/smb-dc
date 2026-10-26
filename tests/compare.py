#!/usr/bin/env python3
"""Score the APU under test against blargg's Nes_Apu on the same SMB register traffic.
usage: compare.py DIR     (DIR holds prod_*.raw from apu_test and ref_*.raw from gme_ref)

Waveform SNR would be meaningless (a pulse wave shifted by a fraction of a cycle is the same sound),
so every metric is phase-insensitive:
  envelope corr    correlation of 10 ms loudness envelopes (note on/off + decay timing); 1.0 = same
  on/off mismatch  % of 46 ms frames where one APU is audible and the other is silent (note timing)
  spectral dist    RMS dB difference of magnitude spectra over the bins that are audible in the
                   reference, after matching overall level (0 dB = identical, < 3 dB very close)
  HF vs ref        energy above 6 kHz relative to the (flat-treble) reference, dB (+ = harsher)
Loudness and channel balance are NOT scored here (the reference's mixer is its own approximation);
the spec tests check every channel's level against the exact NES mixer formulas instead.
Noise is compared per 1/3-octave band because individual FFT bins of noise are random.
"""
import sys
import numpy as np

SR = 44100
QUIET, LOUD = 30.0, 150.0

def load(p):
    return np.fromfile(p, dtype=np.int16).astype(np.float64)

def envelope(x, win=441):
    n = len(x) // win
    return np.sqrt((x[:n * win].reshape(n, win) ** 2).mean(axis=1))

def frames(p, r, n, hop):
    for i in range(0, min(len(p), len(r)) - n, hop):
        yield p[i:i + n], r[i:i + n]

def spectral(p, r, bands, n=2048, hop=1024):
    if bands:
        n, hop = 8192, 4096
    w = np.hanning(n)
    lo, hi = int(50 * n / SR), int(10000 * n / SR)
    dist, mismatch, active = [], 0, 0
    for fp, fr in frames(p, r, n, hop):
        rp, rr = np.sqrt((fp ** 2).mean()), np.sqrt((fr ** 2).mean())
        if rp < QUIET and rr < QUIET:
            continue
        active += 1
        if (rp < QUIET and rr > LOUD) or (rr < QUIET and rp > LOUD):
            mismatch += 1
            continue
        sp = np.abs(np.fft.rfft(fp * w)) + 1.0
        sr = np.abs(np.fft.rfft(fr * w)) + 1.0
        if bands:
            edges = np.unique(np.geomspace(lo, hi, 25).astype(int))
            a = np.array([np.sqrt((sp[x:y] ** 2).mean()) for x, y in zip(edges[:-1], edges[1:])])
            b = np.array([np.sqrt((sr[x:y] ** 2).mean()) for x, y in zip(edges[:-1], edges[1:])])
        else:
            a, b = sp[lo:hi], sr[lo:hi]
        loud = b > b.max() * 10 ** (-45 / 20)
        if loud.sum() < 3:
            continue
        dist.append(np.sqrt(np.mean((20 * np.log10(a[loud] / b[loud])) ** 2)))
    return (float(np.mean(dist)) if dist else float('nan')), 100.0 * mismatch / max(active, 1)

def hf_excess(p, r, f0=6000):
    fp = np.abs(np.fft.rfft(p)) ** 2
    fr = np.abs(np.fft.rfft(r)) ** 2
    k = int(f0 * len(p) / SR)
    return 10 * np.log10((fp[k:].sum() / fp.sum()) / (fr[k:].sum() / fr.sum() + 1e-30) + 1e-30)

def main(d):
    skip = 3 * SR                                      # title screen / startup
    print(f"{'channel':16s} {'envelope corr':>13s} {'on/off mismatch':>16s} {'spectral dist (dB)':>19s} {'HF vs ref (dB)':>15s}")
    for name, label in (('pulse', 'pulses'), ('tri', 'triangle'), ('noise', 'noise')):
        p = load(f'{d}/prod_{name}.raw')[skip:]
        r = load(f'{d}/ref_{name}.raw')[skip:]
        n = min(len(p), len(r)); p, r = p[:n], r[:n]
        gain = np.sqrt((r ** 2).mean()) / np.sqrt((p ** 2).mean())
        corr = float(np.corrcoef(envelope(p), envelope(r))[0, 1])
        sd, mm = spectral(p * gain, r, bands=(name == 'noise'))
        print(f"{label:16s} {corr:13.3f} {mm:15.1f}% {sd:19.2f} {hf_excess(p, r):+15.1f}")

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
