#!/usr/bin/env python3
"""Generate instruction test vectors for the ppc-diff differential test of the recompiler.

Integer, branch-condition and standard FP programs are executed on Unicorn (QEMU, PowerPC 750CL): an oracle
that shares no code with this project. Gekko-only instructions (paired singles, quantized load/store) cannot
run there, so their expected results come from a small independent model below.

Ported from fepor-recomp/tools/gen_vectors.py (the same owner's project, GPL-2.0-or-later); the output format
is text instead of binary. Deterministic for a given --seed and --scale. See tools/ppc-diff/README.md.

Output: a header comment, then one line per case:
    case NAME FLAGS WORDS | IN | OUT
FLAGS is 1 when the ps1 lanes are compared, WORDS the comma-separated hex instruction words (the last is `blr`,
not executed), IN and OUT space-separated KIND.INDEX=HEXVALUE entries. Kinds: 0 GPR, 1 FPR ps0, 2 FPR ps1, 3 CR,
5 LR, 6 CTR, 8 memory word (index into the window of MEM_WORDS words at 0x80002000). XER and FPSCR are not
stored: programs set them with instructions and read XER back with `mfxer r0` (see add_unicorn_case).
Everything not listed in IN starts as zero; OUT lists every value that differs from the start.
"""
import argparse
import math
import random
import struct
import sys

from unicorn import *
from unicorn.ppc_const import *

MEM = 0x80002000
MEM_WORDS = 32
CODE = 0x80003100
BLR = 0x4E800020

GPR_REGS = list(range(3, 13))
FPR_REGS = list(range(1, 13))
BASE, INDEX = 30, 29


# ---- encoders --------------------------------------------------------------------------------
def D(op, rt, ra, imm):
    return op << 26 | rt << 21 | ra << 16 | (imm & 0xFFFF)


def X(op, rt, ra, rb, xo, rc=0):
    return op << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1 | rc


def XO(rt, ra, rb, oe, xo, rc):
    return 31 << 26 | rt << 21 | ra << 16 | rb << 11 | oe << 10 | xo << 1 | rc


def M(op, rs, ra, sh, mb, me, rc):
    return op << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1 | rc


def A(op, frt, fra, frb, frc, xo, rc):
    return op << 26 | frt << 21 | fra << 16 | frb << 11 | frc << 6 | xo << 1 | rc


def spr_field(n):
    return ((n & 0x1F) << 5) | (n >> 5)


# ---- value generation ------------------------------------------------------------------------
def f32(x):
    try:
        return struct.unpack(">f", struct.pack(">f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def bits(d):
    return struct.unpack(">Q", struct.pack(">d", d))[0]


def dbl(b):
    return struct.unpack(">d", struct.pack(">Q", b))[0]


def rand_u32(rng):
    c = rng.random()
    if c < 0.35:
        return rng.choice([0, 1, 2, 3, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0x0000FFFF, 0xFFFF0000, 31, 32, 33, 63, 64])
    if c < 0.5:
        return rng.randrange(0, 64)
    return rng.getrandbits(32)


def rand_f64(rng, finite_only=False, single=False):
    c = rng.random()
    if c < 0.35:
        v = rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.0, 1e-3, 1e10, -1e10, 12345.678, 2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0, 4294967296.0, 1.5, 2.5, -0.5, -1.5, 0.49999999999999994])
    elif c < 0.45 and not finite_only:
        v = rng.choice([math.inf, -math.inf])
    elif c < 0.55:
        v = rng.choice([5e-324, 2.2250738585072014e-308, 1e-310, 1e-45, 1.1754943508222875e-38, 1e300, -1e300, 3.4028234663852886e38, 1e39, 1e-40, -3e-39, 7e-42, 1.4012984643248171e-45, 2.8e-45, 5.9e-39])
    else:
        v = rng.uniform(-1, 1) * 10 ** rng.randint(-8, 12)
    if single:
        v = f32(v)
        if math.isinf(v) and finite_only:
            v = 1.0
    return v


def force25(d):
    b = bits(d)
    return dbl((b & 0xFFFFFFFFF8000000) + (b & 0x8000000) & 0xFFFFFFFFFFFFFFFF)


# ---- oracle ----------------------------------------------------------------------------------
class State:
    def __init__(self):
        self.gpr = [0] * 32
        self.fpr0 = [0] * 32
        self.fpr1 = [0] * 32
        self.cr = 0
        self.xer = 0
        self.lr = 0
        self.ctr = 0
        self.fpscr = 0
        self.rn = None
        self.mem = [0] * MEM_WORDS

    def copy(self):
        s = State()
        s.gpr, s.fpr0, s.fpr1, s.mem = self.gpr[:], self.fpr0[:], self.fpr1[:], self.mem[:]
        s.cr, s.xer, s.lr, s.ctr, s.fpscr, s.rn = self.cr, self.xer, self.lr, self.ctr, self.fpscr, self.rn
        return s


