#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure one WAV, or compare two (ours vs Dolphin's dump), without listening.

  tools/wav-compare.py A.wav                       per-second report of A
  tools/wav-compare.py A.wav B.wav                 report of both + comparison
  tools/wav-compare.py A.wav B.wav --align         also find the time offset of B relative to A
  tools/wav-compare.py A.wav B.wav --a-from 40 --a-to 70 --b-from 38 --b-to 68

A WAV is what GCRECOMP_DUMP_AUDIO=file.wav writes, or Dolphin's <user>/Dump/Audio/*dspdump.wav
(tools/dolphin-run.sh --dump-audio) or *dtkdump.wav. Any 16-bit PCM WAV works; the two files may have
different sample rates (32 kHz / 48 kHz). Needs numpy only.

Per file: duration, rate, non-zero sample ratio, RMS and peak per second (--step to change the bin),
share of the energy per band (0-500, 500-2k, 2k-5k, 5k-8k, 8k-12k, 12k-Nyquist Hz) over the selected
window. With two files: RMS envelope correlation (Pearson, per --step bin), band shares side by side
and their L1 distance, and with --align the offset (seconds, B later than A is positive) that
maximises the envelope correlation.
Exit status is 0 unless --require-nonzero is given and the (first) file's selected window is silent
(non-zero ratio below --min-nonzero).
"""
import argparse
import sys
import wave

import numpy as np

BANDS = [0, 500, 2000, 5000, 8000, 12000, None]


def load(path):
    with wave.open(path) as w:
        rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        if width != 2:
            sys.exit(f"{path}: only 16-bit PCM is supported")
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw[: len(raw) // (2 * ch) * 2 * ch], dtype="<i2").astype(np.float64)
    return rate, x.reshape(-1, ch)


def window(x, rate, t0, t1):
    a = int(t0 * rate)
    b = len(x) if t1 is None else int(t1 * rate)
    return x[a:b]


def per_step(x, rate, step):
    n = int(rate * step)
    out = []
    for i in range(0, len(x) - n + 1, n):
        seg = x[i : i + n]
        out.append((np.sqrt((seg**2).mean()), np.abs(seg).max(), (seg != 0).mean()))
    return np.array(out) if out else np.zeros((0, 3))


def band_shares(x, rate):
    """Energy share per band, summed over every 4096-sample frame of the mono mix."""
    m = x.mean(axis=1)
    n = 4096
    if len(m) < n:
        return None
    frames = m[: len(m) // n * n].reshape(-1, n) * np.hanning(n)
    spec = (np.abs(np.fft.rfft(frames, axis=1)) ** 2).sum(axis=0)
    f = np.fft.rfftfreq(n, 1 / rate)
    total = spec.sum()
    if total == 0:
        return np.zeros(len(BANDS) - 1)
    edges = [e if e is not None else rate / 2 + 1 for e in BANDS]
    return np.array([spec[(f >= lo) & (f < hi)].sum() / total for lo, hi in zip(edges, edges[1:])])


def band_names(rate):
    edges = [e if e is not None else int(rate / 2) for e in BANDS]
    return [f"{lo}-{hi}" for lo, hi in zip(edges, edges[1:])]


def report(label, rate, x, t0, t1, step, quiet):
    print(f"== {label}: {len(x) / rate:.1f} s at {rate} Hz, {x.shape[1]} ch (window {t0}..{t1 if t1 is not None else 'end'} s)")
    w = window(x, rate, t0, t1)
    if len(w) == 0:
        print("   empty window")
        return None, None
    nz = float((w != 0).mean())
    print(f"   non-zero samples {nz * 100:.2f} %, RMS {np.sqrt((w**2).mean()):.1f}, peak {int(np.abs(w).max())}")
    bins = per_step(w, rate, step)
    if not quiet:
        for i, (rms, pk, nzr) in enumerate(bins):
            print(f"   {t0 + i * step:7.1f}s rms {rms:8.1f} peak {int(pk):6d} nonzero {nzr * 100:6.2f} %")
    sh = band_shares(w, rate)
    if sh is not None:
        print("   band shares (Hz): " + "  ".join(f"{n}:{s * 100:.1f}%" for n, s in zip(band_names(rate), sh)))
    return nz, (bins[:, 0] if len(bins) else None, sh)


def corr(a, b):
    n = min(len(a), len(b))
    if n < 3 or a[:n].std() == 0 or b[:n].std() == 0:
        return float("nan")
    return float(np.corrcoef(a[:n], b[:n])[0, 1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b", nargs="?")
    ap.add_argument("--a-from", type=float, default=0.0)
    ap.add_argument("--a-to", type=float)
    ap.add_argument("--b-from", type=float, default=0.0)
    ap.add_argument("--b-to", type=float)
    ap.add_argument("--step", type=float, default=1.0, help="bin in seconds for RMS/peak (default 1)")
    ap.add_argument("--quiet", action="store_true", help="no per-bin lines")
    ap.add_argument("--align", action="store_true", help="search the offset of B vs A on the RMS envelope (needs B)")
    ap.add_argument("--max-offset", type=float, default=60.0, help="--align search range, seconds (default 60)")
    ap.add_argument("--require-nonzero", action="store_true", help="exit 1 if A's window is silent")
    ap.add_argument("--min-nonzero", type=float, default=0.5, help="non-zero ratio below which a window is silent")
    a = ap.parse_args()

    ra, xa = load(a.a)
    nz, da = report("A " + a.a, ra, xa, a.a_from, a.a_to, a.step, a.quiet)
    status = 0
    if a.require_nonzero and (nz is None or nz < a.min_nonzero):
        print(f"A is silent (non-zero {0 if nz is None else nz * 100:.2f} % < {a.min_nonzero * 100:.0f} %)")
        status = 1
    if a.b:
        rb, xb = load(a.b)
        _, db = report("B " + a.b, rb, xb, a.b_from, a.b_to, a.step, a.quiet)
        if da is not None and db is not None and da[0] is not None and db[0] is not None:
            print(f"== A vs B: envelope correlation {corr(da[0], db[0]):.3f} (bins of {a.step} s, windows as given)")
            if da[1] is not None and db[1] is not None:
                print(f"   band-share L1 distance {np.abs(da[1] - db[1]).sum() * 100:.1f} points (0 = same spectrum, 200 = disjoint)")
            ra_rms = np.sqrt((window(xa, ra, a.a_from, a.a_to) ** 2).mean())
            rb_rms = np.sqrt((window(xb, rb, a.b_from, a.b_to) ** 2).mean())
            if rb_rms > 0:
                print(f"   RMS ratio A/B {ra_rms / rb_rms:.3f}")
        if a.align:
            ea = per_step(xa, ra, a.step)[:, 0]
            eb = per_step(xb, rb, a.step)[:, 0]
            best = (float("-inf"), 0)
            lim = int(a.max_offset / a.step)
            for k in range(-lim, lim + 1):
                # B's bin j matches A's bin j-k
                if k >= 0:
                    p, q = ea[: len(eb) - k] if len(eb) - k > 0 else ea[:0], eb[k:]
                else:
                    p, q = ea[-k:], eb[: len(ea) + k] if len(ea) + k > 0 else eb[:0]
                n = min(len(p), len(q))
                if n < 10:
                    continue
                c = corr(p[:n], q[:n])
                if c == c and c > best[0]:
                    best = (c, k)
            print(f"== alignment: B is {best[1] * a.step:+.1f} s later than A, envelope correlation {best[0]:.3f}")
    return status


if __name__ == "__main__":
    sys.exit(main())
