#pragma once
// =============================================================================
// gcrecomp - PowerPC Helper Functions for Recompiled Code
//
// These inline functions implement PowerPC operations that don't map directly
// to single C expressions. The recompiler emits calls to these in the
// generated code.
//
// NOTE: PSQ (Paired Singles Quantized) helpers require memory access macros
// and are defined in recomp_common.h after the MEM_* macro definitions.
// =============================================================================

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#include <intrin.h>
#endif

// Count Leading Zeros (Word) - PowerPC cntlzw instruction
// Returns the number of leading zero bits in a 32-bit value (0-32).
inline uint32_t PPC_CNTLZW(uint32_t val) {
    if (val == 0) return 32;
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse(&idx, val);
    return 31 - idx;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(val);
#else
    uint32_t n = 0;
    if ((val & 0xFFFF0000) == 0) { n += 16; val <<= 16; }
    if ((val & 0xFF000000) == 0) { n += 8;  val <<= 8;  }
    if ((val & 0xF0000000) == 0) { n += 4;  val <<= 4;  }
    if ((val & 0xC0000000) == 0) { n += 2;  val <<= 2;  }
    if ((val & 0x80000000) == 0) { n += 1; }
    return n;
#endif
}

// Rotate Left 32-bit - PowerPC rlwinm/rlwimi/rlwnm base operation
// Rotates a 32-bit value left by the specified number of bits.
inline uint32_t PPC_ROTL32(uint32_t val, uint32_t shift) {
    shift &= 31;
    if (shift == 0) return val;
    return (val << shift) | (val >> (32 - shift));
}

// Read Time Base Lower (mftb / mftbl) - SPR 268
inline uint32_t PPC_MFTBL() {
#ifdef _WIN32
    uint64_t counter = __rdtsc();
    return static_cast<uint32_t>(counter);
#elif defined(__GNUC__) || defined(__clang__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 40500000ULL + ts.tv_nsec / 25);
#else
    return 0;
#endif
}

// Read Time Base Upper (mftbu) - SPR 269
inline uint32_t PPC_MFTBU() {
#ifdef _WIN32
    uint64_t counter = __rdtsc();
    return static_cast<uint32_t>(counter >> 32);
#elif defined(__GNUC__) || defined(__clang__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ticks = ts.tv_sec * 40500000ULL + ts.tv_nsec / 25;
    return static_cast<uint32_t>(ticks >> 32);
#else
    return 0;
#endif
}