def run_unicorn(words, st):
    uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
    uc.ctl_set_cpu_model(UC_CPU_PPC32_750CL_V2_0)
    uc.mem_map(0x80003000, 0x1000)
    uc.mem_map(MEM & ~0xFFF, 0x1000)
    uc.reg_write(UC_PPC_REG_MSR, 0x2000)
    for i in range(32):
        uc.reg_write(UC_PPC_REG_0 + i, st.gpr[i])
        uc.reg_write(UC_PPC_REG_FPR0 + i, st.fpr0[i])
    uc.reg_write(UC_PPC_REG_CR, st.cr)
    uc.reg_write(UC_PPC_REG_LR, st.lr)
    uc.reg_write(UC_PPC_REG_CTR, st.ctr)
    uc.mem_write(MEM, b"".join(struct.pack(">I", w) for w in st.mem))
    uc.mem_write(CODE, b"".join(struct.pack(">I", w) for w in words))
    uc.emu_start(CODE, CODE + 4 * (len(words) - 1))
    out = State()
    for i in range(32):
        out.gpr[i] = uc.reg_read(UC_PPC_REG_0 + i)
        out.fpr0[i] = uc.reg_read(UC_PPC_REG_FPR0 + i)
    out.cr = uc.reg_read(UC_PPC_REG_CR)
    out.lr = uc.reg_read(UC_PPC_REG_LR)
    out.ctr = uc.reg_read(UC_PPC_REG_CTR)
    raw = bytes(uc.mem_read(MEM, 4 * MEM_WORDS))
    out.mem = list(struct.unpack(">%dI" % MEM_WORDS, raw))
    return out


# ---- case construction -----------------------------------------------------------------------
def random_state(rng, fp_single=False, mem=False):
    st = State()
    for r in GPR_REGS:
        st.gpr[r] = rand_u32(rng)
    for r in FPR_REGS:
        st.fpr0[r] = bits(rand_f64(rng, single=fp_single))
    st.cr = rng.getrandbits(32)
    st.xer = rng.choice([0, 1, 2, 3]) << 29 | (rng.getrandbits(1) << 31)
    st.xer = (rng.getrandbits(3) << 29)
    st.lr = 0
    st.ctr = rand_u32(rng)
    st.fpscr = 0
    if mem:
        st.gpr[BASE] = MEM + 0x40
        st.gpr[INDEX] = rng.randrange(0, 0x30, 4)
        st.mem = [rng.getrandbits(32) if rng.random() < 0.7 else rng.choice([0, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF]) for _ in range(MEM_WORDS)]
    return st


class Case:
    def __init__(self, words, st_in, st_out, ps1=False, name="?"):
        self.name = name
        self.words, self.st_in, self.st_out, self.ps1 = words, st_in, st_out, ps1

    def pack(self):
        def entries(st, ref=None):
            e = []

            def add(kind, idx, v, rv):
                if ref is None or v != rv:
                    e.append((kind, idx, v))

            z = State() if ref is None else ref
            for i in range(32):
                add(0, i, st.gpr[i], z.gpr[i])
                add(1, i, st.fpr0[i], z.fpr0[i])
                if self.ps1:
                    add(2, i, st.fpr1[i], z.fpr1[i])
            add(3, 0, st.cr, z.cr)
            add(5, 0, st.lr, z.lr)
            add(6, 0, st.ctr, z.ctr)
            for i in range(MEM_WORDS):
                add(8, i, st.mem[i], z.mem[i])
            return e

        # inputs: everything non-zero; outputs: everything that differs from the input
        ein = [x for x in entries(self.st_in) if x[2] != 0]
        eout = entries(self.st_out, self.st_in)

        def fmt(es):
            return " ".join("%d.%d=%x" % x for x in es)

        return "case %s %d %s | %s | %s" % (self.name.replace(" ", "_"), 1 if self.ps1 else 0, ",".join("%08x" % w for w in self.words), fmt(ein), fmt(eout))


CASES = []
SKIPPED = {}


MTXER_R0 = 31 << 26 | 0 << 21 | 0 << 11 | 467 << 1  # patched below with the SPR field
MFXER_R0 = 31 << 26 | 0 << 21 | 0 << 11 | 339 << 1
MTXER_R0 |= spr_field(1) << 11
MFXER_R0 |= spr_field(1) << 11


def mcrfs(d, s_):
    return 63 << 26 | d << 23 | s_ << 18 | 64 << 1


def mtfsfi(crf, imm):
    return 63 << 26 | crf << 23 | imm << 12 | 134 << 1


def fctiw_expected(b_bits, rn, st_out_low):
    """Reference for fctiw/fctiwz. QEMU disagrees with the architecture on positive overflow (it returns
    0x80000000) and leaves the upper word undefined, so the value is computed here and cross-checked
    against QEMU for in-range inputs. Gekko stores 0xFFF80000 in the upper word, plus bit 32 for a zero
    result from a negative input."""
    b = dbl(b_bits)
    if math.isnan(b):
        low = 0x80000000
    else:
        if rn == 0:
            r = b if math.isinf(b) else round(b)
        elif rn == 1:
            r = b if math.isinf(b) else math.trunc(b)
        elif rn == 2:
            r = b if math.isinf(b) else math.ceil(b)
        else:
            r = b if math.isinf(b) else math.floor(b)
        if r >= 2147483648:
            low = 0x7FFFFFFF
        elif r < -2147483648:
            low = 0x80000000
        else:
            low = int(r) & 0xFFFFFFFF
            assert low == st_out_low, (hex(b_bits), rn, hex(low), hex(st_out_low))
    hi = 0xFFF80000 | (1 if low == 0 and (b_bits >> 63) else 0)
    return hi << 32 | low


def fix_fctiw(st, out, frd, frb, rn):
    out.fpr0[frd] = fctiw_expected(st.fpr0[frb], rn, out.fpr0[frd] & 0xFFFFFFFF)


