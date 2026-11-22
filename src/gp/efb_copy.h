// SPDX-License-Identifier: MIT
// EFB-to-texture copy: pure format conversion and tiling (no OpenGL, no memory map), so it can be
// unit-tested. The renderer reads the EFB back and feeds the pixels to encode_efb_copy.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gcrecomp::gp {

// Destination formats, the 4-bit "real format" of the copy command (low nibble of the GX_TF_*/GX_CTF_*/
// GX_TF_Z* enum values).
enum class EfbCopyFormat : uint8_t {
    I4 = 0x0,    // intensity, or the R channel when the intensity bit is clear (GX_CTF_R4)
    I8 = 0x1,    // likewise (GX_CTF_R8 is 0x8)
    IA4 = 0x2,   // GX_CTF_RA4 without the intensity bit
    IA8 = 0x3,   // GX_CTF_RA8 without the intensity bit
    RGB565 = 0x4,
    RGB5A3 = 0x5,
    RGBA8 = 0x6,
    A8 = 0x7,
    R8 = 0x8,
    G8 = 0x9,
    B8 = 0xA,
    RG8 = 0xB,
    GB8 = 0xC,
};

// Z formats (depth copies), same nibble: Z4=0, Z8=1, Z16=3, Z24X8=6, Z8M=9, Z8L=0xA, Z16L=0xC.
constexpr uint8_t kZ4 = 0x0, kZ8 = 0x1, kZ16 = 0x3, kZ24X8 = 0x6, kZ8M = 0x9, kZ8L = 0xA, kZ16L = 0xC;

struct EfbCopyParams {
    // Source rectangle in EFB pixels (BP 0x49/0x4A).
    int x = 0, y = 0, w = 0, h = 0;
    uint8_t format = 0;        // 4-bit real format
    bool intensity = false;    // command bit 15: I/IA formats take luma instead of R
    bool half_scale = false;   // command bit 9: 2x2 box filter, halves both dimensions
    bool y_scale = false;      // command bit 10: apply the Y scale register
    uint32_t y_scale_reg = 0;  // BP 0x4E, 256 / vertical scale
    bool depth = false;        // copy Z instead of color
    uint32_t stride = 0;       // destination bytes per row of tiles (BP 0x4D * 32)
};

// Decode the copy command (BP 0x52), the source rectangle registers, the stride register and the
// Y scale register. `depth` is true when the EFB is in Z24 mode (BP 0x43).
EfbCopyParams make_efb_copy_params(uint32_t copy_cmd, uint32_t src_xy, uint32_t src_wh, uint32_t stride_reg,
                                   uint32_t y_scale_reg, bool depth);

// Size in pixels of the destination image.
void efb_copy_output_size(const EfbCopyParams& p, int* out_w, int* out_h);

// Tile geometry of the destination format: pixels per tile and bytes per tile.
void efb_copy_tile_geometry(const EfbCopyParams& p, int* tile_w, int* tile_h, int* tile_bytes);

// Highest byte offset (exclusive) written into the destination, from the destination origin.
size_t efb_copy_extent(const EfbCopyParams& p);

// Convert and tile. `rgba` is p.w*p.h*4 bytes, top row first; `z` is p.w*p.h 24-bit depth values (may be
// null for color copies). Writes tiles to dst + tile_row * p.stride + tile_col * tile_bytes. Partial
// edge tiles are padded with zeros.
void encode_efb_copy(const EfbCopyParams& p, const uint8_t* rgba, const uint32_t* z, uint8_t* dst);

}  // namespace gcrecomp::gp
