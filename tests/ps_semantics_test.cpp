// SPDX-License-Identifier: MIT
//
// Floating-point and paired-single semantics of the code the recompiler emits. The instructions are emitted by
// ps_semantics_gen at build time and executed here.
//
// Regressions this guards: (1) the paired-single registers were split from the scalar ones (ps0 lived in a
// separate array), so a psq_l / ps_* result never reached a following fmuls or stfs and a callee-saved f31 saved
// by psq_st / restored by psq_l came back stale: every matrix computed with the SDK's paired-single code was wrong;
// (2) a ps_* instruction wrote lane 0 before reading the other operands, so it was wrong whenever the destination
// was also frB or frC; (3) quantized stores wrapped instead of saturating.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gcrecomp/ppc_helpers.h"
#include "gcrecomp/runtime.h"

using gcrecomp::g_mem;
using gcrecomp::Memory;
using gcrecomp::PPCContext;

#define MEM_READ8(addr)         g_mem.read8(static_cast<uint32_t>(addr))
#define MEM_READ16(addr)        g_mem.read16(static_cast<uint32_t>(addr))
#define MEM_READ32(addr)        g_mem.read32(static_cast<uint32_t>(addr))
#define MEM_READF32(addr)       g_mem.readf32(static_cast<uint32_t>(addr))
#define MEM_READF64(addr)       g_mem.readf64(static_cast<uint32_t>(addr))
#define MEM_WRITE8(addr, val)   g_mem.write8(static_cast<uint32_t>(addr), static_cast<uint8_t>(val))
#define MEM_WRITE16(addr, val)  g_mem.write16(static_cast<uint32_t>(addr), static_cast<uint16_t>(val))
#define MEM_WRITE32(addr, val)  g_mem.write32(static_cast<uint32_t>(addr), static_cast<uint32_t>(val))
#define MEM_WRITEF32(addr, val) g_mem.writef32(static_cast<uint32_t>(addr), static_cast<float>(val))
#define MEM_WRITEF64(addr, val) g_mem.writef64(static_cast<uint32_t>(addr), static_cast<double>(val))
#define PSQ_LOAD_ONE(addr, gqr)             psq_load_one(g_mem, static_cast<uint32_t>(addr), gqr)
#define PSQ_LOAD_PAIR(ps0, ps1, addr, gqr)  psq_load_pair(g_mem, ps0, ps1, static_cast<uint32_t>(addr), gqr)
#define PSQ_STORE_ONE(val, addr, gqr)       psq_store_one(g_mem, val, static_cast<uint32_t>(addr), gqr)
#define PSQ_STORE_PAIR(v0, v1, addr, gqr)   psq_store_pair(g_mem, v0, v1, static_cast<uint32_t>(addr), gqr)

#include "ps_semantics.inc"

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static void set(PPCContext& c, int n, double ps0, float ps1) {
    c.f[n] = ps0;
    c.ps1[n] = ps1;
}

// f[n] is ps0 and must equal the result exactly; ps1 likewise.
static bool is(const PPCContext& c, int n, double ps0, float ps1) { return c.f[n] == ps0 && c.ps1[n] == ps1; }

#define EXPECT(c, n, p0, p1)                                                                                        \
    do {                                                                                                            \
        if (!is(c, n, p0, p1)) {                                                                                    \
            std::printf("%s:%d: f%d = (%g, %g), expected (%g, %g)\n", __FILE__, __LINE__, n, c.f[n], double(c.ps1[n]), \
                        double(p0), double(p1));                                                                    \
            ++g_failures;                                                                                           \
        }                                                                                                           \
    } while (0)