def add_unicorn_case(rng, words, st, name, fpscr_fields=(), fix=None, ps1=None):
    """ps1 = ("fill", frd) | ("keep", None): also check the ps1 lane (Gekko: single-precision results and
    lfs fill both lanes, double-precision results and lfd leave ps1 alone). QEMU has no ps1 lane."""
    rn = st.rn
    """XER (and the FP rounding mode) are set by a prologue; XER is read back into r0 by the epilogue."""
    pro = []
    if st.xer:
        pro += [D(15, 0, 0, st.xer >> 16), MTXER_R0]
    if rn:  # round-to-nearest (0) is the FPSCR reset value; only other modes need mtfsfi
        pro += [mtfsfi(7, rn)]
    epi = [MFXER_R0] + [mcrfs(f_, f_) for f_ in fpscr_fields]
    words = pro + list(words) + epi + [BLR]
    try:
        out = run_unicorn(words, st)
    except UcError as e:
        SKIPPED[name] = SKIPPED.get(name, 0) + 1
        return
    if fix:
        fix(out)
    case = Case(words, st, out)
    if ps1:
        mode, dest = ps1
        out.fpr1 = st.fpr1[:]
        if mode == "fill":
            out.fpr1[dest] = out.fpr0[dest]
        case.ps1 = True
    case.name = name
    CASES.append(case)


def g(rng):
    return rng.choice(GPR_REGS)


def f(rng):
    return rng.choice(FPR_REGS)


def int_cases(rng, n):
    two = [("add", 266), ("addc", 10), ("adde", 138), ("subf", 40), ("subfc", 8), ("subfe", 136), ("mullw", 235), ("mulhw", 75), ("mulhwu", 11), ("divw", 491), ("divwu", 459)]
    one = [("addme", 234), ("addze", 202), ("subfme", 232), ("subfze", 200), ("neg", 104)]  # xo: subfze 200, subfme 232
    oe_ok = {"add", "subf", "mullw", "divw", "divwu", "neg", "addc", "adde", "subfc", "subfe", "addme", "addze", "subfme", "subfze"}
    for name, xo in two + one:
        for _ in range(n):
            oe = rng.getrandbits(1) if name in ("add", "subf", "mullw", "divw", "divwu", "neg") else 0
            rb = g(rng) if (name, xo) in two else 0
            ra = g(rng)
            w = XO(g(rng), ra, rb, oe, xo, rng.getrandbits(1))
            st = random_state(rng)
            if name in ("divw", "divwu"):
                # the result of a division by zero (or INT_MIN / -1) is undefined by the architecture; QEMU's
                # value is arbitrary, so such operands are not compared
                if st.gpr[rb] == 0 or (st.gpr[rb] == 0xFFFFFFFF and st.gpr[ra] == 0x80000000):
                    st.gpr[rb] = 3
                    if ra == rb:
                        st.gpr[ra] = 3
            add_unicorn_case(rng, [w], st, name)
    logic = [("and", 28), ("andc", 60), ("or", 444), ("orc", 412), ("xor", 316), ("nand", 476), ("nor", 124), ("eqv", 284), ("slw", 24), ("srw", 536), ("sraw", 792)]
    for name, xo in logic:
        for _ in range(n):
            w = X(31, g(rng), g(rng), g(rng), xo, rng.getrandbits(1))
            add_unicorn_case(rng, [w], random_state(rng), name)
    for name, xo in [("extsb", 954), ("extsh", 922), ("cntlzw", 26)]:
        for _ in range(n):
            add_unicorn_case(rng, [X(31, g(rng), g(rng), 0, xo, rng.getrandbits(1))], random_state(rng), name)
    for _ in range(n * 2):
        add_unicorn_case(rng, [X(31, g(rng), g(rng), rng.randrange(32), 824, rng.getrandbits(1))], random_state(rng), "srawi")
    imm = [("addi", 14), ("addis", 15), ("addic", 12), ("addic.", 13), ("subfic", 8), ("mulli", 7)]
    for name, op in imm:
        for _ in range(n):
            ra = rng.choice([0, 0] + GPR_REGS) if op in (14, 15) else g(rng)
            add_unicorn_case(rng, [D(op, g(rng), ra, rng.choice([0, 1, -1, 0x7FFF, -0x8000, rng.getrandbits(16)]))], random_state(rng), name)
    for name, op in [("ori", 24), ("oris", 25), ("xori", 26), ("xoris", 27), ("andi.", 28), ("andis.", 29)]:
        for _ in range(n):
            add_unicorn_case(rng, [D(op, g(rng), g(rng), rng.choice([0, 1, 0xFFFF, 0x8000, rng.getrandbits(16)]))], random_state(rng), name)
    for name, op in [("rlwinm", 21), ("rlwimi", 20)]:
        for _ in range(n * 3):
            w = M(op, g(rng), g(rng), rng.randrange(32), rng.randrange(32), rng.randrange(32), rng.getrandbits(1))
            add_unicorn_case(rng, [w], random_state(rng), name)
    for _ in range(n * 3):
        add_unicorn_case(rng, [M(23, g(rng), g(rng), g(rng), rng.randrange(32), rng.randrange(32), rng.getrandbits(1))], random_state(rng), "rlwnm")
    for name, xo in [("cmp", 0), ("cmpl", 32)]:
        for _ in range(n):
            add_unicorn_case(rng, [X(31, rng.randrange(8) << 2, g(rng), g(rng), xo)], random_state(rng), name)
    for name, op in [("cmpi", 11), ("cmpli", 10)]:
        for _ in range(n):
            add_unicorn_case(rng, [D(op, rng.randrange(8) << 2, g(rng), rng.choice([0, 1, -1, 0x7FFF, 0x8000, rng.getrandbits(16)]))], random_state(rng), name)


