#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Per-channel view of a stereo WAV: is the right channel silent, is the file really mono, do the channels differ?

  tools/wav-channels.py A.wav [B.wav ...] [--from S --to S] [--step S]

For each file (a GCRECOMP_DUMP_AUDIO=file.wav dump, or Dolphin's *dspdump.wav / *dtkdump.wav): per --step seconds
bin (default 10) the RMS of the left and right channel, the RMS of L-R, and the share of samples where L == R;
then the totals of the selected window. A "mono" verdict means L == R for more than 99.9% of the samples, a
"left only" / "right only" verdict that the other channel's RMS is below 0.1% of the first's. Needs numpy only.
Exit status 0 always (it is a measurement; read the verdict).
"""
import argparse
import sys
import wave

import numpy as np


def load(path):
    with wave.open(path) as w:
        rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        if width != 2 or ch != 2:
            sys.exit(f"{path}: need a 16-bit stereo WAV (got {ch} ch, {width * 8} bit)")
        raw = w.readframes(w.getnframes())
    return rate, np.frombuffer(raw[: len(raw) // 4 * 4], dtype="<i2").astype(np.float64).reshape(-1, 2)


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def verdict(left, right):
    l, r = rms(left), rms(right)
    if l == 0 and r == 0:
        return "silent"
    if len(left) and np.mean(left == right) > 0.999:
        return "mono (L == R)"
    if r < 0.001 * l:
        return "left only"
    if l < 0.001 * r:
        return "right only"
    return "stereo"


def report(path, args):
    rate, x = load(path)
    t0 = int((args.t_from or 0) * rate)
    t1 = int(args.t_to * rate) if args.t_to is not None else len(x)
    x = x[t0:t1]
    print(f"== {path}: {len(x) / rate:.1f} s at {rate} Hz (window {args.t_from or 0}..{args.t_to if args.t_to is not None else 'end'} s)")
    step = max(1, int(args.step * rate))
    print(f"{'t (s)':>8} {'L rms':>9} {'R rms':>9} {'L-R rms':>9} {'L==R':>7}  verdict")
    for i in range(0, len(x), step):
        b = x[i : i + step]
        if len(b) < step // 2:
            break
        print(f"{(t0 + i) / rate:8.1f} {rms(b[:, 0]):9.1f} {rms(b[:, 1]):9.1f} {rms(b[:, 0] - b[:, 1]):9.1f} "
              f"{np.mean(b[:, 0] == b[:, 1]) * 100:6.1f}%  {verdict(b[:, 0], b[:, 1])}")
    print(f"{'total':>8} {rms(x[:, 0]):9.1f} {rms(x[:, 1]):9.1f} {rms(x[:, 0] - x[:, 1]):9.1f} "
          f"{np.mean(x[:, 0] == x[:, 1]) * 100:6.1f}%  {verdict(x[:, 0], x[:, 1])}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav", nargs="+")
    ap.add_argument("--from", dest="t_from", type=float, help="window start, seconds")
    ap.add_argument("--to", dest="t_to", type=float, help="window end, seconds")
    ap.add_argument("--step", type=float, default=10.0, help="bin length in seconds (default 10)")
    args = ap.parse_args()
    for p in args.wav:
        report(p, args)


if __name__ == "__main__":
    main()