static void alu_tests() {
    PPCContext c = {};
    Memory* mem = &g_mem;

    // The destination is also frC: lane 1 must use the old frC ps0.
    c = {}; set(c, 2, 2, 3); set(c, 1, 5, 7);
    sem_muls0_dst_is_c(&c, mem);
    EXPECT(c, 1, 10, 15);

    c = {}; set(c, 2, 2, 3); set(c, 3, 11, 13);
    sem_muls1(&c, mem);
    EXPECT(c, 4, 26, 39);

    // ps_madds0 f1,f2,f1,f3: (f2.ps0 * f1.ps0 + f3.ps0, f2.ps1 * f1.ps0 + f3.ps1)
    c = {}; set(c, 2, 2, 3); set(c, 1, 5, 7); set(c, 3, 100, 200);
    sem_madds0_dst_is_c(&c, mem);
    EXPECT(c, 1, 110, 215);

    c = {}; set(c, 2, 2, 3); set(c, 3, 11, 13); set(c, 5, 100, 200);
    sem_madds1(&c, mem);  // a=f2 c=f3 b=f5: (2*13+100, 3*13+200)
    EXPECT(c, 4, 126, 239);

    // ps_merge with the destination equal to frB: lane 1 takes the old frB lane.
    c = {}; set(c, 2, 1, 2); set(c, 1, 3, 4);
    sem_merge00_dst_is_b(&c, mem);
    EXPECT(c, 1, 1, 3);
    c = {}; set(c, 2, 1, 2); set(c, 1, 3, 4);
    sem_merge10_dst_is_b(&c, mem);
    EXPECT(c, 1, 2, 3);

    c = {}; set(c, 2, 1, 2); set(c, 3, 3, 4);
    sem_merge01(&c, mem);
    EXPECT(c, 4, 1, 4);
    sem_merge11(&c, mem);
    EXPECT(c, 4, 2, 4);

    c = {}; set(c, 2, 1, 2); set(c, 3, 10, 20); set(c, 5, 100, 200);
    sem_sum0(&c, mem);  // (a0 + b1, c1)
    EXPECT(c, 4, 21, 200);

    // ps_sum1 f1,f1,f2,f3: (c0, a0 + b1); the destination is frA.
    c = {}; set(c, 1, 1, 2); set(c, 2, 10, 20); set(c, 3, 100, 200);
    sem_sum1_dst_is_a(&c, mem);
    EXPECT(c, 1, 100, 21);

    c = {}; set(c, 2, 1, 2); set(c, 3, 10, 20);
    sem_add(&c, mem);
    EXPECT(c, 4, 11, 22);

    c = {}; set(c, 2, 2, 3); set(c, 5, 4, 5); set(c, 3, 1, 2);
    sem_madd(&c, mem);  // a*c + b
    EXPECT(c, 4, 9, 17);
    sem_nmsub(&c, mem);  // -(a*c - b)
    EXPECT(c, 4, -7, -13);

    c = {}; set(c, 2, -1, 1); set(c, 3, 10, 20); set(c, 5, 30, 40);
    sem_sel(&c, mem);  // a >= 0 ? c : b
    EXPECT(c, 4, 10, 40);

    c = {}; set(c, 3, 3, -4);
    sem_neg(&c, mem);
    EXPECT(c, 4, -3, 4);

    // Scalar single-precision results are visible to paired-single instructions and to scalar ones alike.
    c = {}; set(c, 2, 2, 0); set(c, 5, 4, 0); set(c, 3, 1, 0);
    sem_fmadds_fills_both(&c, mem);
    EXPECT(c, 4, 9, 9);
    c = {}; set(c, 2, 1.5, 0); set(c, 3, 2.25, 0);
    sem_fadds_fills_both(&c, mem);
    EXPECT(c, 4, 3.75, 3.75f);
    c = {}; set(c, 4, 1, 77); set(c, 3, 8, 9);
    sem_fmr_keeps_ps1(&c, mem);
    EXPECT(c, 4, 8, 77);  // a double move leaves ps1 alone

    // ps_add reads what fmadds wrote (two different instruction families on one register).
    c = {}; set(c, 2, 2, 0); set(c, 5, 4, 0); set(c, 3, 1, 0);
    sem_fmadds_fills_both(&c, mem);  // f4 = (9, 9)
    c.f[2] = c.f[4]; c.ps1[2] = c.ps1[4]; c.f[3] = c.f[4]; c.ps1[3] = c.ps1[4];
    sem_add(&c, mem);
    EXPECT(c, 4, 18, 18);
}

