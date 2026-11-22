// SPDX-License-Identifier: MIT
// Unit tests for the pure GP helpers (src/gp/gp_decode.*): no display, no disc.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gp_decode.h"

using namespace gcrecomp::gp;

static int g_failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

// TLUT with entry i = RGB5A3 translucent, A3 = 7, R4 = i>>8 &15, G4 = i>>4 &15, B4 = i&15 (so every
// index below 4096 maps to a distinct color), 16384 entries.
static std::vector<uint8_t> make_tlut(uint32_t base_units) {
    std::vector<uint8_t> t(size_t(base_units) * 512 + 16384 * 2, 0);
    for (uint32_t i = 0; i < 16384; ++i) {
        const uint16_t e = uint16_t(0x7000 | (((i >> 8) & 15) << 8) | (((i >> 4) & 15) << 4) | (i & 15));
        t[size_t(base_units) * 512 + i * 2] = uint8_t(e >> 8);
        t[size_t(base_units) * 512 + i * 2 + 1] = uint8_t(e);
    }
    return t;
}

static void test_c14x2() {
    // 6x5 texture: 2x2 tiles of 4x4 texels, 32 bytes each; texel (x, y) gets index 0x100 + 37*y + x
    // plus the two reserved top bits set, which must be ignored.
    const uint32_t w = 6, h = 5;
    std::vector<uint8_t> src(texture_bytes(w, h, kTexC14X2), 0xFF);
    CHECK(src.size() == 4 * 32);
    auto index_of = [](uint32_t x, uint32_t y) { return 0x100u + 37 * y + x; };
    for (uint32_t ty = 0; ty < 2; ++ty)
        for (uint32_t tx = 0; tx < 2; ++tx)
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t px = tx * 4 + x, py = ty * 4 + y;
                    const uint16_t v = uint16_t(0xC000 | index_of(px, py));
                    uint8_t* e = &src[(ty * 2 + tx) * 32 + (y * 4 + x) * 2];
                    e[0] = uint8_t(v >> 8);
                    e[1] = uint8_t(v);
                }
    const uint32_t units = 2;  // TLUT at 1 KiB
    const std::vector<uint8_t> tlut = make_tlut(units);
    const uint32_t settlut = units | (2u << 10);  // RGB5A3
    const std::vector<uint8_t> out = decode_indexed(tlut.data(), tlut.size(), src.data(), w, h, kTexC14X2, settlut);
    CHECK(out.size() == size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t i = index_of(x, y);
            const uint8_t* px = &out[(size_t(y) * w + x) * 4];
            CHECK(px[0] == ((i >> 8) & 15) * 17);
            CHECK(px[1] == ((i >> 4) & 15) * 17);
            CHECK(px[2] == (i & 15) * 17);
            CHECK(px[3] == 255);
        }
}

static void test_c14x2_out_of_range() {
    // An index past the end of the TLUT must not read out of bounds.
    std::vector<uint8_t> src(32, 0);
    src[0] = 0x3F;
    src[1] = 0xFF;  // index 16383
    const std::vector<uint8_t> tlut(64, 0xFF);
    const std::vector<uint8_t> out = decode_indexed(tlut.data(), tlut.size(), src.data(), 4, 4, kTexC14X2, 0);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
    CHECK(out[4] == 255);  // index 0 is inside the table
}

static void test_c8_still_works() {
    // C8 tiles are 8x4; texel (x, y) holds index x + 8*y in a 8x4 texture.
    std::vector<uint8_t> src(32);
    for (int i = 0; i < 32; ++i) src[i] = uint8_t(i);
    const std::vector<uint8_t> tlut = make_tlut(0);
    const std::vector<uint8_t> out = decode_indexed(tlut.data(), tlut.size(), src.data(), 8, 4, kTexC8, 2u << 10);
    for (uint32_t i = 0; i < 32; ++i) CHECK(out[i * 4 + 2] == (i & 15) * 17);
}