def cr_cases(rng, n):
    for name, xo in [("crand", 257), ("crandc", 129), ("creqv", 289), ("crnand", 225), ("crnor", 33), ("cror", 449), ("crorc", 417), ("crxor", 193)]:
        for _ in range(n):
            w = 19 << 26 | rng.randrange(32) << 21 | rng.randrange(32) << 16 | rng.randrange(32) << 11 | xo << 1
            add_unicorn_case(rng, [w], random_state(rng), name)
    for _ in range(n):
        add_unicorn_case(rng, [19 << 26 | rng.randrange(8) << 23 | rng.randrange(8) << 18], random_state(rng), "mcrf")
    for _ in range(n):
        add_unicorn_case(rng, [X(31, g(rng), 0, 0, 19)], random_state(rng), "mfcr")
    for _ in range(n * 2):
        add_unicorn_case(rng, [31 << 26 | g(rng) << 21 | rng.randrange(256) << 12 | 144 << 1], random_state(rng), "mtcrf")
    for _ in range(n):
        st = random_state(rng)
        add_unicorn_case(rng, [31 << 26 | g(rng) << 21 | spr_field(1) << 11 | 339 << 1], st, "mfxer")
        st = random_state(rng)
        rs = g(rng)
        st.gpr[rs] &= 0xE000007F  # reserved XER bits read back as written in QEMU
        add_unicorn_case(rng, [31 << 26 | rs << 21 | spr_field(1) << 11 | 467 << 1], st, "mtxer")
        add_unicorn_case(rng, [31 << 26 | g(rng) << 21 | spr_field(8) << 11 | 339 << 1], random_state(rng), "mflr")
        add_unicorn_case(rng, [31 << 26 | g(rng) << 21 | spr_field(8) << 11 | 467 << 1], random_state(rng), "mtlr")
        add_unicorn_case(rng, [31 << 26 | g(rng) << 21 | spr_field(9) << 11 | 339 << 1], random_state(rng), "mfctr")
        add_unicorn_case(rng, [31 << 26 | g(rng) << 21 | spr_field(9) << 11 | 467 << 1], random_state(rng), "mtctr")
    # branch conditions: taken -> r3 = 1, not taken -> r3 = 0. Covers every BO/BI shape and CTR decrement.
    for _ in range(n * 12):
        bo = rng.choice([0, 2, 4, 8, 10, 12, 16, 18, 20, 1, 3, 5, 9, 11, 13, 17, 19])
        bi = rng.randrange(32)
        prog = [16 << 26 | bo << 21 | bi << 16 | 12, D(14, 3, 0, 0), 18 << 26 | 8, D(14, 3, 0, 1)]
        st = random_state(rng)
        st.ctr = rng.choice([0, 1, 2, 0xFFFFFFFF, rand_u32(rng)])
        add_unicorn_case(rng, prog, st, "bc")


def mem_cases(rng, n):
    d_loads = [("lbz", 34), ("lbzu", 35), ("lhz", 40), ("lhzu", 41), ("lha", 42), ("lhau", 43), ("lwz", 32), ("lwzu", 33)]
    d_stores = [("stb", 38), ("stbu", 39), ("sth", 44), ("sthu", 45), ("stw", 36), ("stwu", 37)]
    for name, op in d_loads + d_stores:
        for _ in range(n):
            rd = rng.choice([r for r in GPR_REGS])
            d = rng.randrange(-0x30, 0x30)
            st = random_state(rng, mem=True)
            add_unicorn_case(rng, [D(op, rd, BASE, d)], st, name)
    x_loads = [("lbzx", 87), ("lbzux", 119), ("lhzx", 279), ("lhzux", 311), ("lhax", 343), ("lhaux", 375), ("lwzx", 23), ("lwzux", 55), ("lhbrx", 790), ("lwbrx", 534)]
    x_stores = [("stbx", 215), ("stbux", 247), ("sthx", 407), ("sthux", 439), ("stwx", 151), ("stwux", 183), ("sthbrx", 918), ("stwbrx", 662)]
    for name, xo in x_loads + x_stores:
        for _ in range(n):
            add_unicorn_case(rng, [X(31, g(rng), BASE, INDEX, xo)], random_state(rng, mem=True), name)
    for _ in range(n):
        add_unicorn_case(rng, [X(31, g(rng), BASE, INDEX, 20), X(31, g(rng), BASE, INDEX, 150, 1)], random_state(rng, mem=True), "lwarx+stwcx.")
        add_unicorn_case(rng, [X(31, 0, BASE, INDEX, 1014)], random_state(rng, mem=True), "dcbz")
    for _ in range(n):
        st = random_state(rng, mem=True)
        st.gpr[20] = MEM + 0x40
        rt = rng.choice([24, 26, 28, 30])
        add_unicorn_case(rng, [D(46, rt, 20, rng.randrange(-0x20, 0x10, 4))], st, "lmw")
        st = random_state(rng, mem=True)
        st.gpr[20] = MEM + 0x40
        for r in range(24, 32):
            st.gpr[r] = rand_u32(rng)
        add_unicorn_case(rng, [D(47, rng.choice([24, 26, 28, 30]), 20, rng.randrange(-0x20, 0x10, 4))], st, "stmw")


