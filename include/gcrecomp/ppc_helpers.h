#pragma once
// =============================================================================
// gcrecomp - PowerPC Helper Functions for Recompiled Code
//
// These inline functions implement PowerPC operations that don't map directly
// to single C expressions. The recompiler emits calls to these in the
// generated code.
//
// Pure computation helpers (CNTLZW, ROTL32, MFTBL, MFTBU) are standalone.
// PSQ (Paired Singles Quantized) helpers take a Memory& parameter so they
// can access emulated RAM without depending on macros. Wrapper macros that
// pass g_mem are defined in the auto-generated recomp_common.h.
// =============================================================================

#include <cstdint>
#include <cstring>
#include "gcrecomp/runtime.h"

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

// =============================================================================
// PSQ (Paired Singles Quantized) Load/Store Helpers
//
// These implement the quantized load/store operations used by the Gekko's
// paired-single instructions (psq_l, psq_st, etc.). The GQR (Graphics
// Quantization Register) encodes the data type and scale factor:
//   Load:  type = bits [2:0], scale = bits [13:8]
//   Store: type = bits [18:16], scale = bits [29:24]
//
// Each function takes a Memory& parameter; the auto-generated recomp_common.h
// defines wrapper macros (PSQ_LOAD_ONE, etc.) that pass g_mem.
// =============================================================================

// Quantization types from GQR
enum PSQ_Type : uint32_t {
    PSQ_TYPE_FLOAT = 0,
    PSQ_TYPE_U8    = 4,
    PSQ_TYPE_S8    = 5,
    PSQ_TYPE_U16   = 6,
    PSQ_TYPE_S16   = 7,
};

inline float psq_load_one(gcrecomp::Memory& mem, uint32_t addr, uint32_t gqr) {
    uint32_t type  = gqr & 0x7;
    uint32_t scale = (gqr >> 8) & 0x3F;
    float divisor  = static_cast<float>(1u << scale);
    switch (type) {
        case PSQ_TYPE_FLOAT: { uint32_t raw = mem.read32(addr); float val; memcpy(&val, &raw, 4); return val; }
        case PSQ_TYPE_U8:  return static_cast<float>(mem.read8(addr)) / divisor;
        case PSQ_TYPE_S8:  return static_cast<float>(static_cast<int8_t>(mem.read8(addr))) / divisor;
        case PSQ_TYPE_U16: return static_cast<float>(mem.read16(addr)) / divisor;
        case PSQ_TYPE_S16: return static_cast<float>(static_cast<int16_t>(mem.read16(addr))) / divisor;
        default: return 0.0f;
    }
}

inline void psq_load_pair(gcrecomp::Memory& mem, float* ps0, float* ps1, uint32_t addr, uint32_t gqr) {
    uint32_t type  = gqr & 0x7;
    uint32_t scale = (gqr >> 8) & 0x3F;
    float divisor  = static_cast<float>(1u << scale);
    switch (type) {
        case PSQ_TYPE_FLOAT: { uint32_t r0 = mem.read32(addr); uint32_t r1 = mem.read32(addr+4); memcpy(ps0, &r0, 4); memcpy(ps1, &r1, 4); break; }
        case PSQ_TYPE_U8:  *ps0 = static_cast<float>(mem.read8(addr))/divisor; *ps1 = static_cast<float>(mem.read8(addr+1))/divisor; break;
        case PSQ_TYPE_S8:  *ps0 = static_cast<float>(static_cast<int8_t>(mem.read8(addr)))/divisor; *ps1 = static_cast<float>(static_cast<int8_t>(mem.read8(addr+1)))/divisor; break;
        case PSQ_TYPE_U16: *ps0 = static_cast<float>(mem.read16(addr))/divisor; *ps1 = static_cast<float>(mem.read16(addr+2))/divisor; break;
        case PSQ_TYPE_S16: *ps0 = static_cast<float>(static_cast<int16_t>(mem.read16(addr)))/divisor; *ps1 = static_cast<float>(static_cast<int16_t>(mem.read16(addr+2)))/divisor; break;
        default: *ps0 = *ps1 = 0.0f; break;
    }
}

inline void psq_store_one(gcrecomp::Memory& mem, float val, uint32_t addr, uint32_t gqr) {
    uint32_t type  = (gqr >> 16) & 0x7;
    uint32_t scale = (gqr >> 24) & 0x3F;
    float mul = static_cast<float>(1u << scale);
    switch (type) {
        case PSQ_TYPE_FLOAT: { uint32_t raw; memcpy(&raw, &val, 4); mem.write32(addr, raw); break; }
        case PSQ_TYPE_U8:  mem.write8(addr, static_cast<uint8_t>(val*mul)); break;
        case PSQ_TYPE_S8:  mem.write8(addr, static_cast<uint8_t>(static_cast<int8_t>(val*mul))); break;
        case PSQ_TYPE_U16: mem.write16(addr, static_cast<uint16_t>(val*mul)); break;
        case PSQ_TYPE_S16: mem.write16(addr, static_cast<uint16_t>(static_cast<int16_t>(val*mul))); break;
        default: break;
    }
}

inline void psq_store_pair(gcrecomp::Memory& mem, float v0, float v1, uint32_t addr, uint32_t gqr) {
    uint32_t type  = (gqr >> 16) & 0x7;
    uint32_t scale = (gqr >> 24) & 0x3F;
    float mul = static_cast<float>(1u << scale);
    switch (type) {
        case PSQ_TYPE_FLOAT: { uint32_t r0,r1; memcpy(&r0,&v0,4); memcpy(&r1,&v1,4); mem.write32(addr,r0); mem.write32(addr+4,r1); break; }
        case PSQ_TYPE_U8:  mem.write8(addr, static_cast<uint8_t>(v0*mul)); mem.write8(addr+1, static_cast<uint8_t>(v1*mul)); break;
        case PSQ_TYPE_S8:  mem.write8(addr, static_cast<uint8_t>(static_cast<int8_t>(v0*mul))); mem.write8(addr+1, static_cast<uint8_t>(static_cast<int8_t>(v1*mul))); break;
        case PSQ_TYPE_U16: mem.write16(addr, static_cast<uint16_t>(v0*mul)); mem.write16(addr+2, static_cast<uint16_t>(v1*mul)); break;
        case PSQ_TYPE_S16: mem.write16(addr, static_cast<uint16_t>(static_cast<int16_t>(v0*mul))); mem.write16(addr+2, static_cast<uint16_t>(static_cast<int16_t>(v1*mul))); break;
        default: break;
    }
}
