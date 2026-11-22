#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run one guest function of a DOL on a reference PowerPC CPU (Unicorn) with a captured state.

The ppc-diff vectors test instructions one at a time; this replays a whole function, to check what the recompiled
build computed against what the real code does:

  1. capture the inputs and outputs of one call of the recompiled function (tools/guest-gdb.sh, or
     GCRECOMP_WATCH / `dump binary memory` in gdb: the registers on entry and the memory the function reads and writes),
  2. replay the call here and compare the written memory with the capture.

  tools/ppc-diff/replay.py --dol main.dol --func 0x8017AD3C \\
      --reg r3=0x81227668 --reg r4=0x81236724 --reg r6=2 \\
      --mem 0x81227668=in_state.bin --mem 0x81236724=in_data.bin --mem 0x80366814=m.bin \\
      --out 0x8122BCC0:4256=replayed.bin [--expect 0x8122BCC0=out.bin]

--dol           the executable (its sections are loaded at their addresses, big-endian).
--func          entry address; the function returns (blr) to a sentinel address.
--reg rN=V      initial register (r0..r31, lr, ctr); r1 defaults to a scratch stack.
--mem A=FILE    load a file into guest memory at A (repeatable); guest RAM is 0x80000000.. (32 MB).
--out A:LEN=F   after the call, write LEN bytes at A to F (repeatable); with --expect A=FILE the replayed bytes are
                compared with FILE and the exit status is 1 on a difference.
Needs `unicorn` (tools/ppc-diff/requirements.txt; tools/ppc-diff/run.sh creates a virtual environment with it).
"""
import argparse
import struct
import sys

from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN
from unicorn import ppc_const as pc

RAM_BASE, RAM_SIZE = 0x80000000, 0x2000000
STACK_BASE, STACK_SIZE = 0x7F000000, 0x10000
RETURN = STACK_BASE  # a `blr` lives here


def number(text):
    return int(text, 0)


def register_id(name):
    name = name.lower()
    if name == "lr":
        return pc.UC_PPC_REG_LR
    if name == "ctr":
        return pc.UC_PPC_REG_CTR
    if name.startswith("r") and name[1:].isdigit() and 0 <= int(name[1:]) <= 31:
        return getattr(pc, "UC_PPC_REG_%d" % int(name[1:]))
    sys.exit("unknown register " + name)


def load_dol(uc, path):
    data = open(path, "rb").read()
    header = struct.unpack(">18I18I18I", data[:0xD8])
    for off, addr, size in zip(header[0:18], header[18:36], header[36:54]):
        if size:
            uc.mem_write(addr, data[off:off + size])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--func", type=number, required=True)
    ap.add_argument("--reg", action="append", default=[], metavar="rN=V")
    ap.add_argument("--mem", action="append", default=[], metavar="A=FILE")
    ap.add_argument("--out", action="append", default=[], metavar="A:LEN=FILE")
    ap.add_argument("--expect", action="append", default=[], metavar="A=FILE")
    args = ap.parse_args()

    uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
    uc.mem_map(RAM_BASE, RAM_SIZE)
    uc.mem_map(STACK_BASE, STACK_SIZE)
    load_dol(uc, args.dol)
    for item in args.mem:
        addr, path = item.split("=", 1)
        uc.mem_write(number(addr), open(path, "rb").read())
    uc.mem_write(RETURN, bytes.fromhex("4e800020"))  # blr
    uc.reg_write(pc.UC_PPC_REG_1, STACK_BASE + STACK_SIZE - 0x100)
    uc.reg_write(pc.UC_PPC_REG_LR, RETURN)
    for item in args.reg:
        name, value = item.split("=", 1)
        uc.reg_write(register_id(name), number(value))
    uc.emu_start(args.func, RETURN)

    expected = {number(a): open(f, "rb").read() for a, f in (e.split("=", 1) for e in args.expect)}
    status = 0
    for item in args.out:
        span, path = item.split("=", 1)
        addr, length = (number(x) for x in span.split(":"))
        got = bytes(uc.mem_read(addr, length))
        open(path, "wb").write(got)
        if addr in expected:
            want = expected[addr][:length]
            if got == want:
                print("0x%08X +%d: identical" % (addr, length))
            else:
                first = next(i for i in range(min(len(got), len(want))) if got[i] != want[i])
                print("0x%08X +%d: DIFFERS from the capture, first at +0x%X (%02X, expected %02X)" %
                      (addr, length, first, got[first], want[first]))
                status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
