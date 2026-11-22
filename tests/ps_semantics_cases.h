// SPDX-License-Identifier: MIT
//
// The instruction words of the floating-point / paired-single semantics test. ps_semantics_gen turns each one
// into C++ with the recompiler's emitter (the code that goes into the generated game sources) at build time, and
// ps_semantics_test runs the result against hand-computed expectations.
#pragma once
#include <cstdint>

// Opcode 4 A-form (frD, frA, frB, frC and a 5-bit extended opcode) and X-form (10-bit extended opcode).
constexpr uint32_t ps_a(uint32_t xo5, uint32_t d, uint32_t a, uint32_t b, uint32_t c) {
    return (4u << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo5 << 1);
}
constexpr uint32_t ps_x(uint32_t xo10, uint32_t d, uint32_t a, uint32_t b) {
    return (4u << 26) | (d << 21) | (a << 16) | (b << 11) | (xo10 << 1);
}
// Opcode 59 (single-precision scalar) and 63 (double-precision scalar) A-form.
constexpr uint32_t fp_a(uint32_t opcd, uint32_t xo5, uint32_t d, uint32_t a, uint32_t b, uint32_t c) {
    return (opcd << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo5 << 1);
}
constexpr uint32_t psq_d(uint32_t opcd, uint32_t reg, uint32_t ra, uint32_t w, uint32_t i, uint32_t d) {
    return (opcd << 26) | (reg << 21) | (ra << 16) | (w << 15) | (i << 12) | (d & 0xFFF);
}
constexpr uint32_t psq_xform(uint32_t xo6, uint32_t reg, uint32_t ra, uint32_t rb, uint32_t w, uint32_t i) {
    return (4u << 26) | (reg << 21) | (ra << 16) | (rb << 11) | (w << 10) | (i << 7) | (xo6 << 1);
}

// X(name, word)
#define PS_SEMANTICS_CASES(X)                                                                                    \
    X(muls0_dst_is_c, ps_a(12, 1, 2, 0, 1))      /* ps_muls0 f1,f2,f1 */                                         \
    X(muls1, ps_a(13, 4, 2, 0, 3))               /* ps_muls1 f4,f2,f3 */                                         \
    X(madds0_dst_is_c, ps_a(14, 1, 2, 3, 1))     /* ps_madds0 f1,f2,f1,f3 */                                     \
    X(madds1, ps_a(15, 4, 2, 5, 3))              /* ps_madds1 f4,f2,f3,f5 */                                     \
    X(merge00_dst_is_b, ps_x(528, 1, 2, 1))      /* ps_merge00 f1,f2,f1 */                                       \
    X(merge01, ps_x(560, 4, 2, 3))               /* ps_merge01 f4,f2,f3 */                                       \
    X(merge10_dst_is_b, ps_x(592, 1, 2, 1))      /* ps_merge10 f1,f2,f1 */                                       \
    X(merge11, ps_x(624, 4, 2, 3))               /* ps_merge11 f4,f2,f3 */                                       \
    X(sum0, ps_a(10, 4, 2, 3, 5))                /* ps_sum0 f4,f2,f3,f5 */                                       \
    X(sum1_dst_is_a, ps_a(11, 1, 1, 2, 3))       /* ps_sum1 f1,f1,f2,f3 */                                       \
    X(add, ps_a(21, 4, 2, 3, 0))                 /* ps_add f4,f2,f3 */                                           \
    X(madd, ps_a(29, 4, 2, 3, 5))                /* ps_madd f4,f2,f5,f3 (a*c+b) */                               \
    X(nmsub, ps_a(30, 4, 2, 3, 5))               /* ps_nmsub f4,f2,f5,f3 (-(a*c-b)) */                           \
    X(sel, ps_a(23, 4, 2, 3, 5))                 /* ps_sel f4,f2,f5,f3 */                                        \
    X(neg, ps_x(40, 4, 0, 3))                    /* ps_neg f4,f3 */                                              \
    X(fmadds_fills_both, fp_a(59, 29, 4, 2, 3, 5)) /* fmadds f4,f2,f5,f3 */                                      \
    X(fadds_fills_both, fp_a(59, 21, 4, 2, 3, 0))  /* fadds f4,f2,f3 */                                          \
    X(fmr_keeps_ps1, (63u << 26) | (4u << 21) | (3u << 11) | (72u << 1)) /* fmr f4,f3 */                        \
    X(psq_l_pair, psq_d(56, 4, 3, 0, 1, 8))      /* psq_l f4,8(r3),0,qr1 */                                      \
    X(psq_l_single, psq_d(56, 4, 3, 1, 1, 8))    /* psq_l f4,8(r3),1,qr1 */                                      \
    X(psq_st_pair, psq_d(60, 2, 3, 0, 2, 8))     /* psq_st f2,8(r3),0,qr2 */                                     \
    X(psq_stu_pair, psq_d(61, 2, 3, 0, 2, 8))    /* psq_stu f2,8(r3),0,qr2 */                                    \
    X(psq_lx_pair, psq_xform(6, 4, 3, 5, 0, 1))  /* psq_lx f4,r3,r5,0,qr1 */                                     \
    X(psq_stx_single, psq_xform(7, 2, 3, 5, 1, 2)) /* psq_stx f2,r3,r5,1,qr2 */                                  \
    X(psq_lux_pair, psq_xform(38, 4, 3, 5, 0, 1)) /* psq_lux f4,r3,r5,0,qr1 */                                   \
    X(fctiwz, fp_a(63, 15, 4, 0, 3, 0))          /* fctiwz f4,f3 */                                              \
    X(stfiwx, (31u << 26) | (4u << 21) | (3u << 16) | (5u << 11) | (983u << 1)) /* stfiwx f4,r3,r5 */ \
    X(fctiw, fp_a(63, 14, 4, 0, 3, 0))           /* fctiw f4,f3 */
