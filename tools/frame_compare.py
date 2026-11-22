#!/usr/bin/env python3
"""Compare a gcrecomp screenshot with a reference frame (for example one dumped by Dolphin).

The screenshots written with GP_SHOT_EVERY / TOS_SCREENSHOT_DIR style options are PPM files; Dolphin's
frame dumps are PNG. Both are read, resized to a common size and compared.

  python3 tools/frame_compare.py OURS REFERENCE [--out side_by_side.png] [--size 320x240] [--grid 8]

Prints the mean absolute error per channel (0-255), the RMSE, and a coarse grid of mean luminance for both
images (so a black render next to a bright reference is visible without opening the picture). With --out
it writes both images side by side, followed by an amplified difference image.
The exit status is 0 unless --max-rmse is given and the RMSE exceeds it, which makes the tool usable as a
regression check on a stored reference.

Requires Pillow and numpy.
"""
import argparse
import sys

import numpy as np
from PIL import Image


def load(path, size):
    img = Image.open(path).convert("RGB")
    return img.resize(size, Image.LANCZOS)


def luminance_grid(arr, n):
    h, w, _ = arr.shape
    lum = arr @ np.array([0.299, 0.587, 0.114])
    return [[float(lum[y * h // n:(y + 1) * h // n, x * w // n:(x + 1) * w // n].mean()) for x in range(n)] for y in range(n)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ours")
    ap.add_argument("reference")
    ap.add_argument("--out", help="write ours | reference | difference to this image")
    ap.add_argument("--size", default="320x240", help="comparison size WxH (default 320x240)")
    ap.add_argument("--grid", type=int, default=8, help="luminance grid size (0 to skip)")
    ap.add_argument("--max-rmse", type=float, help="fail (exit 1) when the RMSE is above this value")
    args = ap.parse_args()

    w, h = (int(v) for v in args.size.lower().split("x"))
    a = load(args.ours, (w, h))
    b = load(args.reference, (w, h))
    fa, fb = np.asarray(a, dtype=np.float64), np.asarray(b, dtype=np.float64)
    diff = np.abs(fa - fb)
    rmse = float(np.sqrt(((fa - fb) ** 2).mean()))
    print("mean abs error R/G/B: %.2f %.2f %.2f   RMSE: %.2f" % (*diff.mean(axis=(0, 1)), rmse))
    if args.grid:
        for name, arr in (("ours", fa), ("reference", fb)):
            print("%s luminance grid:" % name)
            for row in luminance_grid(arr, args.grid):
                print("  " + " ".join("%3d" % v for v in row))
    if args.out:
        sheet = Image.new("RGB", (w * 3, h))
        sheet.paste(a, (0, 0))
        sheet.paste(b, (w, 0))
        sheet.paste(Image.fromarray(np.clip(diff * 3, 0, 255).astype(np.uint8)), (2 * w, 0))
        sheet.save(args.out)
    if args.max_rmse is not None and rmse > args.max_rmse:
        print("RMSE above --max-rmse %.2f" % args.max_rmse, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