def fp_cases(rng, n):
    def fstate(single=False, nan_free=False):
        st = random_state(rng, fp_single=single)
        st.rn = 0  # arithmetic assumes round-to-nearest; only fctiw tests vary it
        for r in FPR_REGS:
            st.fpr1[r] = bits(rand_f64(rng, finite_only=True))
        for r in FPR_REGS:
            if nan_free and math.isnan(dbl(st.fpr0[r])):
                st.fpr0[r] = bits(1.0)
        return st

    # double precision (op 63): a, b, c operands. Operands for fmul/fmadd family use c freely (no force25 on doubles).
    for name, xo, kind in [("fadd", 21, "ab"), ("fsub", 20, "ab"), ("fdiv", 18, "ab"), ("fmul", 25, "ac")]:
        for _ in range(n * 2):
            fa, fb, fc = f(rng), f(rng), f(rng)
            d = f(rng)
            w = A(63, d, fa, fb if kind == "ab" else 0, fc if kind == "ac" else 0, xo, 0)
            add_unicorn_case(rng, [w], fstate(), name, ps1=("keep", None))
    for name, xo in [("fmadd", 29), ("fmsub", 28), ("fnmadd", 31), ("fnmsub", 30)]:
        for _ in range(n * 2):
            add_unicorn_case(rng, [A(63, f(rng), f(rng), f(rng), f(rng), xo, 0)], fstate(), name, ps1=("keep", None))
    for _ in range(n * 2):
        add_unicorn_case(rng, [A(63, f(rng), f(rng), f(rng), f(rng), 23, 0)], fstate(), "fsel")
    # single precision (op 59): operands exactly representable as single, so Gekko's 25-bit multiplier input rounding is a no-op
    for name, xo, kind in [("fadds", 21, "ab"), ("fsubs", 20, "ab"), ("fdivs", 18, "ab"), ("fmuls", 25, "ac")]:
        for _ in range(n * 2):
            fa, fb, fc = f(rng), f(rng), f(rng)
            st = fstate(single=True)
            if kind == "ac":
                st.fpr0[fc] = bits(f32(rng.choice([1.0, 0.5, 3.0, -2.0, 1e-3, 7.25, rng.uniform(-100, 100)])))
                st.fpr0[fc] = bits(dbl(st.fpr0[fc]) if (bits(dbl(st.fpr0[fc])) & 0xFFFFFFF) == 0 else f32(dbl(st.fpr0[fc])))
            d = f(rng)
            for r in FPR_REGS:
                st.fpr1[r] = bits(f32(dbl(st.fpr1[r])))
            add_unicorn_case(rng, [A(59, d, fa, fb if kind == "ab" else 0, fc if kind == "ac" else 0, xo, 0)], st, name, ps1=("fill", d))
    for name, xo in [("fmadds", 29), ("fmsubs", 28), ("fnmadds", 31), ("fnmsubs", 30)]:
        for _ in range(n * 2):
            fc = f(rng)
            st = fstate(single=True)
            st.fpr0[fc] = bits(f32(rng.choice([1.0, 0.5, 3.0, -2.0, 7.25, rng.uniform(-100, 100)])))
            d = f(rng)
            add_unicorn_case(rng, [A(59, d, f(rng), f(rng), fc, xo, 0)], st, name, ps1=("fill", d))
    for name, xo in [("frsp", 12), ("fctiw", 14), ("fctiwz", 15), ("fneg", 40), ("fmr", 72), ("fnabs", 136), ("fabs", 264)]:
        for _ in range(n * 2):
            st = fstate()
            if name == "fctiw":
                st.rn = rng.getrandbits(2)
            frd, frb = f(rng), f(rng)
            fix = (lambda o, st=st, frd=frd, frb=frb, name=name: fix_fctiw(st, o, frd, frb, 1 if name == "fctiwz" else st.rn)) if name in ("fctiw", "fctiwz") else None
            mode = ("fill", frd) if name == "frsp" else ("keep", None)
            add_unicorn_case(rng, [X(63, frd, 0, frb, xo)], st, name, fix=fix, ps1=mode)
    for name, xo in [("fcmpu", 0), ("fcmpo", 32)]:
        for _ in range(n * 2):
            add_unicorn_case(rng, [X(63, rng.randrange(8) << 2, f(rng), f(rng), xo)], fstate(nan_free=(xo == 32)), name)
    # FP loads and stores (ps0 only is compared)
    for name, op in [("lfs", 48), ("lfsu", 49), ("lfd", 50), ("lfdu", 51), ("stfs", 52), ("stfsu", 53), ("stfd", 54), ("stfdu", 55)]:
        for _ in range(n):
            st = random_state(rng, mem=True)
            for r in FPR_REGS:
                st.fpr1[r] = bits(f32(rand_f64(rng, finite_only=True)))
            d = f(rng)
            mode = ("fill", d) if name in ("lfs", "lfsu") else ("keep", None) if name.startswith("lf") else None
            add_unicorn_case(rng, [D(op, d, BASE, rng.randrange(-0x30, 0x30))], st, name, ps1=mode)
    for name, xo in [("lfsx", 535), ("lfsux", 567), ("lfdx", 599), ("lfdux", 631), ("stfsx", 663), ("stfsux", 695), ("stfdx", 727), ("stfdux", 759), ("stfiwx", 983)]:
        for _ in range(n):
            st = random_state(rng, mem=True)
            for r in FPR_REGS:
                st.fpr1[r] = bits(f32(rand_f64(rng, finite_only=True)))
            d = f(rng)
            mode = ("fill", d) if name in ("lfsx", "lfsux") else ("keep", None) if name.startswith("lf") else None
            add_unicorn_case(rng, [X(31, d, BASE, INDEX, xo)], st, name, ps1=mode)
    # FPSCR moves: only the enable/rounding fields 6 and 7 are deterministic; they are read back with mcrfs.
    for _ in range(n * 2):
        rd = f(rng)
        # QEMU leaves the upper word of mffs results zero; Gekko stores 0xFFF80000 there.
        def fix_mffs(o, rd=rd):
            o.fpr0[rd] = 0xFFF80000 << 32 | (o.fpr0[rd] & 0xFFFFFFFF)

        add_unicorn_case(rng, [X(63, rd, 0, 0, 583)], fstate(), "mffs", fpscr_fields=(6, 7), fix=fix_mffs)
        st = fstate()
        r = f(rng)
        st.fpr0[r] = rng.getrandbits(8)
        add_unicorn_case(rng, [63 << 26 | rng.choice([1, 2, 3]) << 17 | r << 11 | 711 << 1], st, "mtfsf", fpscr_fields=(6, 7))
        add_unicorn_case(rng, [mtfsfi(rng.choice([6, 7]), rng.randrange(16))], fstate(), "mtfsfi", fpscr_fields=(6, 7))
        add_unicorn_case(rng, [X(63, rng.randrange(24, 32), 0, 0, 70)], fstate(), "mtfsb0", fpscr_fields=(6, 7))
        add_unicorn_case(rng, [X(63, rng.choice([24, 25, 26, 27, 28, 30, 31]), 0, 0, 38)], fstate(), "mtfsb1", fpscr_fields=(6, 7))
    for _ in range(n):
        for name, xo in [("mcrfs", 64)]:
            add_unicorn_case(rng, [mcrfs(rng.randrange(8), rng.choice([6, 7]))], fstate(), name)


