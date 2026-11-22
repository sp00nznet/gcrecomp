// SPDX-License-Identifier: MIT
// Round-trips synthetic images through encode_efb_copy and the existing decode_texture.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "efb_copy.h"
#include "gcrecomp/gx/gx.h"

using namespace gcrecomp;
using namespace gcrecomp::gp;

namespace gcrecomp::gx {
std::vector<uint8_t> decode_texture(const uint8_t* src, uint32_t width, uint32_t height, GXTexFmt fmt);
}

static int g_failures = 0;
#define CHECK(cond, ...)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);  \
            std::fprintf(stderr, __VA_ARGS__);                                    \
            std::fprintf(stderr, "\n");                                           \
        }                                                                         \
    } while (0)

// Build the copy command: format nibble split over bits 3 and 4-6, plus flags.
static uint32_t cmd(uint8_t fmt, bool intensity = false, bool half = false, bool yscale = false) {
    return (uint32_t(fmt & 7) << 4) | (uint32_t(fmt >> 3) << 3) | (uint32_t(intensity) << 15) | (uint32_t(half) << 9) |
           (uint32_t(yscale) << 10);
}

static EfbCopyParams params(uint32_t command, int w, int h, uint32_t yscale_reg = 0x100, bool depth = false) {
    EfbCopyParams p = make_efb_copy_params(command, 0, uint32_t(w - 1) | (uint32_t(h - 1) << 10), 0, yscale_reg, depth);
    int ow, oh, tw, th, tb;
    efb_copy_output_size(p, &ow, &oh);
    efb_copy_tile_geometry(p, &tw, &th, &tb);
    p.stride = uint32_t((ow + tw - 1) / tw * tb);  // tightly packed, as decode_texture expects
    return p;
}

static std::vector<uint8_t> encode(const EfbCopyParams& p, const std::vector<uint8_t>& rgba,
                                   const std::vector<uint32_t>* z = nullptr) {
    std::vector<uint8_t> out(efb_copy_extent(p) + 64, 0xAB);  // sentinel to catch overruns
    encode_efb_copy(p, rgba.data(), z ? z->data() : nullptr, out.data());
    for (size_t i = efb_copy_extent(p); i < out.size(); ++i) CHECK(out[i] == 0xAB, "wrote past the extent at %zu", i);
    return out;
}

// Image whose channels are all exactly representable in the narrow formats, non-multiple-of-tile size.
static std::vector<uint8_t> make_image(int w, int h) {
    std::vector<uint8_t> img(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* px = &img[(size_t(y) * w + x) * 4];
            px[0] = uint8_t(((x * 5 + y) & 31) * 255 / 31);
            px[1] = uint8_t(((x + y * 7) & 31) * 255 / 31);
            px[2] = uint8_t(((x * 3 + y * 2) & 31) * 255 / 31);
            px[3] = uint8_t(((x + y) & 7) * 255 / 7);
        }
    return img;
}

static void roundtrip(const char* name, uint8_t fmt, gx::GXTexFmt gxfmt, bool intensity,
                      uint8_t (*expect)(const uint8_t* px, int channel)) {
    const int w = 21, h = 13;
    const std::vector<uint8_t> img = make_image(w, h);
    const EfbCopyParams p = params(cmd(fmt, intensity), w, h);
    const std::vector<uint8_t> tex = encode(p, img);
    const std::vector<uint8_t> dec = gx::decode_texture(tex.data(), w, h, gxfmt);
    for (int i = 0; i < w * h; ++i)
        for (int c = 0; c < 4; ++c)
            CHECK(dec[i * 4 + c] == expect(&img[i * 4], c), "%s pixel %d (%d,%d) channel %d: got %u want %u", name, i,
                  i % w, i / w, c, dec[i * 4 + c], expect(&img[i * 4], c));
}

