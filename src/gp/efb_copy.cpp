// SPDX-License-Identifier: MIT
#include "efb_copy.h"

#include <algorithm>
#include <cstring>

namespace gcrecomp::gp {

namespace {

struct Texel {
    uint8_t r, g, b, a;
    uint32_t z;
};

uint8_t luma(const Texel& t) {
    // ITU-R BT.601 studio-swing luma, as the hardware's RGB-to-YUV matrix.
    const int y = (257 * t.r + 504 * t.g + 98 * t.b) / 1000 + 16;
    return uint8_t(std::min(y, 255));
}

// Output pixels per format class.
enum class Layout { Nibble8x8, Byte8x4, Word4x4, Rgba4x4 };

Layout layout_of(const EfbCopyParams& p) {
    const uint8_t f = p.format;
    if (p.depth) {
        switch (f) {
            case kZ4: return Layout::Nibble8x8;
            case kZ8: case kZ8M: case kZ8L: return Layout::Byte8x4;
            case kZ16: case kZ16L: return Layout::Word4x4;
            default: return Layout::Rgba4x4;
        }
    }
    switch (EfbCopyFormat(f)) {
        case EfbCopyFormat::I4: return Layout::Nibble8x8;
        case EfbCopyFormat::I8: case EfbCopyFormat::IA4: case EfbCopyFormat::A8: case EfbCopyFormat::R8:
        case EfbCopyFormat::G8: case EfbCopyFormat::B8: return Layout::Byte8x4;
        case EfbCopyFormat::RGBA8: return Layout::Rgba4x4;
        default: return Layout::Word4x4;
    }
}

// One destination texel as up to 4 bytes (for the Word4x4 and Byte8x4 layouts) or a nibble.
uint32_t texel_value(const EfbCopyParams& p, const Texel& t) {
    if (p.depth) {
        switch (p.format) {
            case kZ4: return t.z >> 20;
            case kZ8: return t.z >> 16;
            case kZ8M: return (t.z >> 8) & 0xFF;
            case kZ8L: return t.z & 0xFF;
            case kZ16: return t.z >> 8;
            case kZ16L: return t.z & 0xFFFF;
            default: return t.z;
        }
    }
    switch (EfbCopyFormat(p.format)) {
        case EfbCopyFormat::I4: return (p.intensity ? luma(t) : t.r) >> 4;
        case EfbCopyFormat::I8: return p.intensity ? luma(t) : t.r;
        case EfbCopyFormat::IA4: return ((t.a >> 4) << 4) | ((p.intensity ? luma(t) : t.r) >> 4);
        case EfbCopyFormat::IA8: return (uint32_t(t.a) << 8) | (p.intensity ? luma(t) : t.r);
        case EfbCopyFormat::RGB565: return (uint32_t(t.r >> 3) << 11) | (uint32_t(t.g >> 2) << 5) | (t.b >> 3);
        case EfbCopyFormat::RGB5A3:
            if (t.a >= 0xE0) return 0x8000u | (uint32_t(t.r >> 3) << 10) | (uint32_t(t.g >> 3) << 5) | (t.b >> 3);
            return (uint32_t(t.a >> 5) << 12) | (uint32_t(t.r >> 4) << 8) | (uint32_t(t.g >> 4) << 4) | (t.b >> 4);
        case EfbCopyFormat::A8: return t.a;
        case EfbCopyFormat::R8: return t.r;
        case EfbCopyFormat::G8: return t.g;
        case EfbCopyFormat::B8: return t.b;
        case EfbCopyFormat::RG8: return (uint32_t(t.r) << 8) | t.g;
        case EfbCopyFormat::GB8: return (uint32_t(t.g) << 8) | t.b;
        default: return 0;
    }
}

}  // namespace

EfbCopyParams make_efb_copy_params(uint32_t copy_cmd, uint32_t src_xy, uint32_t src_wh, uint32_t stride_reg,
                                   uint32_t y_scale_reg, bool depth) {
    EfbCopyParams p;
    p.x = int(src_xy & 0x3FF);
    p.y = int((src_xy >> 10) & 0x3FF);
    p.w = int(src_wh & 0x3FF) + 1;
    p.h = int((src_wh >> 10) & 0x3FF) + 1;
    // Bits 4-6 hold the low three bits of the format and bit 3 the fourth.
    p.format = uint8_t(((copy_cmd >> 4) & 7) | (((copy_cmd >> 3) & 1) << 3));
    p.half_scale = (copy_cmd >> 9) & 1;
    p.y_scale = (copy_cmd >> 10) & 1;
    p.intensity = (copy_cmd >> 15) & 1;
    p.y_scale_reg = y_scale_reg & 0x1FF;
    p.depth = depth;
    p.stride = (stride_reg & 0x3FF) * 32;
    return p;
}

void efb_copy_output_size(const EfbCopyParams& p, int* out_w, int* out_h) {
    int w = p.w, h = p.h;
    if (p.half_scale) {
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
    }
    if (p.y_scale && p.y_scale_reg != 0) h = std::max(int((uint64_t(h) * 256 + p.y_scale_reg / 2) / p.y_scale_reg), 1);
    *out_w = w;
    *out_h = h;
}

void efb_copy_tile_geometry(const EfbCopyParams& p, int* tw, int* th, int* tb) {
    switch (layout_of(p)) {
        case Layout::Nibble8x8: *tw = 8; *th = 8; *tb = 32; break;
        case Layout::Byte8x4: *tw = 8; *th = 4; *tb = 32; break;
        case Layout::Word4x4: *tw = 4; *th = 4; *tb = 32; break;
        case Layout::Rgba4x4: *tw = 4; *th = 4; *tb = 64; break;
    }
}

size_t efb_copy_extent(const EfbCopyParams& p) {
    int ow, oh, tw, th, tb;
    efb_copy_output_size(p, &ow, &oh);
    efb_copy_tile_geometry(p, &tw, &th, &tb);
    const size_t rows = size_t(oh + th - 1) / th, cols = size_t(ow + tw - 1) / tw;
    return (rows - 1) * p.stride + cols * size_t(tb);
}

void encode_efb_copy(const EfbCopyParams& p, const uint8_t* rgba, const uint32_t* z, uint8_t* dst) {
    int ow, oh, tw, th, tb;
    efb_copy_output_size(p, &ow, &oh);
    efb_copy_tile_geometry(p, &tw, &th, &tb);
    const Layout layout = layout_of(p);

    // Destination texel (x, y): 2x2 box filter when half-scaled, then nearest row for the Y scale.
    auto fetch = [&](int x, int y) {
        int sy = y;
        if (p.y_scale && p.y_scale_reg != 0) {
            const int hh = p.half_scale ? std::max(p.h / 2, 1) : p.h;
            sy = std::min(int((uint64_t(y) * hh) / oh), hh - 1);
        }
        Texel t{};
        const int n = p.half_scale ? 2 : 1;
        uint32_t sum[4] = {};
        for (int dy = 0; dy < n; ++dy) {
            for (int dx = 0; dx < n; ++dx) {
                const size_t i = size_t(std::min(sy * n + dy, p.h - 1)) * p.w + std::min(x * n + dx, p.w - 1);
                for (int c = 0; c < 4; ++c) sum[c] += rgba ? rgba[i * 4 + c] : 0;
            }
        }
        const uint32_t cnt = uint32_t(n * n);
        t.r = uint8_t(sum[0] / cnt); t.g = uint8_t(sum[1] / cnt); t.b = uint8_t(sum[2] / cnt); t.a = uint8_t(sum[3] / cnt);
        if (z) t.z = z[size_t(std::min(sy * n, p.h - 1)) * p.w + std::min(x * n, p.w - 1)] & 0xFFFFFF;
        return t;
    };

    const int cols = (ow + tw - 1) / tw, rows = (oh + th - 1) / th;
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            uint8_t* tile = dst + size_t(ty) * p.stride + size_t(tx) * tb;
            std::memset(tile, 0, size_t(tb));
            for (int py = 0; py < th; ++py) {
                for (int px = 0; px < tw; ++px) {
                    const int x = tx * tw + px, y = ty * th + py;
                    if (x >= ow || y >= oh) continue;
                    const Texel t = fetch(x, y);
                    const int n = py * tw + px;
                    switch (layout) {
                        case Layout::Nibble8x8: {
                            const uint8_t v = uint8_t(texel_value(p, t) & 0xF);
                            uint8_t& b = tile[n / 2];
                            b = (n & 1) ? uint8_t((b & 0xF0) | v) : uint8_t((b & 0x0F) | (v << 4));
                            break;
                        }
                        case Layout::Byte8x4: tile[n] = uint8_t(texel_value(p, t)); break;
                        case Layout::Word4x4: {
                            const uint32_t v = texel_value(p, t);
                            tile[n * 2] = uint8_t(v >> 8);
                            tile[n * 2 + 1] = uint8_t(v);
                            break;
                        }
                        case Layout::Rgba4x4: {
                            uint8_t a = t.a, r = t.r, g = t.g, b = t.b;
                            if (p.depth) { a = 0xFF; r = uint8_t(t.z >> 16); g = uint8_t(t.z >> 8); b = uint8_t(t.z); }
                            tile[n * 2] = a;
                            tile[n * 2 + 1] = r;
                            tile[32 + n * 2] = g;
                            tile[32 + n * 2 + 1] = b;
                            break;
                        }
                    }
                }
            }
        }
    }
}

}  // namespace gcrecomp::gp
