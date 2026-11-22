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

#include <cmath>
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

// The scale fields of a GQR are 6-bit two's complement exponents: a load divides by 2^scale, a store multiplies.
inline float psq_scale(uint32_t field) { return std::ldexp(1.0f, static_cast<int>(field ^ 0x20u) - 0x20); }

inline float psq_dequant(uint32_t raw, uint32_t type, float inv_scale_exp) {
    switch (type) {
        case PSQ_TYPE_U8:  return static_cast<float>(static_cast<uint8_t>(raw)) * inv_scale_exp;
        case PSQ_TYPE_S8:  return static_cast<float>(static_cast<int8_t>(raw)) * inv_scale_exp;
        case PSQ_TYPE_U16: return static_cast<float>(static_cast<uint16_t>(raw)) * inv_scale_exp;
        default:           return static_cast<float>(static_cast<int16_t>(raw)) * inv_scale_exp;
    }
}

// Quantize for a store: multiply by the scale, truncate toward zero and saturate to the integer type
// (NaN saturates low, like the hardware's conversion).
inline uint32_t psq_quant(float v, uint32_t type, float mul) {
    float lo, hi;
    switch (type) {
        case PSQ_TYPE_U8:  lo = 0.0f;      hi = 255.0f;   break;
        case PSQ_TYPE_S8:  lo = -128.0f;   hi = 127.0f;   break;
        case PSQ_TYPE_U16: lo = 0.0f;      hi = 65535.0f; break;
        default:           lo = -32768.0f; hi = 32767.0f; break;
    }
    float t = v * mul;
    if (!(t > lo)) t = lo;
    if (t > hi) t = hi;
    return static_cast<uint32_t>(static_cast<int32_t>(t));
}

inline uint32_t psq_elem_size(uint32_t type) { return (type == PSQ_TYPE_U8 || type == PSQ_TYPE_S8) ? 1 : (type == PSQ_TYPE_FLOAT ? 4 : 2); }

inline uint32_t psq_read_raw(gcrecomp::Memory& mem, uint32_t addr, uint32_t type) {
    switch (psq_elem_size(type)) {
        case 1: return mem.read8(addr);
        case 2: return mem.read16(addr);
        default: return mem.read32(addr);
    }
}

inline float psq_load_elem(gcrecomp::Memory& mem, uint32_t addr, uint32_t type, float inv_scale) {
    // Types 1-3 are reserved; the hardware treats them as float.
    if (type < PSQ_TYPE_U8) {
        const uint32_t raw = mem.read32(addr);
        float val;
        memcpy(&val, &raw, 4);
        return val;
    }
    return psq_dequant(psq_read_raw(mem, addr, type), type, inv_scale);
}

inline float psq_load_one(gcrecomp::Memory& mem, uint32_t addr, uint32_t gqr) {
    const uint32_t type = gqr & 0x7;
    return psq_load_elem(mem, addr, type, 1.0f / psq_scale((gqr >> 8) & 0x3F));
}

inline void psq_load_pair(gcrecomp::Memory& mem, float* ps0, float* ps1, uint32_t addr, uint32_t gqr) {
    const uint32_t type = gqr & 0x7;
    const float inv = 1.0f / psq_scale((gqr >> 8) & 0x3F);
    const uint32_t step = type < PSQ_TYPE_U8 ? 4 : psq_elem_size(type);
    *ps0 = psq_load_elem(mem, addr, type, inv);
    *ps1 = psq_load_elem(mem, addr + step, type, inv);
}

inline void psq_store_elem(gcrecomp::Memory& mem, float val, uint32_t addr, uint32_t type, float mul) {
    if (type < PSQ_TYPE_U8) {
        uint32_t raw;
        memcpy(&raw, &val, 4);
        mem.write32(addr, raw);
        return;
    }
    const uint32_t q = psq_quant(val, type, mul);
    switch (psq_elem_size(type)) {
        case 1: mem.write8(addr, static_cast<uint8_t>(q)); break;
        default: mem.write16(addr, static_cast<uint16_t>(q)); break;
    }
}

inline void psq_store_one(gcrecomp::Memory& mem, float val, uint32_t addr, uint32_t gqr) {
    psq_store_elem(mem, val, addr, (gqr >> 16) & 0x7, psq_scale((gqr >> 24) & 0x3F));
}

// A store to the write-gather pipe (0xCC008000) feeds the same FIFO address, so the second element
// of a paired store goes to the same address rather than the next one.
inline uint32_t psq_next_addr(uint32_t addr, uint32_t step) { return addr == 0xCC008000u ? addr : addr + step; }

inline void psq_store_pair(gcrecomp::Memory& mem, float v0, float v1, uint32_t addr, uint32_t gqr) {
    const uint32_t type = (gqr >> 16) & 0x7;
    const float mul = psq_scale((gqr >> 24) & 0x3F);
    const uint32_t step = type < PSQ_TYPE_U8 ? 4 : psq_elem_size(type);
    psq_store_elem(mem, v0, addr, type, mul);
    psq_store_elem(mem, v1, psq_next_addr(addr, step), type, mul);
}

// fctiwz / fctiw: the result integer goes to the low word of the register. Both saturate; NaN gives 0x80000000.
inline uint64_t ppc_fctiwz(double v) {
    if (v != v) return 0x80000000u;
    if (v >= 2147483648.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(v));
}

// Round to nearest, ties to even (FPSCR RN = 0, the only mode games use).
inline uint64_t ppc_fctiw(double v) { return v != v ? 0x80000000u : ppc_fctiwz(std::nearbyint(v)); }
