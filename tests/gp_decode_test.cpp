// SPDX-License-Identifier: MIT
// Unit tests for the pure GP helpers (src/gp/gp_decode.*): no display, no disc.
#include <cmath>
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

// Encodes fog the way the SDK does (GX_SetFog, perspective case), so that the decoder is checked against
// the writer of the registers rather than against itself.
static void encode_fog_perspective(uint32_t* bp, uint32_t type, float startz, float endz, float nearz, float farz,
                                   uint32_t rgb) {
    const float A = (farz * nearz) / ((farz - nearz) * (endz - startz));
    const float B = farz / (farz - nearz);
    const float C = startz / (endz - startz);
    float b_mant = B;
    uint32_t b_expn = 1;
    while (b_mant > 1.0f) { b_mant /= 2.0f; ++b_expn; }
    while (b_mant > 0.0f && b_mant < 0.5f) { b_mant *= 2.0f; --b_expn; }
    const float a_f = A / float(1u << b_expn);
    const uint32_t b_m = uint32_t(b_mant * 8388638.0f);
    uint32_t a_bits, c_bits;
    std::memcpy(&a_bits, &a_f, 4);
    std::memcpy(&c_bits, &C, 4);
    bp[0xEE] = a_bits >> 12;
    bp[0xEF] = b_m & 0xFFFFFF;
    bp[0xF0] = b_expn & 0x1F;
    bp[0xF1] = (type << 21) | (c_bits >> 12);
    bp[0xF2] = rgb;
}

static void test_fog_linear_perspective() {
    const float n = 1.0f, f = 1000.0f, start = 10.0f, end = 100.0f;
    uint32_t bp[256] = {};
    encode_fog_perspective(bp, kFogLinear, start, end, n, f, 0x336699);
    const FogParams p = decode_fog(bp);
    CHECK(p.type == kFogLinear && !p.ortho);
    CHECK(std::fabs(p.color[0] - 0x33 / 255.0f) < 1e-6f && std::fabs(p.color[1] - 0x66 / 255.0f) < 1e-6f &&
          std::fabs(p.color[2] - 0x99 / 255.0f) < 1e-6f);
    // Depth of an eye-space distance d under the GX perspective projection, as a 24-bit integer.
    for (float d : {2.0f, 10.0f, 30.0f, 55.0f, 99.0f, 100.0f, 400.0f}) {
        const float zs = f / (f - n) * (1.0f - n / d);
        const uint32_t z24 = uint32_t(zs * 16777215.0f + 0.5f);
        float expect = (d - start) / (end - start);
        expect = expect < 0 ? 0 : expect > 1 ? 1 : expect;
        CHECK(std::fabs(fog_factor(p, z24) - expect) < 3e-3f);
    }
}

static void test_fog_types() {
    uint32_t bp[256] = {};
    encode_fog_perspective(bp, kFogOff, 10, 100, 1, 1000, 0);
    CHECK(fog_factor(decode_fog(bp), 0xFFFFFF) == 0.0f);  // fog disabled
    // Mid-range point of the linear ramp (x = 0.5) through the four non-linear curves.
    const float d = 55.0f, zs = 1000.0f / 999.0f * (1.0f - 1.0f / d);
    const uint32_t z24 = uint32_t(zs * 16777215.0f + 0.5f);
    const float x = 0.5f;
    const struct { uint32_t type; float expect; } cases[] = {
        {kFogExp, 1.0f - std::exp2(-8.0f * x)},
        {kFogExp2, 1.0f - std::exp2(-8.0f * x * x)},
        {kFogBackwardsExp, std::exp2(-8.0f * (1.0f - x))},
        {kFogBackwardsExp2, std::exp2(-8.0f * (1.0f - x) * (1.0f - x))},
    };
    for (const auto& c : cases) {
        encode_fog_perspective(bp, c.type, 10, 100, 1, 1000, 0);
        CHECK(std::fabs(fog_factor(decode_fog(bp), z24) - c.expect) < 5e-3f);
    }
}

static void test_fog_orthographic() {
    // GX_SetFog, orthographic: A = (far - near) / (end - start), C = (start - near) / (end - start).
    const float n = 0.0f, f = 100.0f, start = 20.0f, end = 60.0f;
    const float A = (f - n) / (end - start), C = (start - n) / (end - start);
    uint32_t a_bits, c_bits;
    std::memcpy(&a_bits, &A, 4);
    std::memcpy(&c_bits, &C, 4);
    uint32_t bp[256] = {};
    bp[0xEE] = a_bits >> 12;
    bp[0xF1] = (kFogLinear << 21) | (1u << 20) | (c_bits >> 12);
    const FogParams p = decode_fog(bp);
    CHECK(p.ortho);
    for (float d : {0.0f, 20.0f, 40.0f, 60.0f, 80.0f}) {  // depth is linear in eye distance
        const uint32_t z24 = uint32_t((d - n) / (f - n) * 16777215.0f + 0.5f);
        float expect = (d - start) / (end - start);
        expect = expect < 0 ? 0 : expect > 1 ? 1 : expect;
        CHECK(std::fabs(fog_factor(p, z24) - expect) < 1e-3f);
    }
}

