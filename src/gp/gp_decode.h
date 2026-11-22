// SPDX-License-Identifier: MIT
//
// Pure helpers of the GP module: texture and register decoding that does not touch OpenGL, so that
// it can be unit-tested without a display or a disc.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gcrecomp::gp {

// Texture formats of BP 0x88+ (TX_SETIMAGE0 bits 20..23) that this file cares about.
constexpr uint32_t kTexC4 = 8, kTexC8 = 9, kTexC14X2 = 10;

// Bytes occupied by one texture level in memory, including padding to whole tiles.
size_t texture_bytes(uint32_t w, uint32_t h, uint32_t fmt);

// Converts a 16-bit TLUT entry to RGBA8. `pal_fmt` is the TLUT format of TX_SETTLUT (0 IA8, 1 RGB565,
// 2 RGB5A3).
void tlut_entry_to_rgba(uint32_t entry, uint32_t pal_fmt, uint8_t out[4]);

// Decodes C4, C8 or C14X2 texture data to RGBA8 through the palette in `tlut` (`tlut_size` bytes).
// `settlut` is TX_SETTLUT: bits 0..9 the TMEM offset in 512-byte units, bits 10..11 the entry format.
// Indices that point past the end of the TLUT give transparent black.
std::vector<uint8_t> decode_indexed(const uint8_t* tlut, size_t tlut_size, const uint8_t* src, uint32_t w,
                                    uint32_t h, uint32_t fmt, uint32_t settlut);

// ---- Mipmaps ----------------------------------------------------------------------------------
//
// TX_SETMODE0 (BP 0x80+map, 0xA0+map): bits 0-1 wrap S, 2-3 wrap T, 4 magnification filter (1 linear),
// 5-7 minification filter, 9-16 signed LOD bias in 1/32 units. TX_SETMODE1 (BP 0x84+map, 0xA4+map):
// bits 0-7 minimum LOD, bits 8-15 maximum LOD, both unsigned 4.4 fixed point.
//
// Approximations: the game does not say how many levels a texture has; the count is derived from the
// maximum LOD (what GXInitTexObj writes for a mipmapped texture). Levels are read from memory one
// after the other, each padded to whole tiles, as the SDK lays them out. The LOD itself is computed
// by OpenGL, not by the GX formula (no diagonal-LOD or edge-LOD modes, no anisotropy).

enum class MinFilter : uint32_t { Nearest, NearestMipNearest, NearestMipLinear, Linear, LinearMipNearest, LinearMipLinear };

// Minification filter of TX_SETMODE0. Reserved codes 3 and 7 fall back to the base filter.
MinFilter min_filter_of(uint32_t mode0);

// Number of levels to load: 1 unless the filter uses mipmaps and the maximum LOD asks for more, never
// more than the texture can have (down to 1x1).
uint32_t mip_level_count(uint32_t w, uint32_t h, uint32_t mode0, uint32_t mode1);

// The filter to give OpenGL: mipmap filters degrade to their base filter when only one level exists.
MinFilter effective_min_filter(uint32_t mode0, uint32_t levels);

// Size of a level (each dimension halves, rounding down, never below 1).
inline uint32_t mip_dim(uint32_t d, uint32_t level) { return (d >> level) ? (d >> level) : 1; }

// Byte offset of `level` from the start of the texture in memory.
size_t mip_level_offset(uint32_t w, uint32_t h, uint32_t fmt, uint32_t level);

// Bytes occupied by `levels` levels.
size_t mip_chain_bytes(uint32_t w, uint32_t h, uint32_t fmt, uint32_t levels);

// LOD bias in levels (TX_SETMODE0 bits 9-16, 1/32 units).
float lod_bias_of(uint32_t mode0);

// Minimum and maximum LOD in levels (TX_SETMODE1).
inline float min_lod_of(uint32_t mode1) { return float(mode1 & 0xFF) / 16.0f; }
inline float max_lod_of(uint32_t mode1) { return float((mode1 >> 8) & 0xFF) / 16.0f; }

