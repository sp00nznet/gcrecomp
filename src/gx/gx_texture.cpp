// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// GX Texture Decoding
//
// GameCube textures are stored in tiled (swizzled) formats optimized for
// the GPU's texture cache. Each format has a specific tile size:
//
//   Format    | BPP | Tile Size | Description
//   ----------|-----|-----------|---------------------------------------------
//   I4        |  4  |   8x8    | 4-bit intensity (grayscale)
//   I8        |  8  |   8x4    | 8-bit intensity (grayscale)
//   IA4       |  8  |   8x4    | 4-bit intensity + 4-bit alpha
//   IA8       | 16  |   4x4    | 8-bit intensity + 8-bit alpha
//   RGB565    | 16  |   4x4    | 16-bit color (5-6-5 bits)
//   RGB5A3    | 16  |   4x4    | 16-bit: RGB555 (opaque) or RGB4A3 (transparent)
//   RGBA8     | 32  |   4x4    | 32-bit RGBA, stored as AR+GB tile pairs
//   C4        |  4  |   8x8    | 4-bit paletted (16 colors via TLUT)
//   C8        |  8  |   8x4    | 8-bit paletted (256 colors via TLUT)
//   C14X2     | 16  |   4x4    | 14-bit paletted (16384 colors via TLUT)
//   CMPR      |  4  |   8x8    | S3TC/DXT1 variant (4x4 sub-blocks)
//
// All tiles are 32 bytes (one cache line) except RGBA8 which uses two
// 32-byte cache lines per tile (AR data + GB data).
//
// The tile layout ensures that spatially adjacent texels are stored
// close together in memory, maximizing texture cache hit rates.
//
// CMPR is Nintendo's variant of S3TC/DXT1 block compression. It uses
// the same 4x4 block size and 2-bit index table, but the byte order
// within blocks is big-endian (matching the GameCube's PowerPC CPU).
// Each 8x8 macro tile contains 4 DXT1 blocks in a 2x2 arrangement.
//
// References:
//   - libogc — Texture format enum values (GXTexFmt)
//   - Pureikyubu — Texture memory layout and tile organization
//   - YAGCD (Yet Another GameCube Document) — Texture format details
// =============================================================================

#include "gcrecomp/gx/gx.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gcrecomp::gx {