# ---- paired-single model (no oracle exists for these on Unicorn) -----------------------------
def ps_arith(op, a, b, c):
    """Lane-wise reference for the two-lane ops. a, b, c are single-valued doubles."""
    def lane(x, y, z):
        c25 = force25(z)
        if op == "add":
            return f32(x + y)
        if op == "sub":
            return f32(x - y)
        if op == "mul":
            return f32(x * c25)
        if op == "div":
            return f32(x / y) if y != 0 else (math.nan if x == 0 else math.copysign(math.inf, x) * math.copysign(1.0, y))
        if op == "madd":
            return f32(x * c25 + y)
        if op == "msub":
            return f32(x * c25 - y)
        if op == "nmadd":
            return -f32(x * c25 + y)
        if op == "nmsub":
            return -f32(x * c25 - y)
        raise KeyError(op)

    return [lane(a[0], b[0], c[0]), lane(a[1], b[1], c[1])]


def ps_cases(rng, n):
    def pstate(special=False):
        st = random_state(rng)
        for r in FPR_REGS:
            st.fpr0[r] = bits(f32(dbl(st.fpr0[r])))
            st.fpr1[r] = bits(f32(rand_f64(rng, finite_only=not special)))
        for r in FPR_REGS:
            for arr in (st.fpr0, st.fpr1):
                v = dbl(arr[r])
                if math.isnan(v):
                    arr[r] = bits(1.5)
        return st

    def lanes(st, r):
        return [dbl(st.fpr0[r]), dbl(st.fpr1[r])]

    def put(st, r, v0, v1):
        # no input is NaN, so any NaN is the default NaN of an invalid operation
        st.fpr0[r] = 0x7FF8000000000000 if math.isnan(v0) else bits(v0)
        st.fpr1[r] = 0x7FF8000000000000 if math.isnan(v1) else bits(v1)

    def case(name, w, st, fn):
        out = st.copy()
        fn(out)
        CASES.append(Case([w, BLR], st, out, ps1=True, name=name))

    ops = [("ps_add", 21, "add"), ("ps_sub", 20, "sub"), ("ps_mul", 25, "mul"), ("ps_div", 18, "div"), ("ps_madd", 29, "madd"), ("ps_msub", 28, "msub"), ("ps_nmadd", 31, "nmadd"), ("ps_nmsub", 30, "nmsub")]
    for name, xo, op in ops:
        for _ in range(n * 2):
            d, a, b, c = f(rng), f(rng), f(rng), f(rng)
            w = 4 << 26 | d << 21 | a << 16 | (b if op not in ("mul",) else 0) << 11 | (c if op in ("mul", "madd", "msub", "nmadd", "nmsub") else 0) << 6 | xo << 1
            st = pstate()
            if op == "div":
                for r in FPR_REGS:  # avoid NaN/inf results whose payload is implementation-defined
                    if dbl(st.fpr0[r]) == 0 or dbl(st.fpr1[r]) == 0:
                        put(st, r, 3.0, 5.0)
            bb = b if op != "mul" else 0
            cc = c if op in ("mul", "madd", "msub", "nmadd", "nmsub") else 0

            def run(o, d=d, a=a, bb=bb, cc=cc, op=op):
                res = ps_arith(op, lanes(o, a), lanes(o, bb), lanes(o, cc))
                if any(math.isnan(x) for x in res):
                    raise ValueError
                put(o, d, res[0], res[1])

            try:
                case(name, w, st, run)
            except ValueError:
                CASES.pop() if False else None
    for name, xo, lane, madd in [("ps_muls0", 12, 0, False), ("ps_muls1", 13, 1, False), ("ps_madds0", 14, 0, True), ("ps_madds1", 15, 1, True)]:
        for _ in range(n * 2):
            d, a, b, c = f(rng), f(rng), f(rng), f(rng)
            w = 4 << 26 | d << 21 | a << 16 | (b if madd else 0) << 11 | c << 6 | xo << 1
            st = pstate()

            def run(o, d=d, a=a, b=b, c=c, lane=lane, madd=madd):
                x, y, z = lanes(o, a), lanes(o, b), lanes(o, c)[lane]
                z = force25(z)
                res = [f32(x[i] * z + (y[i] if madd else 0.0)) if madd else f32(x[i] * z) for i in range(2)]
                put(o, d, res[0], res[1])

            case(name, w, st, run)
    for name, xo, first in [("ps_sum0", 10, True), ("ps_sum1", 11, False)]:
        for _ in range(n * 2):
            d, a, b, c = f(rng), f(rng), f(rng), f(rng)
            w = 4 << 26 | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1
            st = pstate()

            def run(o, d=d, a=a, b=b, c=c, first=first):
                x, y, z = lanes(o, a), lanes(o, b), lanes(o, c)
                s = f32(x[0] + y[1])
                put(o, d, *( [s, z[1]] if first else [z[0], s] ))

            case(name, w, st, run)
    for name, xo, (l0, l1) in [("ps_merge00", 528, (0, 0)), ("ps_merge01", 560, (0, 1)), ("ps_merge10", 592, (1, 0)), ("ps_merge11", 624, (1, 1))]:
        for _ in range(n):
            d, a, b = f(rng), f(rng), f(rng)
            w = 4 << 26 | d << 21 | a << 16 | b << 11 | xo << 1
            st = pstate(special=True)

            def run(o, d=d, a=a, b=b, l0=l0, l1=l1):
                put(o, d, lanes(o, a)[l0], lanes(o, b)[l1])

            case(name, w, st, run)
    for name, xo, fn in [("ps_mr", 72, lambda v: v), ("ps_neg", 40, lambda v: -v), ("ps_abs", 264, abs), ("ps_nabs", 136, lambda v: -abs(v))]:
        for _ in range(n):
            d, b = f(rng), f(rng)
            w = 4 << 26 | d << 21 | b << 11 | xo << 1
            st = pstate(special=True)

            def run(o, d=d, b=b, fn=fn):
                x = lanes(o, b)
                put(o, d, fn(x[0]), fn(x[1]))

            case(name, w, st, run)
    for _ in range(n * 2):
        d, a, b, c = f(rng), f(rng), f(rng), f(rng)
        w = 4 << 26 | d << 21 | a << 16 | b << 11 | c << 6 | 23 << 1
        st = pstate()

        def run(o, d=d, a=a, b=b, c=c):
            x, y, z = lanes(o, a), lanes(o, b), lanes(o, c)
            put(o, d, *[z[i] if x[i] >= 0.0 or (x[i] == 0.0) else y[i] for i in range(2)])

        case("ps_sel", w, st, run)
    for name, xo, lane in [("ps_cmpu0", 0, 0), ("ps_cmpo0", 32, 0), ("ps_cmpu1", 64, 1), ("ps_cmpo1", 96, 1)]:
        for _ in range(n):
            crf = rng.randrange(8)
            a, b = f(rng), f(rng)
            w = 4 << 26 | crf << 23 | a << 16 | b << 11 | xo << 1
            st = pstate()

            def run(o, crf=crf, a=a, b=b, lane=lane):
                x, y = lanes(o, a)[lane], lanes(o, b)[lane]
                nib = 8 if x < y else 4 if x > y else 2
                sh = 28 - 4 * crf
                o.cr = (o.cr & ~(0xF << sh) & 0xFFFFFFFF) | nib << sh

            case(name, w, st, run)