static void test_mip_levels() {
    // TX_SETMODE0 min filter field is bits 5-7; TX_SETMODE1 max LOD is bits 8-15 (4.4 fixed point).
    auto mode0 = [](uint32_t filter) { return filter << 5; };
    auto mode1 = [](uint32_t max_lod_x16) { return max_lod_x16 << 8; };
    // Non-mipmap filters never load more than one level, whatever the max LOD says.
    CHECK(mip_level_count(64, 64, mode0(0), mode1(0x50)) == 1);
    CHECK(mip_level_count(64, 64, mode0(4), mode1(0x50)) == 1);
    // GX_LIN_MIP_LIN with maxLOD = 5 on a 64x64 texture: levels 0..5.
    CHECK(mip_level_count(64, 64, mode0(6), mode1(0x50)) == 6);
    // A fractional max LOD rounds up (LOD 2.5 samples between levels 2 and 3).
    CHECK(mip_level_count(64, 64, mode0(6), mode1(0x28)) == 4);
    // Capped by the size: 64x16 has 7 levels (64..1), a max LOD of 15.9 does not ask for more.
    CHECK(mip_level_count(64, 16, mode0(5), mode1(0xFF)) == 7);
    CHECK(mip_level_count(1, 1, mode0(6), mode1(0x50)) == 1);
    // Mipmap filters degrade to their base filter with a single level.
    CHECK(effective_min_filter(mode0(6), 1) == MinFilter::Linear);
    CHECK(effective_min_filter(mode0(2), 1) == MinFilter::Nearest);
    CHECK(effective_min_filter(mode0(6), 3) == MinFilter::LinearMipLinear);
    CHECK(effective_min_filter(mode0(1), 3) == MinFilter::NearestMipNearest);
    CHECK(effective_min_filter(mode0(2), 3) == MinFilter::NearestMipLinear);
    CHECK(effective_min_filter(mode0(5), 3) == MinFilter::LinearMipNearest);
    CHECK(effective_min_filter(mode0(0), 3) == MinFilter::Nearest);
}

static void test_mip_layout() {
    // 32x16 CMPR (8x8 tiles of 32 bytes): 4x2 tiles, then 16x8 (2x1), 8x4 (1x1 after padding), 4x2, 2x1, 1x1.
    CHECK(texture_bytes(32, 16, 14) == 8 * 32);
    CHECK(mip_level_offset(32, 16, 14, 0) == 0);
    CHECK(mip_level_offset(32, 16, 14, 1) == 8 * 32);
    CHECK(mip_level_offset(32, 16, 14, 2) == 8 * 32 + 2 * 32);
    CHECK(mip_level_offset(32, 16, 14, 3) == 8 * 32 + 2 * 32 + 32);
    CHECK(mip_chain_bytes(32, 16, 14, 6) == 8 * 32 + 2 * 32 + 4 * 32);
    // RGBA8 uses 64-byte tiles; levels below the tile size still take one tile each.
    CHECK(mip_chain_bytes(8, 8, 6, 4) == (4 + 1 + 1 + 1) * 64);
    // The dimension never drops below 1 for non-square textures.
    CHECK(mip_dim(16, 4) == 1 && mip_dim(4, 4) == 1 && mip_dim(4, 1) == 2);
}

static void test_lod_fields() {
    CHECK(lod_bias_of(0x10u << 9) == 0.5f);                 // +16/32
    CHECK(lod_bias_of(0xF0u << 9) == -0.5f);                // two's complement -16/32
    CHECK(lod_bias_of(0x80u << 9) == -4.0f);
    CHECK(lod_bias_of(0x7Fu << 9) == 127.0f / 32.0f);
    CHECK(min_lod_of(0x18) == 1.5f);
    CHECK(max_lod_of(0x2800) == 2.5f);
}

int main() {
    test_c14x2();
    test_c14x2_out_of_range();
    test_c8_still_works();
    test_mip_levels();
    test_mip_layout();
    test_lod_fields();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("gp_decode_test: all passed\n");
    return 0;
}
