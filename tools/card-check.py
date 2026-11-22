#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent validator and lister of a raw GameCube memory card image (as GCRECOMP_CARD_IMAGE writes).

    tools/card-check.py CARD.raw [--files] [--extract NAME OUT]

Written from the public format documentation (YAGCD, "Memory Card Format"), not from src/os/card_image.cpp,
so it can catch a bug of the emulation. Checks the size, the header checksum, both directory copies and both
block-map copies (checksums, update counters), then every file: chains stay in range, have `block_count`
blocks, cross no other file, and the free-block count matches the map. Exit status 0 = valid, 1 = invalid,
2 = usage / unreadable.
"""
import argparse
import struct
import sys

BLOCK = 0x2000


def checksum(data):
    """Sum and sum of complements of the big-endian 16-bit words; 0xFFFF is stored as 0."""
    s = i = 0
    for (w,) in struct.iter_unpack(">H", data):
        s = (s + w) & 0xFFFF
        i = (i + (w ^ 0xFFFF)) & 0xFFFF
    return (0 if s == 0xFFFF else s), (0 if i == 0xFFFF else i)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--files", action="store_true", help="list the directory")
    ap.add_argument("--extract", nargs=2, metavar=("NAME", "OUT"), help="write the data blocks of file NAME to OUT")
    a = ap.parse_args()
    try:
        img = open(a.image, "rb").read()
    except OSError as e:
        print(f"card-check: {e}", file=sys.stderr)
        return 2
    errors = []

    def bad(msg):
        errors.append(msg)
        print("INVALID:", msg)

    if len(img) not in (0x80000, 0x100000, 0x200000, 0x400000, 0x800000, 0x1000000):
        bad(f"size {len(img):#x} is not a card size")
        return 1
    total = len(img) // BLOCK
    if img[:BLOCK] == b"\xff" * BLOCK:
        print("blank (unformatted) card")
        return 1
    hdr = img[:BLOCK]
    s, i = checksum(hdr[:0x1FC])
    if (s, i) != struct.unpack(">HH", hdr[0x1FC:0x200]):
        bad("header checksum")
    size_mbit, enc = struct.unpack(">HH", hdr[0x22:0x26])
    if size_mbit * 0x20000 != len(img):
        bad(f"header says {size_mbit} Mbit, image is {len(img) // 0x20000} Mbit")
    print(f"header ok: {size_mbit} Mbit, encoding {enc} ({'Shift-JIS' if enc else 'ANSI'}), {total - 5} usable blocks")

    def pick(first, kind, ck_off, ck_range, ctr_off):
        best = None
        for n, b in enumerate((first, first + 1)):
            blk = img[b * BLOCK:(b + 1) * BLOCK]
            ok = checksum(blk[ck_range[0]:ck_range[1]]) == struct.unpack(">HH", blk[ck_off:ck_off + 4])
            ctr = struct.unpack(">H", blk[ctr_off:ctr_off + 2])[0]
            print(f"  {kind} copy {n}: checksum {'ok' if ok else 'BAD'}, update counter {ctr}")
            if ok and (best is None or ((ctr - best[0]) & 0xFFFF) < 0x8000 and ctr != best[0]):
                best = (ctr, blk)
        if best is None:
            bad(f"no valid {kind} copy")
        return best[1] if best else None

    # Directory: 127 entries of 0x40 bytes, update counter at 0x1FFA, checksum at 0x1FFC.
    d = pick(1, "directory", 0x1FFC, (0, 0x1FFC), 0x1FFA)
    # Block map: checksum at 0, over bytes 4..0x1FFF; counter at 4, free count at 6, last allocated at 8.
    m = pick(3, "block map", 0, (4, BLOCK), 4)
    if d is None or m is None:
        return 1
    free, last = struct.unpack(">HH", m[6:10])
    nxt = lambda blk: struct.unpack(">H", m[10 + 2 * (blk - 5):12 + 2 * (blk - 5)])[0]  # noqa: E731
    owner = {}
    files = []
    for n in range(127):
        e = d[n * 0x40:(n + 1) * 0x40]
        if e[0] == 0xFF:
            continue
        game, maker = e[0:4], e[4:6]
        name = e[8:40].split(b"\0")[0].decode("latin-1")
        first, count = struct.unpack(">HH", e[0x36:0x3A])
        files.append((n, game, maker, name, first, count))
        blk, chain = first, []
        while blk != 0xFFFF and len(chain) <= total:
            if not 5 <= blk < total:
                bad(f"file {name!r}: block {blk} out of range")
                break
            if blk in owner:
                bad(f"file {name!r}: block {blk} also belongs to {owner[blk]!r}")
                break
            owner[blk] = name
            chain.append(blk)
            blk = nxt(blk)
        else:
            if blk != 0xFFFF:
                bad(f"file {name!r}: chain does not end")
        if len(chain) != count:
            bad(f"file {name!r}: directory says {count} blocks, chain has {len(chain)}")
    real_free = sum(1 for b in range(5, total) if nxt(b) == 0 and b not in owner)
    if real_free != free:
        bad(f"free block count says {free}, the map has {real_free} free blocks")
    for b in range(5, total):
        if nxt(b) != 0 and b not in owner:
            bad(f"block {b} is allocated in the map but belongs to no file")
    print(f"{len(files)} file(s), {free} free block(s)")
    if a.files or a.extract:
        for n, game, maker, name, first, count in files:
            if a.files:
                print(f"  #{n} {game.decode('latin-1')}{maker.decode('latin-1')} {name!r} first={first} blocks={count}")
            if a.extract and name == a.extract[0]:
                blk, out = first, bytearray()
                while blk != 0xFFFF:
                    out += img[blk * BLOCK:(blk + 1) * BLOCK]
                    blk = nxt(blk)
                open(a.extract[1], "wb").write(out)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