static void test_indirect_detection() {
    uint32_t bp[256] = {};
    bp[0x00] = (3u << 10);  // four TEV stages, no indirect stages
    bp[0x11] = 0x1234;      // stale IND_CMD data is ignored without indirect stages
    CHECK(indirect_tev_stages(bp) == 0);
    bp[0x00] |= 1u << 16;   // one indirect stage declared
    CHECK(indirect_tev_stages(bp) == 0x2);
    bp[0x13] = 3u << 9;     // matrix 3 on stage 3
    bp[0x14] = 1;           // stage 4 is beyond the stages in use
    CHECK(indirect_tev_stages(bp) == 0xA);
    // A directly sampled stage (GXSetTevDirect) writes all zero fields.
    bp[0x11] = 0;
    bp[0x13] = 0;
    CHECK(indirect_tev_stages(bp) == 0);
}

static uint32_t alpha_cmp(uint32_t c0, uint32_t ref0, uint32_t c1, uint32_t ref1, uint32_t logic) {
    return ref0 | (ref1 << 8) | (c0 << 16) | (c1 << 19) | (logic << 22);
}

static void test_alpha_test_can_reject() {
    enum { NEVER, LESS, EQUAL, LEQUAL, GREATER, NEQUAL, GEQUAL, ALWAYS };
    enum { AND, OR, XOR, XNOR };
    CHECK(!alpha_test_can_reject(alpha_cmp(ALWAYS, 0, ALWAYS, 0, AND)));      // GXSetAlphaCompare default
    CHECK(alpha_test_can_reject(alpha_cmp(GREATER, 128, ALWAYS, 0, AND)));    // typical cutout
    CHECK(!alpha_test_can_reject(alpha_cmp(GEQUAL, 0, ALWAYS, 0, AND)));      // everything passes
    CHECK(!alpha_test_can_reject(alpha_cmp(LEQUAL, 255, ALWAYS, 0, AND)));
    CHECK(alpha_test_can_reject(alpha_cmp(NEVER, 0, ALWAYS, 0, AND)));
    CHECK(!alpha_test_can_reject(alpha_cmp(GREATER, 128, ALWAYS, 0, OR)));    // the OR makes it pass
    CHECK(alpha_test_can_reject(alpha_cmp(GREATER, 128, GREATER, 64, OR)));
    CHECK(alpha_test_can_reject(alpha_cmp(ALWAYS, 0, ALWAYS, 0, XOR)));       // always XOR always = never passes
    CHECK(!alpha_test_can_reject(alpha_cmp(ALWAYS, 0, ALWAYS, 0, XNOR)));
    CHECK(!alpha_test_can_reject(alpha_cmp(NEVER, 0, NEVER, 0, XNOR)));
    CHECK(!alpha_test_can_reject(alpha_cmp(ALWAYS, 0, NEVER, 0, XOR)));
}

static void test_early_z() {
    uint32_t bp[256] = {};
    bp[0xF3] = alpha_cmp(4, 128, 7, 0, 0);  // alpha > 128
    bp[0x40] = 0x1 | (3 << 1) | 0x10;       // depth test LEQUAL, update on
    CHECK(!early_z_writes_rejected_pixels(bp));      // late Z: discard semantics are already right
    bp[0x43] = 0x40;
    CHECK(early_z_writes_rejected_pixels(bp));
    bp[0x40] &= ~0x10u;                                // no depth write: nothing to emulate
    CHECK(!early_z_writes_rejected_pixels(bp));
    bp[0x40] = 0x10;                                   // depth test off
    CHECK(!early_z_writes_rejected_pixels(bp));
    bp[0x40] = 0x1 | 0x10;
    bp[0xF3] = alpha_cmp(7, 0, 7, 0, 0);               // no alpha test
    CHECK(!early_z_writes_rejected_pixels(bp));
}

int main() {
    test_c14x2();
    test_c14x2_out_of_range();
    test_c8_still_works();
    test_mip_levels();
    test_mip_layout();
    test_lod_fields();
    test_fog_linear_perspective();
    test_fog_types();
    test_fog_orthographic();
    test_indirect_detection();
    test_alpha_test_can_reject();
    test_early_z();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("gp_decode_test: all passed\n");
    return 0;
}
