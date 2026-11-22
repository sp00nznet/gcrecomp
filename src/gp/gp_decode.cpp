// SPDX-License-Identifier: MIT
#include "gp_decode.h"

#include "gcrecomp/gx/gx.h"

namespace gcrecomp::gx {
// Defined in src/gx/gx_texture.cpp.
std::vector<uint8_t> decode_texture(const uint8_t* src, uint32_t width, uint32_t height, GXTexFmt fmt);
}  // namespace gcrecomp::gx

namespace gcrecomp::gp {

size_t texture_bytes(uint32_t w, uint32_t h, uint32_t fmt) {
    uint32_t bw, bh, bytes;
    switch (fmt) {
        case 0: case 8: case 14: bw = 8; bh = 8; bytes = 32; break;   // I4, C4, CMPR
        case 1: case 2: case 9: bw = 8; bh = 4; bytes = 32; break;    // I8, IA4, C8
        case 6: bw = 4; bh = 4; bytes = 64; break;                    // RGBA8
        default: bw = 4; bh = 4; bytes = 32; break;                   // 16-bit formats, C14X2
    }
    return size_t((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

void tlut_entry_to_rgba(uint32_t e, uint32_t pal_fmt, uint8_t out[4]) {
    uint8_t r, g, b, a;
    if (pal_fmt == 0) {  // IA8: alpha in the high byte, intensity in the low byte
        a = uint8_t(e >> 8);
        r = g = b = uint8_t(e);
    } else if (pal_fmt == 1) {  // RGB565
        r = uint8_t(((e >> 11) & 31) * 255 / 31);
        g = uint8_t(((e >> 5) & 63) * 255 / 63);
        b = uint8_t((e & 31) * 255 / 31);
        a = 255;
    } else if (e & 0x8000) {  // RGB5A3, opaque: RGB555
        r = uint8_t(((e >> 10) & 31) * 255 / 31);
        g = uint8_t(((e >> 5) & 31) * 255 / 31);
        b = uint8_t((e & 31) * 255 / 31);
        a = 255;
    } else {  // RGB5A3, translucent: A3 R4 G4 B4
        a = uint8_t(((e >> 12) & 7) * 255 / 7);
        r = uint8_t(((e >> 8) & 15) * 17);
        g = uint8_t(((e >> 4) & 15) * 17);
        b = uint8_t((e & 15) * 17);
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
}

namespace {

// C14X2 stores one 16-bit big-endian entry per texel in 4x4 tiles; the low 14 bits are the index.
std::vector<uint16_t> read_c14x2_indices(const uint8_t* src, uint32_t w, uint32_t h) {
    std::vector<uint16_t> idx(size_t(w) * h);
    const uint32_t tiles_x = (w + 3) / 4, tiles_y = (h + 3) / 4;
    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            const uint8_t* tile = src + (size_t(ty) * tiles_x + tx) * 32;
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t px = tx * 4 + x, py = ty * 4 + y;
                    if (px < w && py < h)
                        idx[size_t(py) * w + px] = uint16_t(((tile[(y * 4 + x) * 2] << 8) | tile[(y * 4 + x) * 2 + 1]) & 0x3FFF);
                }
            }
        }
    }
    return idx;
}

}  // namespace

std::vector<uint8_t> decode_indexed(const uint8_t* tlut, size_t tlut_size, const uint8_t* src, uint32_t w,
                                    uint32_t h, uint32_t fmt, uint32_t settlut) {
    const size_t texels = size_t(w) * h;
    std::vector<uint16_t> idx(texels);
    if (fmt == kTexC14X2) {
        idx = read_c14x2_indices(src, w, h);
    } else {
        // C4 and C8 use the I4 and I8 tile layouts: decode as intensity to recover the indices.
        const bool c4 = fmt == kTexC4;
        const std::vector<uint8_t> raw = gx::decode_texture(src, w, h, c4 ? gx::GX_TF_I4 : gx::GX_TF_I8);
        for (size_t p = 0; p < texels; ++p) idx[p] = c4 ? raw[p * 4] / 17 : raw[p * 4];
    }

    const size_t base = size_t(settlut & 0x3FF) * 512;
    const uint32_t pal_fmt = (settlut >> 10) & 3;
    std::vector<uint8_t> out(texels * 4, 0);
    for (size_t p = 0; p < texels; ++p) {
        const size_t off = base + size_t(idx[p]) * 2;
        if (off + 2 > tlut_size) continue;
        tlut_entry_to_rgba(uint32_t(tlut[off] << 8) | tlut[off + 1], pal_fmt, &out[p * 4]);
    }
    return out;
}

}  // namespace gcrecomp::gp