// ---- Fog --------------------------------------------------------------------------------------
//
// BP 0xEE FOG_PARAM_0 (A), 0xEF FOG_PARAM_1 (B magnitude, 24 bits), 0xF0 FOG_PARAM_2 (B shift, 5 bits),
// 0xF1 FOG_PARAM_3 (C, bit 20 orthographic projection, bits 21-23 fog type), 0xF2 FOG_COLOR (R G B bytes).
// A and C are floats stored without their low 12 mantissa bits (11 mantissa bits, 8 exponent bits, sign).
//
// Not implemented: the range adjustment table (BP 0xE8-0xED, GXSetFogRangeAdj).

constexpr uint32_t kFogOff = 0, kFogLinear = 2, kFogExp = 4, kFogExp2 = 5, kFogBackwardsExp = 6, kFogBackwardsExp2 = 7;

struct FogParams {
    uint32_t type = kFogOff;
    bool ortho = false;
    float a = 0, c = 0;
    uint32_t b_mag = 0, b_shift = 0;
    float color[3] = {};  // 0..1
};

FogParams decode_fog(const uint32_t* bp);

// Fog amount (0 = none, 1 = fully fog colored) for a 24-bit depth value. Mirrors the fragment shader.
float fog_factor(const FogParams& p, uint32_t z24);

// ---- Indirect texturing -----------------------------------------------------------------------
//
// BP 0x06-0x0E (indirect matrices), 0x0F (mask), 0x10-0x1F (IND_CMD of TEV stage 0..15), 0x25-0x27
// (indirect texture scale and order). A draw is indirect when GENMODE (BP 0x00 bits 16-18) declares
// indirect stages and a TEV stage in use has a non-zero IND_CMD (GXSetTevDirect writes zero).
// Not implemented: only detected, so the log tells which game needs it.

// Bit i is set when TEV stage i of the current draw does an indirect lookup (0 when none does).
uint32_t indirect_tev_stages(const uint32_t* bp);

// ---- Z compare location -------------------------------------------------------------------------
//
// BP 0x43 PE_CONTROL bit 6 (GXSetZCompLoc): the depth test runs before texturing ("early Z"). The
// depth buffer is then updated even for pixels the alpha test rejects afterwards; with the test
// after texturing (late Z) a rejected pixel leaves the depth buffer alone. OpenGL discards before the
// depth write, which is the late behaviour, so early Z needs a second, depth-only pass.

// True when the alpha test (BP 0xF3) can reject some pixels.
bool alpha_test_can_reject(uint32_t alpha_compare);

// True when a draw has to write depth for pixels rejected by the alpha test: early Z, depth test and
// depth update enabled (BP 0x40), and an alpha test that can reject.
bool early_z_writes_rejected_pixels(const uint32_t* bp);

// ---- Blend mode, logic ops and constant alpha ---------------------------------------------------
//
// BP 0x41 CMODE0: bit 0 blend, bit 1 logic op, bit 3 color update, bit 4 alpha update, bits 5-7
// destination factor, bits 8-10 source factor, bit 11 subtract, bits 12-15 logic op. BP 0x42 CMODE1
// (GXSetDstAlpha): bits 0-7 constant alpha, bit 8 enable. The constant replaces the alpha that is
// written to the EFB; blend factors that use the source alpha keep using the TEV alpha.

struct BlendState {
    bool blend = false;
    bool subtract = false;
    bool logic_op = false;     // a logic op replaces the blend: set only when blending is off
    uint32_t logic_mode = 3;   // GX_LO_*: 0 clear, 1 and, 2 and-reverse, 3 copy, 4 and-inverted, 5 noop, 6 xor, 7 or,
                               // 8 nor, 9 equiv, 10 invert, 11 or-reverse, 12 copy-inverted, 13 or-inverted, 14 nand, 15 set
    bool color_update = false, alpha_update = false;
    uint32_t src_factor = 0, dst_factor = 0;  // GX_BL_*: 0 zero, 1 one, 2 src/dst color, 3 inverse, 4 src alpha, 5 inverse, 6 dst alpha, 7 inverse
    bool const_alpha = false;
    uint8_t const_alpha_value = 0;
};

BlendState decode_blend(uint32_t cmode0, uint32_t cmode1);

}  // namespace gcrecomp::gp