def double_to_single(d):
    """stfs/psq_st float conversion: truncate the mantissa."""
    x = bits(d)
    exp = x >> 52 & 0x7FF
    if exp > 896 or (x & 0x7FFFFFFFFFFFFFFF) == 0:
        return ((x >> 32) & 0xC0000000) | ((x >> 29) & 0x3FFFFFFF)
    if exp >= 874:
        t = ((x & 0xFFFFFFFFFFFFF) | 0x10000000000000) >> (926 - exp)
        return ((x >> 32) & 0x80000000) | t
    return (x >> 32) & 0x80000000


def clamp(v, lo, hi):
    if math.isnan(v):
        return 0
    if v <= lo:
        return lo
    if v >= hi:
        return hi
    return int(v)


def quant(v, typ, scale):
    if typ in (0, 1, 2, 3):
        return struct.pack(">I", double_to_single(v)), 4
    v = math.ldexp(v, scale)
    if typ == 4:
        return struct.pack(">B", clamp(v, 0, 255)), 1
    if typ == 5:
        return struct.pack(">H", clamp(v, 0, 65535)), 2
    if typ == 6:
        return struct.pack(">b", clamp(v, -128, 127)), 1
    return struct.pack(">h", clamp(v, -32768, 32767)), 2


def dequant(raw, typ, scale):
    if typ in (0, 1, 2, 3):
        return struct.unpack(">f", raw[:4])[0], 4
    if typ == 4:
        return math.ldexp(raw[0], -scale), 1
    if typ == 5:
        return math.ldexp(struct.unpack(">H", raw[:2])[0], -scale), 2
    if typ == 6:
        return math.ldexp(struct.unpack(">b", raw[:1])[0], -scale), 1
    return math.ldexp(struct.unpack(">h", raw[:2])[0], -scale), 2