// Decode a GameCube texture to RGBA8888
// Returns width*height*4 bytes of RGBA data
std::vector<uint8_t> decode_texture(const uint8_t* src, uint32_t width, uint32_t height, GXTexFmt fmt) {
    std::vector<uint8_t> rgba(width * height * 4, 0xFF);

    switch (fmt) {
    case GX_TF_I4: {
        // 8x8 tiles, 4 bits per pixel
        uint32_t tiles_x = (width + 7) / 8;
        uint32_t tiles_y = (height + 7) / 8;
        uint32_t src_pos = 0;

        for (uint32_t ty = 0; ty < tiles_y; ty++) {
            for (uint32_t tx = 0; tx < tiles_x; tx++) {
                for (uint32_t py = 0; py < 8; py++) {
                    for (uint32_t px = 0; px < 8; px += 2) {
                        uint32_t x = tx * 8 + px;
                        uint32_t y = ty * 8 + py;
                        uint8_t byte = src[src_pos++];

                        for (int n = 0; n < 2; n++) {
                            uint8_t val = (n == 0) ? (byte >> 4) : (byte & 0xF);
                            val = val | (val << 4);
                            uint32_t xx = x + n;
                            if (xx < width && y < height) {
                                uint32_t idx = (y * width + xx) * 4;
                                rgba[idx + 0] = val;
                                rgba[idx + 1] = val;
                                rgba[idx + 2] = val;
                                rgba[idx + 3] = 0xFF;
                            }
                        }
                    }
                }
            }
        }
        break;
    }

    case GX_TF_I8: {
        // 8x4 tiles, 8 bits per pixel
        uint32_t tiles_x = (width + 7) / 8;
        uint32_t tiles_y = (height + 3) / 4;
        uint32_t src_pos = 0;

        for (uint32_t ty = 0; ty < tiles_y; ty++) {
            for (uint32_t tx = 0; tx < tiles_x; tx++) {
                for (uint32_t py = 0; py < 4; py++) {
                    for (uint32_t px = 0; px < 8; px++) {
                        uint32_t x = tx * 8 + px;
                        uint32_t y = ty * 4 + py;
                        uint8_t val = src[src_pos++];

                        if (x < width && y < height) {
                            uint32_t idx = (y * width + x) * 4;
                            rgba[idx + 0] = val;
                            rgba[idx + 1] = val;
                            rgba[idx + 2] = val;
                            rgba[idx + 3] = 0xFF;
                        }
                    }
                }
            }
        }
        break;
    }

    case GX_TF_IA4: {
        // 8x4 tiles, 8 bits per pixel (4-bit intensity + 4-bit alpha)
        uint32_t tiles_x = (width + 7) / 8;
        uint32_t tiles_y = (height + 3) / 4;
        uint32_t src_pos = 0;

        for (uint32_t ty = 0; ty < tiles_y; ty++) {
            for (uint32_t tx = 0; tx < tiles_x; tx++) {
                for (uint32_t py = 0; py < 4; py++) {
                    for (uint32_t px = 0; px < 8; px++) {
                        uint32_t x = tx * 8 + px;
                        uint32_t y = ty * 4 + py;
                        uint8_t byte = src[src_pos++];
                        uint8_t alpha = (byte >> 4) | ((byte >> 4) << 4);
                        uint8_t inten = (byte & 0xF) | ((byte & 0xF) << 4);

                        if (x < width && y < height) {
                            uint32_t idx = (y * width + x) * 4;
                            rgba[idx + 0] = inten;
                            rgba[idx + 1] = inten;
                            rgba[idx + 2] = inten;
                            rgba[idx + 3] = alpha;
                        }
                    }
                }
            }
        }
        break;
    }

    case GX_TF_IA8: {
        // 4x4 tiles, 16 bits per pixel (8-bit intensity + 8-bit alpha)
        uint32_t tiles_x = (width + 3) / 4;
        uint32_t tiles_y = (height + 3) / 4;
        uint32_t src_pos = 0;

        for (uint32_t ty = 0; ty < tiles_y; ty++) {
            for (uint32_t tx = 0; tx < tiles_x; tx++) {
                for (uint32_t py = 0; py < 4; py++) {
                    for (uint32_t px = 0; px < 4; px++) {
                        uint32_t x = tx * 4 + px;
                        uint32_t y = ty * 4 + py;
                        uint8_t alpha = src[src_pos];
                        uint8_t inten = src[src_pos + 1];
                        src_pos += 2;

                        if (x < width && y < height) {
                            uint32_t idx = (y * width + x) * 4;
                            rgba[idx + 0] = inten;
                            rgba[idx + 1] = inten;
                            rgba[idx + 2] = inten;
                            rgba[idx + 3] = alpha;
                        }
                    }
                }
            }
        }
        break;
    }

    case GX_TF_RGB565: {
        // 4x4 tiles, 16 bits per pixel
        uint32_t tiles_x = (width + 3) / 4;
        uint32_t tiles_y = (height + 3) / 4;
        uint32_t src_pos = 0;

        for (uint32_t ty = 0; ty < tiles_y; ty++) {
            for (uint32_t tx = 0; tx < tiles_x; tx++) {
                for (uint32_t py = 0; py < 4; py++) {
                    for (uint32_t px = 0; px < 4; px++) {
                        uint32_t x = tx * 4 + px;
                        uint32_t y = ty * 4 + py;
                        uint16_t pixel = ((uint16_t)src[src_pos] << 8) | src[src_pos + 1];
                        src_pos += 2;

                        uint8_t r = ((pixel >> 11) & 0x1F) * 255 / 31;
                        uint8_t g = ((pixel >> 5) & 0x3F) * 255 / 63;
                        uint8_t b = (pixel & 0x1F) * 255 / 31;

                        if (x < width && y < height) {
                            uint32_t idx = (y * width + x) * 4;
                            rgba[idx + 0] = r;
                            rgba[idx + 1] = g;
                            rgba[idx + 2] = b;
                            rgba[idx + 3] = 0xFF;
                        }
                    }
                }
            }
        }
        break;
    }

    case GX_TF_RGB5A3: {
        uint32_t tiles_x = (width + 3) / 4;
        uint32_t tiles_y = (height + 3) / 4;
        uint32_t src_pos = 0;

        for (uint32_t ty = 0; ty < tiles_y; ty++) {
            for (uint32_t tx = 0; tx < tiles_x; tx++) {
                for (uint32_t py = 0; py < 4; py++) {
                    for (uint32_t px = 0; px < 4; px++) {
                        uint32_t x = tx * 4 + px;
                        uint32_t y = ty * 4 + py;
                        uint16_t pixel = ((uint16_t)src[src_pos] << 8) | src[src_pos + 1];
                        src_pos += 2;

                        uint8_t r, g, b, a;
                        if (pixel & 0x8000) {
                            // RGB555, opaque
                            r = ((pixel >> 10) & 0x1F) * 255 / 31;
                            g = ((pixel >> 5) & 0x1F) * 255 / 31;
                            b = (pixel & 0x1F) * 255 / 31;
                            a = 0xFF;
                        } else {
                            // RGB4A3
                            a = ((pixel >> 12) & 0x7) * 255 / 7;
                            r = ((pixel >> 8) & 0xF) * 255 / 15;
                            g = ((pixel >> 4) & 0xF) * 255 / 15;
                            b = (pixel & 0xF) * 255 / 15;
                        }

                        if (x < width && y < height) {
                            uint32_t idx = (y * width + x) * 4;
                            rgba[idx + 0] = r;
                            rgba[idx + 1] = g;
                            rgba[idx + 2] = b;
                            rgba[idx + 3] = a;
                        }
                    }
                }
            }
        }
        break;
    }

    case GX_TF_RGBA8: {
        // 4x4 tiles, 32 bits per pixel
        // Stored as two 32-byte cache lines per tile:
        //   First line: 16 AR pairs (alpha, red)
        //   Second line: 16 GB pairs (green, blue)
        uint32_t tiles_x = (width + 3) / 4;
        uint32_t tiles_y = (height + 3) / 4;
        uint32_t src_pos = 0;

        for (uint32_t ty = 0; ty < tiles_y; ty++) {
            for (uint32_t tx = 0; tx < tiles_x; tx++) {
                // Read AR block (32 bytes = 16 pixels)
                uint8_t ar[16][2];
                for (int p = 0; p < 16; p++) {
                    ar[p][0] = src[src_pos++]; // A
                    ar[p][1] = src[src_pos++]; // R
                }
                // Read GB block (32 bytes = 16 pixels)
                uint8_t gb[16][2];
                for (int p = 0; p < 16; p++) {
                    gb[p][0] = src[src_pos++]; // G
                    gb[p][1] = src[src_pos++]; // B
                }

                // Write pixels
                for (int p = 0; p < 16; p++) {
                    uint32_t x = tx * 4 + (p % 4);
                    uint32_t y = ty * 4 + (p / 4);
                    if (x < width && y < height) {
                        uint32_t idx = (y * width + x) * 4;
                        rgba[idx + 0] = ar[p][1]; // R
                        rgba[idx + 1] = gb[p][0]; // G
                        rgba[idx + 2] = gb[p][1]; // B
                        rgba[idx + 3] = ar[p][0]; // A
                    }
                }
            }
        }
        break;
    }

    case GX_TF_CMPR: {
        // S3TC/DXT1 variant, 4x4 sub-blocks in 8x8 tiles
        // Each 8x8 tile contains 4 DXT1 blocks (2x2 arrangement)
        uint32_t tiles_x = (width + 7) / 8;
        uint32_t tiles_y = (height + 7) / 8;
        uint32_t src_pos = 0;

        for (uint32_t ty = 0; ty < tiles_y; ty++) {
            for (uint32_t tx = 0; tx < tiles_x; tx++) {
                // 4 DXT1 sub-blocks per tile
                for (int sb = 0; sb < 4; sb++) {
                    uint32_t sub_x = tx * 8 + (sb & 1) * 4;
                    uint32_t sub_y = ty * 8 + (sb >> 1) * 4;

                    // Decode DXT1 block (8 bytes)
                    uint16_t c0 = ((uint16_t)src[src_pos] << 8) | src[src_pos + 1];
                    uint16_t c1 = ((uint16_t)src[src_pos + 2] << 8) | src[src_pos + 3];
                    src_pos += 4;

                    uint8_t colors[4][4]; // [index][RGBA]
                    // Decode color0
                    colors[0][0] = ((c0 >> 11) & 0x1F) * 255 / 31;
                    colors[0][1] = ((c0 >> 5) & 0x3F) * 255 / 63;
                    colors[0][2] = (c0 & 0x1F) * 255 / 31;
                    colors[0][3] = 0xFF;
                    // Decode color1
                    colors[1][0] = ((c1 >> 11) & 0x1F) * 255 / 31;
                    colors[1][1] = ((c1 >> 5) & 0x3F) * 255 / 63;
                    colors[1][2] = (c1 & 0x1F) * 255 / 31;
                    colors[1][3] = 0xFF;

                    if (c0 > c1) {
                        colors[2][0] = (2 * colors[0][0] + colors[1][0]) / 3;
                        colors[2][1] = (2 * colors[0][1] + colors[1][1]) / 3;
                        colors[2][2] = (2 * colors[0][2] + colors[1][2]) / 3;
                        colors[2][3] = 0xFF;
                        colors[3][0] = (colors[0][0] + 2 * colors[1][0]) / 3;
                        colors[3][1] = (colors[0][1] + 2 * colors[1][1]) / 3;
                        colors[3][2] = (colors[0][2] + 2 * colors[1][2]) / 3;
                        colors[3][3] = 0xFF;
                    } else {
                        colors[2][0] = (colors[0][0] + colors[1][0]) / 2;
                        colors[2][1] = (colors[0][1] + colors[1][1]) / 2;
                        colors[2][2] = (colors[0][2] + colors[1][2]) / 2;
                        colors[2][3] = 0xFF;
                        colors[3][0] = 0; colors[3][1] = 0; colors[3][2] = 0; colors[3][3] = 0;
                    }

                    // Read 4 rows of 4 2-bit indices
                    for (int row = 0; row < 4; row++) {
                        uint8_t indices = src[src_pos++];
                        for (int col = 0; col < 4; col++) {
                            uint8_t idx = (indices >> (6 - col * 2)) & 3;
                            uint32_t x = sub_x + col;
                            uint32_t y = sub_y + row;
                            if (x < width && y < height) {
                                uint32_t dst = (y * width + x) * 4;
                                rgba[dst + 0] = colors[idx][0];
                                rgba[dst + 1] = colors[idx][1];
                                rgba[dst + 2] = colors[idx][2];
                                rgba[dst + 3] = colors[idx][3];
                            }
                        }
                    }
                }
            }
        }
        break;
    }

    default:
        printf("[GX] Unsupported texture format: 0x%X\n", fmt);
        break;
    }

    return rgba;
}

} // namespace gcrecomp::gx