static void psq_tests() {
    PPCContext c = {};
    Memory* mem = &g_mem;
    const uint32_t base = 0x80700000;

    // qr1 = float; qr2 = signed 16-bit with scale 4 on the store side.
    // GQR: store type bits 16-18, store scale 24-29, load type 0-2, load scale 8-13.
    g_mem.writef32(base + 8, 1.5f);
    g_mem.writef32(base + 12, -2.5f);
    c = {}; c.gqr[1] = 0; c.r[3] = base;
    sem_psq_l_pair(&c, mem);
    EXPECT(c, 4, 1.5, -2.5f);
    sem_psq_l_single(&c, mem);  // W=1: ps1 loads as 1.0
    EXPECT(c, 4, 1.5, 1.0f);

    // The loaded value is the scalar register: a following scalar op sees it (no separate ps0 copy).
    CHECK(c.f[4] == 1.5);

    // psq_st with qr2 (s16, scale 4): 1.5 * 16 = 24, -2.5 * 16 = -40
    c = {}; c.gqr[2] = (7u << 16) | (4u << 24); c.r[3] = base; set(c, 2, 1.5, -2.5f);
    sem_psq_st_pair(&c, mem);
    CHECK(g_mem.read16(base + 8) == 24);
    CHECK(static_cast<int16_t>(g_mem.read16(base + 10)) == -40);

    // psq_stu updates rA.
    c.r[3] = base;
    sem_psq_stu_pair(&c, mem);
    CHECK(c.r[3] == base + 8);

    // Indexed forms: EA = rA + rB.
    g_mem.writef32(base + 0x20, 6.0f);
    g_mem.writef32(base + 0x24, 7.0f);
    c = {}; c.gqr[1] = 0; c.r[3] = base; c.r[5] = 0x20;
    sem_psq_lx_pair(&c, mem);
    EXPECT(c, 4, 6.0, 7.0f);
    sem_psq_lux_pair(&c, mem);
    CHECK(c.r[3] == base + 0x20);

    c = {}; c.gqr[2] = (7u << 16) | (4u << 24); c.r[3] = base; c.r[5] = 0x40; set(c, 2, 3.0, 99.0f);
    sem_psq_stx_single(&c, mem);  // W=1: one element
    CHECK(static_cast<int16_t>(g_mem.read16(base + 0x40)) == 48);

    // Saturation instead of wrapping, and a negative (two's complement) scale.
    g_mem.write32(base + 0x60, 0);
    gcrecomp::Memory& m = g_mem;
    psq_store_one(m, 300.0f, base + 0x60, (4u << 16));       // u8
    CHECK(m.read8(base + 0x60) == 255);
    psq_store_one(m, -200.0f, base + 0x61, (5u << 16));      // s8
    CHECK(m.read8(base + 0x61) == 0x80);
    psq_store_one(m, 200.0f, base + 0x62, (5u << 16));
    CHECK(m.read8(base + 0x62) == 0x7F);
    psq_store_one(m, -5.0f, base + 0x64, (6u << 16));        // u16
    CHECK(m.read16(base + 0x64) == 0);
    psq_store_one(m, 1e9f, base + 0x66, (7u << 16));         // s16
    CHECK(m.read16(base + 0x66) == 0x7FFF);
    psq_store_one(m, 100.0f, base + 0x68, (4u << 16) | (62u << 24));  // scale -2: 100 / 4
    CHECK(m.read8(base + 0x68) == 25);
    m.write8(base + 0x6A, 100);
    CHECK(psq_load_one(m, base + 0x6A, 4u | (62u << 8)) == 400.0f);  // u8 100 * 4
    m.write8(base + 0x6B, 0xFF);
    CHECK(psq_load_one(m, base + 0x6B, 5u | (6u << 8)) == -1.0f / 64.0f);  // s8 -1 / 2^6
}

static void convert_tests() {
    PPCContext c = {};
    Memory* mem = &g_mem;
    auto low = [](const PPCContext& x) { uint64_t b; std::memcpy(&b, &x.f[4], 8); return static_cast<int32_t>(b); };
    c = {}; c.f[3] = -2.7;
    sem_fctiwz(&c, mem); CHECK(low(c) == -2);
    sem_fctiw(&c, mem);  CHECK(low(c) == -3);
    c.f[3] = 2.5;
    sem_fctiw(&c, mem);  CHECK(low(c) == 2);  // ties to even
    c.f[3] = 1e20;
    sem_fctiwz(&c, mem); CHECK(low(c) == 0x7FFFFFFF);
    c.f[3] = -1e20;
    sem_fctiwz(&c, mem); CHECK(low(c) == INT32_MIN);
    // stfiwx stores the low word of the register as it is (after fctiwz).
    c = {}; c.f[3] = -7.9; c.r[3] = 0x80700000; c.r[5] = 0x80;
    sem_fctiwz(&c, mem);
    sem_stfiwx(&c, mem);
    CHECK(static_cast<int32_t>(g_mem.read32(0x80700080)) == -7);
    c.f[3] = std::nan("");
    sem_fctiwz(&c, mem); CHECK(low(c) == INT32_MIN);
}

int main() {
    if (!g_mem.init()) {
        std::printf("cannot initialise memory\n");
        return 1;
    }
    alu_tests();
    psq_tests();
    convert_tests();
    if (g_failures) std::printf("%d failure(s)\n", g_failures);
    else std::printf("ps_semantics: all checks passed\n");
    return g_failures ? 1 : 0;
}
