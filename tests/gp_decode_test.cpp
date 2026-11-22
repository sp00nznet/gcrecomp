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

int main() {
    test_c14x2();
    test_c14x2_out_of_range();
    test_c8_still_works();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("gp_decode_test: all passed\n");
    return 0;
}