int main() {
    roundtrip("RGBA8", 6, gx::GX_TF_RGBA8, false, [](const uint8_t* px, int c) { return px[c]; });
    roundtrip("RGB565", 4, gx::GX_TF_RGB565, false, [](const uint8_t* px, int c) -> uint8_t {
        if (c == 3) return 255;
        // Quantize to the format and expand the way decode_texture does.
        return c == 1 ? uint8_t((px[1] >> 2) * 255 / 63) : uint8_t((px[c] >> 3) * 255 / 31);
    });
    roundtrip("RGB5A3", 5, gx::GX_TF_RGB5A3, false, [](const uint8_t* px, int c) -> uint8_t {
        if (px[3] >= 0xE0) return c == 3 ? 255 : uint8_t((px[c] >> 3) * 255 / 31);
        if (c == 3) return uint8_t((px[3] >> 5) * 255 / 7);
        return uint8_t((px[c] >> 4) * 255 / 15);
    });
    roundtrip("IA8", 3, gx::GX_TF_IA8, false, [](const uint8_t* px, int c) { return c == 3 ? px[3] : px[0]; });
    roundtrip("I8", 1, gx::GX_TF_I8, false, [](const uint8_t* px, int c) -> uint8_t { return c == 3 ? 255 : px[0]; });
    roundtrip("IA4", 2, gx::GX_TF_IA4, false, [](const uint8_t* px, int c) -> uint8_t {
        return uint8_t((c == 3 ? px[3] >> 4 : px[0] >> 4) * 17);
    });
    roundtrip("I4", 0, gx::GX_TF_I4, false, [](const uint8_t* px, int c) -> uint8_t { return c == 3 ? 255 : uint8_t((px[0] >> 4) * 17); });

    // Intensity copies use luma: white stays white, black stays black (studio swing: 16 to 235).
    {
        const std::vector<uint8_t> img = {255, 255, 255, 255, 0, 0, 0, 255, 0, 255, 0, 255, 255, 0, 0, 255};
        const EfbCopyParams p = params(cmd(1, true), 4, 1);
        const std::vector<uint8_t> dec = gx::decode_texture(encode(p, img).data(), 4, 1, gx::GX_TF_I8);
        CHECK(dec[0] == 235, "white luma %u", dec[0]);
        CHECK(dec[4] == 16, "black luma %u", dec[4]);
        CHECK(dec[8] > dec[12], "green brighter than red: %u %u", dec[8], dec[12]);
    }

    // Stride: tile rows are `stride` apart, not tightly packed.
    {
        const int w = 8, h = 8;
        const std::vector<uint8_t> img = make_image(w, h);
        EfbCopyParams p = params(cmd(4), w, h);
        const uint32_t packed = p.stride;
        p.stride = packed + 96;
        const std::vector<uint8_t> out = encode(p, img);
        CHECK(out.size() >= efb_copy_extent(p), "extent");
        // Re-pack the two tile rows and decode.
        std::vector<uint8_t> tight;
        for (int row = 0; row < 2; ++row) tight.insert(tight.end(), out.begin() + row * p.stride, out.begin() + row * p.stride + packed);
        const std::vector<uint8_t> dec = gx::decode_texture(tight.data(), w, h, gx::GX_TF_RGB565);
        CHECK(dec[(7 * w + 7) * 4] == uint8_t((img[(7 * w + 7) * 4] >> 3) * 255 / 31), "stride lost the second tile row");
    }

    // Half scale: 2x2 box filter.
    {
        std::vector<uint8_t> img(4 * 2 * 4, 0);
        for (int i = 0; i < 4 * 2; ++i) { img[i * 4] = (i % 2 == 0) ? 200 : 100; img[i * 4 + 3] = 255; }  // R alternates per column
        const EfbCopyParams p = params(cmd(6, false, true), 4, 2);
        int ow, oh;
        efb_copy_output_size(p, &ow, &oh);
        CHECK(ow == 2 && oh == 1, "half-scale size %dx%d", ow, oh);
        const std::vector<uint8_t> dec = gx::decode_texture(encode(p, img).data(), ow, oh, gx::GX_TF_RGBA8);
        CHECK(dec[0] == 150 && dec[4] == 150, "box average %u %u", dec[0], dec[4]);
    }

    // Y scale register (9 bits): 256/reg is the vertical factor; 0x1C0 shrinks 14 rows to 8, 0x80 doubles.
    {
        const EfbCopyParams half = params(cmd(6, false, false, true), 8, 14, 0x1C0), twice = params(cmd(6, false, false, true), 8, 8, 0x80);
        int ow, oh;
        efb_copy_output_size(half, &ow, &oh);
        CHECK(ow == 8 && oh == 8, "yscale 0x1C0 gives %dx%d", ow, oh);
        efb_copy_output_size(twice, &ow, &oh);
        CHECK(ow == 8 && oh == 16, "yscale 0x80 gives %dx%d", ow, oh);
        // Without the enable bit the register is ignored.
        const EfbCopyParams off = params(cmd(6), 8, 8, 0x1C0);
        efb_copy_output_size(off, &ow, &oh);
        CHECK(oh == 8, "yscale without enable gives height %d", oh);
        // Stretching repeats rows: top half of the output is the first source row.
        std::vector<uint8_t> img(8 * 2 * 4, 255);
        for (int x = 0; x < 8; ++x) img[x * 4] = 10;   // row 0 red = 10
        for (int x = 0; x < 8; ++x) img[(8 + x) * 4] = 90;  // row 1 red = 90
        const EfbCopyParams st = params(cmd(6, false, false, true), 8, 2, 0x80);
        const std::vector<uint8_t> dec = gx::decode_texture(encode(st, img).data(), 8, 4, gx::GX_TF_RGBA8);
        CHECK(dec[0] == 10 && dec[(1 * 8) * 4] == 10 && dec[(2 * 8) * 4] == 90 && dec[(3 * 8) * 4] == 90, "stretched rows %u %u %u %u",
              dec[0], dec[32], dec[64], dec[96]);
    }

    // Z formats.
    {
        const int w = 8, h = 4;
        std::vector<uint32_t> z(w * h);
        for (int i = 0; i < w * h; ++i) z[i] = 0x123456u + uint32_t(i) * 0x010203u;
        const std::vector<uint8_t> dummy(w * h * 4, 0);
        auto z8 = encode(params(cmd(kZ8), w, h, 0x100, true), dummy, &z);
        CHECK(z8[0] == 0x12 && z8[1] == 0x13, "Z8 %02X %02X", z8[0], z8[1]);
        auto z8l = encode(params(cmd(kZ8L), w, h, 0x100, true), dummy, &z);
        CHECK(z8l[0] == 0x56, "Z8L %02X", z8l[0]);
        auto z16 = encode(params(cmd(kZ16), 4, 4, 0x100, true), dummy, &z);
        CHECK(z16[0] == 0x12 && z16[1] == 0x34, "Z16 %02X%02X", z16[0], z16[1]);
        auto z24 = encode(params(cmd(kZ24X8), 4, 4, 0x100, true), dummy, &z);
        CHECK(z24[0] == 0xFF && z24[1] == 0x12 && z24[32] == 0x34 && z24[33] == 0x56, "Z24X8 %02X %02X %02X %02X", z24[0], z24[1], z24[32], z24[33]);
        auto z4 = encode(params(cmd(kZ4), 8, 8, 0x100, true), dummy, &z);
        CHECK(z4[0] >> 4 == 1, "Z4 %02X", z4[0]);
    }

    // Command decoding.
    {
        const EfbCopyParams p = make_efb_copy_params(cmd(0xB, true, true, true) | (1u << 11), 3 | (5u << 10), 9 | (6u << 10), 7, 0x155, false);
        CHECK(p.format == 0xB && p.intensity && p.half_scale && p.y_scale, "flags");
        CHECK(p.x == 3 && p.y == 5 && p.w == 10 && p.h == 7 && p.stride == 224 && p.y_scale_reg == 0x155, "fields");
    }

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("efb_copy_test: all passed");
    return 0;
}
