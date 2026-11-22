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

}  // namespace gcrecomp::gp