def psq_cases(rng, n):
    # Quantized load/store use the GQR registers; programs set GQR1 through mtspr 913 first.
    for load in (True, False):
        for _ in range(n * 6):
            typ = rng.choice([0, 4, 5, 6, 7])
            scale = rng.randrange(-8, 9) if typ else 0
            gqr = (typ << 16 | (scale & 0x3F) << 24) if load else (typ | (scale & 0x3F) << 8)
            w_flag = rng.getrandbits(1)
            disp = rng.randrange(-0x20, 0x20, 4) & 0xFFF
            sdisp = disp - 0x1000 if disp & 0x800 else disp
            r = f(rng)
            st = random_state(rng, mem=True)
            st.gpr[11] = gqr
            for rr in FPR_REGS:
                st.fpr0[rr] = bits(f32(rand_f64(rng, finite_only=True)))
                st.fpr1[rr] = bits(f32(rand_f64(rng, finite_only=True)))
            prog = [31 << 26 | 11 << 21 | spr_field(913) << 11 | 467 << 1,
                    (56 if load else 60) << 26 | r << 21 | BASE << 16 | w_flag << 15 | 1 << 12 | disp]
            out = st.copy()
            ea = st.gpr[BASE] + sdisp
            off = ea - MEM
            raw = b"".join(struct.pack(">I", x) for x in st.mem)
            if load:
                a, sz = dequant(raw[off:off + 4], typ, scale)
                if math.isnan(a):
                    continue
                b2 = 1.0
                if not w_flag:
                    b2, _ = dequant(raw[off + sz:off + sz + 4], typ, scale)
                    if math.isnan(b2):
                        continue
                out.fpr0[r], out.fpr1[r] = bits(a), bits(b2)
            else:
                a = dbl(st.fpr0[r])
                b2 = dbl(st.fpr1[r])
                q0, sz = quant(a, typ, scale)
                mem = bytearray(raw)
                mem[off:off + sz] = q0
                if not w_flag:
                    q1, _ = quant(b2, typ, scale)
                    mem[off + sz:off + sz + sz] = q1
                out.mem = list(struct.unpack(">%dI" % MEM_WORDS, bytes(mem)))
            out.gpr[11] = gqr
            CASES.append(Case(prog + [BLR], st, out, ps1=True, name="psq_l" if load else "psq_st"))


def psqx_cases(rng, n):
    """psq_lx / psq_stx: same semantics as psq_l/psq_st with the displacement taken from rB."""
    for load in (True, False):
        for _ in range(n * 3):
            typ = rng.choice([0, 4, 5, 6, 7])
            scale = rng.randrange(-8, 9) if typ else 0
            gqr = (typ << 16 | (scale & 0x3F) << 24) if load else (typ | (scale & 0x3F) << 8)
            w_flag = rng.getrandbits(1)
            r = f(rng)
            st = random_state(rng, mem=True)
            st.gpr[11] = gqr
            for rr in FPR_REGS:
                st.fpr0[rr] = bits(f32(rand_f64(rng, finite_only=True)))
                st.fpr1[rr] = bits(f32(rand_f64(rng, finite_only=True)))
            xo = 6 if load else 7
            prog = [31 << 26 | 11 << 21 | spr_field(913) << 11 | 467 << 1,
                    4 << 26 | r << 21 | BASE << 16 | INDEX << 11 | w_flag << 10 | 1 << 7 | xo << 1]
            out = st.copy()
            off = st.gpr[BASE] + st.gpr[INDEX] - MEM
            raw = b"".join(struct.pack(">I", x) for x in st.mem)
            if load:
                a, sz = dequant(raw[off:off + 4], typ, scale)
                b2 = 1.0
                if not w_flag:
                    b2, _ = dequant(raw[off + sz:off + sz + 4], typ, scale)
                if math.isnan(a) or math.isnan(b2):
                    continue
                out.fpr0[r], out.fpr1[r] = bits(a), bits(b2)
            else:
                q0, sz = quant(dbl(st.fpr0[r]), typ, scale)
                mem = bytearray(raw)
                mem[off:off + sz] = q0
                if not w_flag:
                    q1, _ = quant(dbl(st.fpr1[r]), typ, scale)
                    mem[off + sz:off + sz + sz] = q1
                out.mem = list(struct.unpack(">%dI" % MEM_WORDS, bytes(mem)))
            CASES.append(Case(prog + [BLR], st, out, ps1=True, name="psq_lx" if load else "psq_stx"))


def main():
    ap = argparse.ArgumentParser(
        description="Generate PowerPC test vectors (Unicorn oracle + a Gekko paired-single model) as a text file.")
    ap.add_argument("out", help="output file")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0xFE9, help="RNG seed (default 0xFE9)")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="multiplier of the number of random cases per instruction (default 1.0; 0.1 makes a small subset)")
    args = ap.parse_args()
    rng = random.Random(args.seed)

    def n(base):
        return max(1, round(base * args.scale))

    int_cases(rng, n(24))
    cr_cases(rng, n(24))
    mem_cases(rng, n(16))
    fp_cases(rng, n(12))
    ps_cases(rng, n(12))
    psq_cases(rng, n(8))
    psqx_cases(rng, n(8))
    with open(args.out, "w") as fh:
        fh.write("# ppc-diff vectors v1; seed %#x scale %g\n" % (args.seed, args.scale))
        for c in CASES:
            fh.write(c.pack() + "\n")
    print(f"{len(CASES)} cases written to {args.out}")
    if SKIPPED:
        print("skipped (oracle refused):", SKIPPED)


if __name__ == "__main__":
    main()
