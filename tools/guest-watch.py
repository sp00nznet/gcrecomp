#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Find which guest function writes a guest memory address: GCRECOMP_WATCH + symbolisation of its call chain.

  GCRECOMP_WATCH=0x80123400:64 ./game ... 2> run.log
  tools/guest-watch.py --bin ./game run.log [--summary]

GCRECOMP_WATCH=0xADDR:LEN[,0xADDR:LEN...] (up to 8 ranges; cached and uncached addresses are the same RAM) makes the
runtime print, for each of the first GCRECOMP_WATCH_MAX (default 64) stores that overlap a range:

  [watch] #N write32 0x80123400 = 0x1234 bt: 8A1C3 8A0F0 ...

where the numbers are the host call chain as offsets in the executable. This tool replaces them with the nearest
symbol of the binary (`nm`): for a recompiled game these are the guest functions, func_80123456+0x1c, innermost
first. The binary needs its symbols (no `strip`). --summary prints one line per distinct innermost function with
its hit count instead of every line. The `[bt] #N bt: ...` lines of `GP_TRACE_BT` (the call chain of a traced draw) are
read the same way. Reads stdin when no log file is given.
"""
import argparse
import bisect
import re
import subprocess
import sys
from collections import Counter


def load_base(binary):
    """Virtual address of the first PT_LOAD segment: the offsets in the log are relative to it."""
    out = subprocess.run(["readelf", "-lW", binary], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0] == "LOAD":
            return int(parts[2], 16)
    return 0


def load_symbols(binary):
    out = subprocess.run(["nm", "-C", "--defined-only", "-n", binary], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "TtWw":
            addrs.append(int(parts[0], 16))
            names.append(parts[2].split("(")[0])  # no argument list: func_80123456, not the full signature
    return addrs, names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="stderr of the run (default: stdin)")
    ap.add_argument("--bin", required=True, help="the executable that wrote the log")
    ap.add_argument("--summary", action="store_true", help="count hits per innermost function")
    ap.add_argument("--depth", type=int, default=4, help="frames shown per hit (default 4)")
    args = ap.parse_args()
    addrs, names = load_symbols(args.bin)
    base = load_base(args.bin)

    def sym(off):
        off += base  # the runtime prints offsets from the load address of the executable
        i = bisect.bisect_right(addrs, off) - 1
        return f"{names[i]}+0x{off - addrs[i]:x}" if i >= 0 else f"0x{off:x}"

    pat = re.compile(r"^(\[watch\] #\d+ write(\d+) (0x[0-9A-Fa-f]+) = 0x[0-9A-Fa-f]+|\[bt\](?: #\d+)?()()) bt: (.*)$")
    counts = Counter()
    for line in open(args.log) if args.log else sys.stdin:
        m = pat.match(line.rstrip("\n"))
        if not m:
            continue
        frames = [sym(int(x, 16)) for x in m.group(6).split()]
        frames = [f for f in frames if "Memory::write" not in f and "Memory::watch" not in f][: args.depth]
        if args.summary:
            # the access width and the address modulo 4 tell the left half-word (+0) from the right one (+2)
            shape = f"write{m.group(2)} at +{int(m.group(3), 16) % 4}" if m.group(2) else "draw"
            counts[(frames[0] if frames else "?", shape)] += 1
        else:
            print(m.group(1), "<-", " <- ".join(frames))
    for (name, shape), n in counts.most_common():
        print(f"{n:8d}  {name}  {shape}")


if __name__ == "__main__":
    main()
