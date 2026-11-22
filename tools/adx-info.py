#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the header of a CRI ADX file and check that every channel really carries data.

  tools/adx-info.py FILE.adx [--offset N]

Prints encoding, block size, bit depth, channel count, sample rate, total samples and loop flag, then walks the
interleaved blocks (block-size bytes per channel, channels alternating) and prints, per channel, the number of blocks,
the share that is all zero (a silent or missing channel) and the mean of the 16-bit block scales. Use --offset when
the ADX data is embedded in a container (offset of the 0x80 0x00 magic). Exit status 1 when the file is not ADX.
Only the standard library is needed.
"""
import argparse
import struct
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0, help="offset of the ADX magic in the file")
    args = ap.parse_args()
    data = open(args.file, "rb").read()[args.offset:]
    if data[:2] != b"\x80\x00":
        sys.exit("not an ADX file (no 0x8000 magic at the offset)")
    data_off = struct.unpack(">H", data[2:4])[0] + 4
    enc, bsize, depth, chans = data[4], data[5], data[6], data[7]
    rate, total = struct.unpack(">II", data[8:16])
    print(f"encoding {enc}  block {bsize} B  depth {depth}  channels {chans}  {rate} Hz  {total} samples "
          f"({total / rate:.1f} s)  data at 0x{data_off:X}")
    stats = [[0, 0, 0] for _ in range(chans)]
    pos = data_off
    frame = bsize * chans
    while pos + frame <= len(data):
        for c in range(chans):
            blk = data[pos + c * bsize : pos + (c + 1) * bsize]
            s = stats[c]
            s[0] += 1
            s[1] += not any(blk)
            s[2] += struct.unpack(">H", blk[:2])[0]
        pos += frame
    for c, (n, z, scale) in enumerate(stats):
        print(f"channel {c}: {n} blocks, {100.0 * z / max(n, 1):.1f}% all zero, mean scale {scale / max(n, 1):.0f}")


if __name__ == "__main__":
    main()
