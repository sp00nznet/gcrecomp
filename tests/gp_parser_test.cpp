// SPDX-License-Identifier: MIT
//
// GP command parser and vertex decoder tests: synthetic FIFO byte streams go through gp::feed() and
// gp::wgp_write(), the register state and the draw calls recorded by a fake renderer are checked.
// No window, no GL, no disc. The expected values are derived from the public GameCube register
// documentation (see gp_parser.cpp), not from the decoder's own code.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gcrecomp/runtime.h"
#include "gp_internal.h"

using namespace gcrecomp;
using namespace gcrecomp::gp;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        const long long va_ = (long long)(a), vb_ = (long long)(b);                                        \
        if (va_ != vb_) {                                                                                  \
            std::printf("%s:%d: %s == %s failed: %lld vs %lld\n", __FILE__, __LINE__, #a, #b, va_, vb_);   \
            ++g_failures;                                                                                  \
        }                                                                                                  \
    } while (0)
#define CHECK_NEAR(a, b)                                                                                    \
    do {                                                                                                    \
        const double va_ = (a), vb_ = (b);                                                                  \
        if (std::fabs(va_ - vb_) > 1e-4) {                                                                  \
            std::printf("%s:%d: %s ~= %s failed: %g vs %g\n", __FILE__, __LINE__, #a, #b, va_, vb_);        \
            ++g_failures;                                                                                   \
        }                                                                                                   \
    } while (0)

// ---- Fake renderer ----------------------------------------------------------------------------

struct RecordedDraw {
    Primitive primitive;
    uint32_t count;
    std::vector<float> v;  // count * kVertexFloats
    float vertex(uint32_t i, int field) const { return v[size_t(i) * kVertexFloats + field]; }
};
static std::vector<RecordedDraw> g_draws;
static std::vector<CopyRequest> g_copies;

namespace gcrecomp::gp::renderer {
bool init(int, int, const char*, int) { return true; }
void shutdown() {}
void draw(const DrawCall& call, const State&) {
    g_draws.push_back({call.primitive, call.vertex_count,
                       std::vector<float>(call.vertices, call.vertices + size_t(call.vertex_count) * kVertexFloats)});
}
void copy_efb(const CopyRequest& request, const State&) { g_copies.push_back(request); }
bool pump_events() { return true; }
}  // namespace gcrecomp::gp::renderer

// Vertex float layout handed to the renderer.
enum { kPos = 0, kW = 3, kCol0 = 4, kCol1 = 8, kTex = 12 };

// ---- Stream builders --------------------------------------------------------------------------

using Bytes = std::vector<uint8_t>;

static void put32(Bytes& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
static void put16(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
static void putf(Bytes& b, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put32(b, v);
}
static uint32_t fbits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}
static void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }

static Bytes cp(uint8_t reg, uint32_t value) {
    Bytes b{0x08, reg};
    put32(b, value);
    return b;
}
static Bytes bp(uint8_t reg, uint32_t data) {
    Bytes b{0x61};
    put32(b, (uint32_t(reg) << 24) | (data & 0xFFFFFF));
    return b;
}
static Bytes xf(uint32_t addr, const std::vector<uint32_t>& words) {
    Bytes b{0x10};
    put32(b, ((uint32_t(words.size()) - 1) << 16) | addr);
    for (uint32_t w : words) put32(b, w);
    return b;
}
static Bytes draw_cmd(Primitive prim, uint32_t fmt, uint32_t count) {
    Bytes b{uint8_t(0x80 | (uint32_t(prim) << 3) | fmt)};
    put16(b, count);
    return b;
}

static void feed_all(const Bytes& b) { feed(b.data(), b.size()); }

// VCD (CP 0x50 / 0x60) builders: position, normal, color 0/1 at bits 9.., texcoord i at 2i of the high word.
static uint32_t vcd_lo(bool pmidx, uint32_t pos, uint32_t nrm = 0, uint32_t col0 = 0, uint32_t col1 = 0, uint32_t texmidx = 0) {
    return (pmidx ? 1u : 0u) | (texmidx << 1) | (pos << 9) | (nrm << 11) | (col0 << 13) | (col1 << 15);
}
// VAT group A: position (cnt bit 0, fmt 1-3, shift 4-8), normal (cnt 9, fmt 10-12), color0 (cnt 13, fmt 14-16),
// color1 (cnt 17, fmt 18-20), texcoord 0 (cnt 21, fmt 22-24, shift 25-29).
static uint32_t vat_a(uint32_t pos_xyz, uint32_t pos_fmt, uint32_t pos_shift, uint32_t nrm_nbt = 0, uint32_t nrm_fmt = 0,
                      uint32_t col0_rgba = 0, uint32_t col0_fmt = 0, uint32_t col1_rgba = 0, uint32_t col1_fmt = 0,
                      uint32_t tex0_st = 0, uint32_t tex0_fmt = 0, uint32_t tex0_shift = 0) {
    return pos_xyz | (pos_fmt << 1) | (pos_shift << 4) | (nrm_nbt << 9) | (nrm_fmt << 10) | (col0_rgba << 13) |
           (col0_fmt << 14) | (col1_rgba << 17) | (col1_fmt << 18) | (tex0_st << 21) | (tex0_fmt << 22) | (tex0_shift << 25);
}

// Direct/indexed array memory used by the tests.
constexpr uint32_t kArrayPhys = 0x00200000;

// Resets the parser, enables rendering with the recording renderer and sets up a transform that
// returns object-space positions unchanged (x, y in the output, z as 2 * z - 1 in the depth slot): identity position matrices 0 and 3 (the latter
// translated by (10, 20, 30)), identity normal matrix 0, orthographic identity projection, vertex
// colors passed through (material source = vertex, no lighting), texgen 0..1 reading texcoord 0..1.
static void setup() {
    reset();
    g_graphics = true;
    g_draws.clear();
    g_copies.clear();
    std::memset(g_mem.hw_regs, 0, sizeof(g_mem.hw_regs));
    Bytes s;
    std::vector<uint32_t> ident = {fbits(1), 0, 0, 0, 0, fbits(1), 0, 0, 0, 0, fbits(1), 0};
    append(s, xf(0, ident));
    std::vector<uint32_t> moved = ident;
    moved[3] = fbits(10);
    moved[7] = fbits(20);
    moved[11] = fbits(30);
    append(s, xf(12, moved));  // position matrix index 3 (4 words per row group, 3 rows)
    append(s, xf(0x400, {fbits(1), 0, 0, 0, fbits(1), 0, 0, 0, fbits(1)}));
    append(s, xf(0x1020, {fbits(1), 0, fbits(1), 0, fbits(1), 0, 1}));
    append(s, xf(0x101C, {fbits(16777215.f), 0, 0, 0}));  // z range; far plane 0 at 0x101F: depth = z
    append(s, xf(0x100E, {1, 1, 1, 1}));  // color/alpha channel control: material from the vertex
    append(s, xf(0x1040, {5u << 7, 6u << 7}));
    append(s, bp(0x00, 2));  // GENMODE: two texgens
    feed_all(s);
    g_stats = Stats{};
}

static uint32_t be(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// ---- Tests ------------------------------------------------------------------------------------

static void test_cp_registers() {
    setup();
    Bytes s;
    append(s, cp(0x50, 0x123456));
    append(s, cp(0x60, 0xABCD));
    append(s, cp(0x30, 0x11111111));
    append(s, cp(0x40, 0x22222222));
    append(s, cp(0x70 + 3, 0xA0A0A0A0));
    append(s, cp(0x80 + 7, 0xB0B0B0B0));
    append(s, cp(0x90 + 0, 0xC0C0C0C0));
    append(s, cp(0xA0 + 4, 0xFF123456));  // array base: masked to 26 bits
    append(s, cp(0xB0 + 4, 0x1234));      // array stride: 8 bits
    append(s, cp(0xA0 + 15, 0x00000040));
    append(s, cp(0xB0 + 15, 0x7));
    feed_all(s);
    CHECK_EQ(g_state.vcd_lo, 0x123456);
    CHECK_EQ(g_state.vcd_hi, 0xABCD);
    CHECK_EQ(g_state.matidx_a, 0x11111111);
    CHECK_EQ(g_state.matidx_b, 0x22222222);
    CHECK_EQ(g_state.vat[0][3], 0xA0A0A0A0u);
    CHECK_EQ(g_state.vat[1][7], 0xB0B0B0B0u);
    CHECK_EQ(g_state.vat[2][0], 0xC0C0C0C0u);
    CHECK_EQ(g_state.vat[0][0], 0);
    CHECK_EQ(g_state.array_base[4], 0x03123456);
    CHECK_EQ(g_state.array_stride[4], 0x34);
    CHECK_EQ(g_state.array_base[15], 0x40);
    CHECK_EQ(g_state.array_stride[15], 7);
    CHECK_EQ(g_stats.cp_writes, 11);
    CHECK_EQ(g_stats.commands, 11);
    CHECK_EQ(g_stats.unknown_commands, 0);
}

static void test_xf_load() {
    setup();
    feed_all(xf(0x0600, {1, 2, 3, 4}));
    CHECK_EQ(g_state.xf[0x600], 1);
    CHECK_EQ(g_state.xf[0x603], 4);
    CHECK_EQ(g_state.xf[0x604], 0);
    // Words past the end of XF memory are dropped, the ones before are kept.
    const uint64_t before = g_stats.xf_writes;
    feed_all(xf(0x10FE, {7, 8, 9, 10}));
    CHECK_EQ(g_state.xf[0x10FE], 7);
    CHECK_EQ(g_state.xf[0x10FF], 8);
    CHECK_EQ(g_stats.xf_writes - before, 4);
    // A single word.
    feed_all(xf(0x1005, {0xDEADBEEF}));
    CHECK_EQ(g_state.xf[0x1005], 0xDEADBEEFu);
    CHECK_NEAR(g_state.xf_float(0x1020), 1.0);  // set by setup()
}

static void test_xf_indexed_load() {
    setup();
    uint8_t* arr = g_mem.translate(0x80000000u | kArrayPhys);
    for (int i = 0; i < 64; ++i) arr[i] = uint8_t(0x40 + i);
    // Array 12 (position matrices): base and stride 16 bytes.
    feed_all(cp(0xA0 + 12, kArrayPhys));
    feed_all(cp(0xB0 + 12, 16));
    // Opcode 0x20 = XF A. Index 2, length 4 words (field 3), XF address 0x0020.
    Bytes b{0x20};
    put32(b, (2u << 16) | (3u << 12) | 0x0020);
    feed_all(b);
    CHECK_EQ(g_state.xf[0x20], be(arr + 32));
    CHECK_EQ(g_state.xf[0x23], be(arr + 44));
    CHECK_EQ(g_state.xf[0x24], 0);
    // Opcode 0x38 = XF D (lights): array 15.
    feed_all(cp(0xA0 + 15, kArrayPhys + 4));
    feed_all(cp(0xB0 + 15, 8));
    Bytes d{0x38};
    put32(d, (1u << 16) | (1u << 12) | 0x0600);
    feed_all(d);
    CHECK_EQ(g_state.xf[0x600], be(arr + 12));
    CHECK_EQ(g_state.xf[0x601], be(arr + 16));
    CHECK_EQ(g_stats.unknown_commands, 0);
}

static void test_bp_and_mask() {
    setup();
    feed_all(bp(0x40, 0x123456));
    CHECK_EQ(g_state.bp[0x40], 0x123456);
    // The mask register (0xFE) limits the next write, then resets to all ones.
    feed_all(bp(0xFE, 0x00FF00));
    feed_all(bp(0x40, 0xABCDEF));
    CHECK_EQ(g_state.bp[0x40], 0x12CD56);
    CHECK_EQ(g_state.bp[0xFE], 0xFFFFFF);
    feed_all(bp(0x40, 0xABCDEF));
    CHECK_EQ(g_state.bp[0x40], 0xABCDEF);
    // The mask does not apply to itself.
    feed_all(bp(0xFE, 0x0000FF));
    CHECK_EQ(g_state.bp[0xFE], 0x0000FF);
    feed_all(bp(0x41, 0x111111));
    CHECK_EQ(g_state.bp[0x41], 0x000011);
    CHECK_EQ(g_stats.bp_writes, 6);
}

static void test_tev_colors() {
    setup();
    // BP 0xE0: PREV red/alpha (low half = red, high half = alpha), 0xE1: blue/green.
    auto pack = [](int a, int b) { return uint32_t(a & 0x7FF) | (uint32_t(b & 0x7FF) << 12); };
    feed_all(bp(0xE0, pack(255, 51)));
    feed_all(bp(0xE1, pack(102, 204)));
    CHECK_NEAR(g_state.tev_reg[0][0], 1.0);
    CHECK_NEAR(g_state.tev_reg[0][3], 51 / 255.0);
    CHECK_NEAR(g_state.tev_reg[0][2], 102 / 255.0);
    CHECK_NEAR(g_state.tev_reg[0][1], 204 / 255.0);
    // Bit 23 selects the constant color bank; values are signed 11 bit.
    feed_all(bp(0xE6, pack(-255, 255) | (1u << 23)));  // slot 3, red/alpha
    CHECK_NEAR(g_state.tev_konst[3][0], -1.0);
    CHECK_NEAR(g_state.tev_konst[3][3], 1.0);
    CHECK_NEAR(g_state.tev_reg[3][0], 0.0);
}

static void test_tlut_load() {
    setup();
    uint8_t* src = g_mem.translate(0x80000000u | kArrayPhys);
    for (int i = 0; i < 64; ++i) src[i] = uint8_t(0x90 + i);
    feed_all(bp(0x64, kArrayPhys >> 5));
    // BP 0x65: TMEM offset 2 (512-byte units), 2 lines of 32 bytes.
    feed_all(bp(0x65, 2u | (2u << 10)));
    CHECK_EQ(g_state.tlut[2 * 512], 0x90);
    CHECK_EQ(g_state.tlut[2 * 512 + 63], 0x90 + 63);
    CHECK_EQ(g_state.tlut[2 * 512 + 64], 0);
    CHECK_EQ(g_state.tlut[0], 0);
    // Out-of-range loads are ignored.
    feed_all(bp(0x65, 0x3FFu | (0x7FFu << 10)));
    CHECK_EQ(g_state.tlut[0x3FF * 512], 0);
}

static void test_sync_token_and_copy() {
    setup();
    feed_all(bp(0x48, 0xBEEF));
    CHECK_EQ(g_mem.hw_regs[0x100E], 0xBE);
    CHECK_EQ(g_mem.hw_regs[0x100F], 0xEF);
    feed_all(bp(0x47, 0x1234));
    CHECK_EQ(g_mem.hw_regs[0x100E], 0x12);
    CHECK_EQ(g_mem.hw_regs[0x100F], 0x34);

    // EFB copy: source corner (BP 0x49: x | y << 10), size - 1 (BP 0x4A), then BP 0x52.
    feed_all(bp(0x49, 16 | (32u << 10)));
    feed_all(bp(0x4A, 639 | (479u << 10)));
    feed_all(bp(0x52, 1u << 14 | 1u << 11));  // to XFB, clear
    CHECK_EQ(g_copies.size(), 1);
    CHECK(g_copies.size() == 1 && g_copies[0].to_xfb && g_copies[0].clear);
    CHECK(g_copies.size() == 1 && g_copies[0].x == 16 && g_copies[0].y == 32);
    CHECK(g_copies.size() == 1 && g_copies[0].w == 640 && g_copies[0].h == 480);
    CHECK_EQ(g_frames, 1);
    feed_all(bp(0x52, 0));  // texture copy: no frame
    CHECK_EQ(g_copies.size(), 2);
    CHECK(g_copies.size() == 2 && !g_copies[1].to_xfb && !g_copies[1].clear);
    CHECK_EQ(g_frames, 1);
}

static void test_split_stream() {
    // The same stream fed in one piece and byte by byte must give the same state and draws.
    Bytes s;
    append(s, cp(0x50, vcd_lo(false, 1)));
    append(s, cp(0x70, vat_a(1, 4, 0)));
    append(s, xf(0x0600, {1, 2, 3, 4, 5}));
    append(s, bp(0x40, 0x1234));
    append(s, draw_cmd(kTriangles, 0, 3));
    for (float f : {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f}) putf(s, f);
    append(s, Bytes{0x00, 0x00});
    Bytes dl{0x40};
    put32(dl, 0x80000000);
    put32(dl, 0);
    append(s, dl);

    setup();
    feed_all(s);
    const std::vector<RecordedDraw> whole = g_draws;
    const Stats whole_stats = g_stats;
    const uint32_t whole_bp = g_state.bp[0x40], whole_xf = g_state.xf[0x604];

    setup();
    for (uint8_t byte : s) feed(&byte, 1);
    CHECK_EQ(g_draws.size(), 1);
    CHECK_EQ(whole.size(), 1);
    CHECK(!g_draws.empty() && !whole.empty() && g_draws[0].v == whole[0].v);
    CHECK_EQ(g_stats.commands, whole_stats.commands);
    CHECK_EQ(g_stats.xf_writes, whole_stats.xf_writes);
    CHECK_EQ(g_stats.vertices, whole_stats.vertices);
    CHECK_EQ(g_state.bp[0x40], whole_bp);
    CHECK_EQ(g_state.xf[0x604], whole_xf);
    CHECK_EQ(g_stats.unknown_commands, 0);

    // A truncated command waits for the rest.
    setup();
    Bytes half = bp(0x40, 0x777777);
    feed(half.data(), 3);
    CHECK_EQ(g_state.bp[0x40], 0);
    feed(half.data() + 3, 2);
    CHECK_EQ(g_state.bp[0x40], 0x777777);
}

static void test_unknown_and_nop() {
    setup();
    feed_all(Bytes{0x00, 0x44, 0x48});
    CHECK_EQ(g_stats.commands, 3);
    CHECK_EQ(g_stats.unknown_commands, 0);
    std::fprintf(stderr, "(expected: an unknown command report follows)\n");
    feed_all(Bytes{0x7F});
    CHECK_EQ(g_stats.unknown_commands, 1);
    feed_all(bp(0x40, 5));  // the parser recovers on the next byte
    CHECK_EQ(g_state.bp[0x40], 5);
}

// ---- Vertex decoding --------------------------------------------------------------------------

static void test_draw_call_shape() {
    setup();
    feed_all(cp(0x50, vcd_lo(false, 1)));
    feed_all(cp(0x70, vat_a(1, 4, 0)));
    for (Primitive prim : {kQuads, kTriangles, kTriangleStrip, kTriangleFan, kLines, kLineStrip, kPoints}) {
        Bytes s = draw_cmd(prim, 0, 4);
        for (int i = 0; i < 12; ++i) putf(s, float(i));
        feed_all(s);
    }
    CHECK_EQ(g_draws.size(), 7);
    const Primitive order[] = {kQuads, kTriangles, kTriangleStrip, kTriangleFan, kLines, kLineStrip, kPoints};
    for (size_t i = 0; i < g_draws.size() && i < 7; ++i) {
        CHECK_EQ(g_draws[i].primitive, order[i]);
        CHECK_EQ(g_draws[i].count, 4);
    }
    CHECK_EQ(g_stats.draws, 7);
    CHECK_EQ(g_stats.vertices, 28);
    // The ortho identity projection returns the positions; w is 1.
    if (!g_draws.empty()) {
        CHECK_NEAR(g_draws[0].vertex(0, kPos + 0), 0);
        CHECK_NEAR(g_draws[0].vertex(1, kPos + 0), 3);
        CHECK_NEAR(g_draws[0].vertex(1, kPos + 1), 4);
        CHECK_NEAR(g_draws[0].vertex(3, kPos + 1), 10);
        CHECK_NEAR(g_draws[0].vertex(3, kW), 1);
    }
    // A zero-vertex draw is not counted; a draw with no renderer only updates the counters.
    feed_all(draw_cmd(kTriangles, 0, 0));
    CHECK_EQ(g_stats.draws, 7);
    g_graphics = false;
    Bytes s = draw_cmd(kTriangles, 0, 3);
    for (int i = 0; i < 9; ++i) putf(s, 1.f);
    feed_all(s);
    CHECK_EQ(g_stats.draws, 8);
    CHECK_EQ(g_draws.size(), 7);
}

static void test_position_formats() {
    struct Case {
        uint32_t fmt, shift, xyz;
        Bytes data;
        float x, y, z;
    };
    std::vector<Case> cases;
    {
        Bytes d{200, 3, 4};  // u8, shift 2
        cases.push_back({0, 2, 1, d, 50.f, 0.75f, 1.0f});
    }
    {
        Bytes d{uint8_t(-5), 6, uint8_t(-128)};  // s8, shift 1
        cases.push_back({1, 1, 1, d, -2.5f, 3.f, -64.f});
    }
    {
        Bytes d;  // u16, shift 4
        put16(d, 0xFFFF);
        put16(d, 16);
        put16(d, 8);
        cases.push_back({2, 4, 1, d, 4095.9375f, 1.f, 0.5f});
    }
    {
        Bytes d;  // s16, shift 8
        put16(d, uint16_t(int16_t(-256)));
        put16(d, 640);
        put16(d, uint16_t(int16_t(-32768)));
        cases.push_back({3, 8, 1, d, -1.f, 2.5f, -128.f});
    }
    {
        Bytes d;  // f32 (shift ignored)
        putf(d, 1.5f);
        putf(d, -2.25f);
        putf(d, 100.f);
        cases.push_back({4, 7, 1, d, 1.5f, -2.25f, 100.f});
    }
    {
        Bytes d;  // 2D position: z is 0
        put16(d, 5);
        put16(d, 6);
        cases.push_back({2, 0, 0, d, 5.f, 6.f, 0.f});
    }
    int n = 0;
    for (const Case& c : cases) {
        setup();
        feed_all(cp(0x50, vcd_lo(false, 1)));
        feed_all(cp(0x70 + 2, vat_a(c.xyz, c.fmt, c.shift)));
        Bytes s = draw_cmd(kPoints, 2, 1);
        append(s, c.data);
        feed_all(s);
        CHECK_EQ(g_draws.size(), 1);
        if (g_draws.size() != 1) {
            std::printf("  (position case %d)\n", n);
            continue;
        }
        CHECK_NEAR(g_draws[0].vertex(0, kPos + 0), c.x);
        CHECK_NEAR(g_draws[0].vertex(0, kPos + 1), c.y);
        CHECK_NEAR(g_draws[0].vertex(0, kPos + 2), 2 * c.z - 1);
        ++n;
    }
}

static void test_position_matrix_index() {
    setup();
    feed_all(cp(0x50, vcd_lo(true, 1)));  // per-vertex position matrix index, direct float positions
    feed_all(cp(0x70, vat_a(1, 4, 0)));
    Bytes s = draw_cmd(kTriangles, 0, 3);
    for (uint8_t idx : {uint8_t(0), uint8_t(3), uint8_t(0xC3)}) {  // top two bits are ignored: 0xC3 & 0x3F = 3
        s.push_back(idx);
        putf(s, 1.f);
        putf(s, 2.f);
        putf(s, 3.f);
    }
    feed_all(s);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) {
        CHECK_NEAR(g_draws[0].vertex(0, kPos + 0), 1);
        CHECK_NEAR(g_draws[0].vertex(1, kPos + 0), 11);
        CHECK_NEAR(g_draws[0].vertex(1, kPos + 1), 22);
        CHECK_NEAR(g_draws[0].vertex(2, kPos + 0), 11);
    }
    // Default matrix index from CP 0x30 when the vertex has none.
    setup();
    feed_all(cp(0x30, 3));
    feed_all(cp(0x50, vcd_lo(false, 1)));
    feed_all(cp(0x70, vat_a(1, 4, 0)));
    s = draw_cmd(kPoints, 0, 1);
    for (float f : {1.f, 2.f, 3.f}) putf(s, f);
    feed_all(s);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) CHECK_NEAR(g_draws[0].vertex(0, kPos + 1), 22);
}

static void test_indexed_positions() {
    setup();
    uint8_t* arr = g_mem.translate(0x80000000u | kArrayPhys);
    std::memset(arr, 0, 256);
    // Array 0: 3 x s16 per element (shift 4), 8 byte stride (2 bytes of padding).
    auto elem = [&](int i, int16_t x, int16_t y, int16_t z) {
        uint8_t* p = arr + i * 8;
        p[0] = uint8_t(uint16_t(x) >> 8); p[1] = uint8_t(x);
        p[2] = uint8_t(uint16_t(y) >> 8); p[3] = uint8_t(y);
        p[4] = uint8_t(uint16_t(z) >> 8); p[5] = uint8_t(z);
    };
    elem(0, 16, 32, 48);
    elem(1, -16, 64, 0);
    elem(300, 160, 0, 0);
    feed_all(cp(0xA0, kArrayPhys));
    feed_all(cp(0xB0, 8));
    feed_all(cp(0x70, vat_a(1, 3, 4)));
    // 8-bit indices
    feed_all(cp(0x50, vcd_lo(false, 2)));
    Bytes s = draw_cmd(kTriangles, 0, 3);
    s.push_back(1);
    s.push_back(0);
    s.push_back(1);
    feed_all(s);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) {
        CHECK_NEAR(g_draws[0].vertex(0, kPos + 0), -1);
        CHECK_NEAR(g_draws[0].vertex(0, kPos + 1), 4);
        CHECK_NEAR(g_draws[0].vertex(1, kPos + 0), 1);
        CHECK_NEAR(g_draws[0].vertex(1, kPos + 1), 2);
        CHECK_NEAR(g_draws[0].vertex(2, kPos + 0), -1);
    }
    // 16-bit indices: element 300 is beyond an 8-bit index.
    g_draws.clear();
    feed_all(cp(0x50, vcd_lo(false, 3)));
    Bytes t = draw_cmd(kPoints, 0, 2);
    put16(t, 300);  // does not fit an 8-bit index
    put16(t, 1);
    feed_all(t);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) {
        CHECK_NEAR(g_draws[0].vertex(0, kPos + 0), 10);
        CHECK_NEAR(g_draws[0].vertex(1, kPos + 0), -1);
    }
    CHECK_EQ(g_stats.unknown_commands, 0);
}

static void test_colors() {
    struct Case {
        uint32_t fmt, rgba;
        Bytes data;
        float c[4];
    };
    std::vector<Case> cases;
    auto add = [&](uint32_t fmt, uint32_t rgba, Bytes d, float r, float g, float b, float a) {
        cases.push_back({fmt, rgba, d, {r, g, b, a}});
    };
    { Bytes d; put16(d, (31u << 11) | (0u << 5) | 31u); add(0, 0, d, 1.f, 0.f, 1.f, 1.f); }                 // RGB565
    { Bytes d{10, 20, 30}; add(1, 0, d, 10 / 255.f, 20 / 255.f, 30 / 255.f, 1.f); }                         // RGB888
    { Bytes d{10, 20, 30, 99}; add(2, 0, d, 10 / 255.f, 20 / 255.f, 30 / 255.f, 1.f); }                    // RGBX8888
    { Bytes d; put16(d, 0xF084); add(3, 1, d, 1.f, 0.f, 8 / 15.f, 4 / 15.f); }                             // RGBA4444
    { Bytes d; put32(d, 0); d.resize(3); d[0] = 0xFC; d[1] = 0x00; d[2] = 0x3F; add(4, 1, d, 1.f, 0.f, 0.f, 1.f); }  // RGBA6666
    { Bytes d{1, 2, 3, 4}; add(5, 1, d, 1 / 255.f, 2 / 255.f, 3 / 255.f, 4 / 255.f); }                      // RGBA8888
    int n = 0;
    for (const Case& c : cases) {
        setup();
        feed_all(cp(0x50, vcd_lo(false, 1, 0, 1)));
        feed_all(cp(0x70, vat_a(1, 4, 0, 0, 0, c.rgba, c.fmt)));
        Bytes s = draw_cmd(kPoints, 0, 1);
        for (float f : {0.f, 0.f, 0.f}) putf(s, f);
        append(s, c.data);
        feed_all(s);
        CHECK_EQ(g_draws.size(), 1);
        if (g_draws.size() != 1) continue;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(g_draws[0].vertex(0, kCol0 + i) - c.c[i]) > 1e-4) {
                std::printf("  color case %d channel %d: %g vs %g\n", n, i, g_draws[0].vertex(0, kCol0 + i), c.c[i]);
                ++g_failures;
            }
        }
        ++n;
    }
}

static void test_color_sources_and_indexed() {
    setup();
    // Color 0 indexed (8 bit, array 2, RGBA8888), color 1 direct RGB888; position direct float.
    uint8_t* arr = g_mem.translate(0x80000000u | kArrayPhys);
    const uint8_t pal[8] = {9, 9, 9, 9, 100, 150, 200, 255};
    std::memcpy(arr, pal, 8);
    feed_all(cp(0xA0 + 2, kArrayPhys));
    feed_all(cp(0xB0 + 2, 4));
    feed_all(cp(0x50, vcd_lo(false, 1, 0, 2, 1)));
    feed_all(cp(0x70, vat_a(1, 4, 0, 0, 0, 1, 5, 0, 1)));
    Bytes s = draw_cmd(kPoints, 0, 1);
    for (float f : {1.f, 2.f, 3.f}) putf(s, f);
    s.push_back(1);  // color 0 index
    s.push_back(51); s.push_back(102); s.push_back(153);  // color 1
    feed_all(s);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) {
        CHECK_NEAR(g_draws[0].vertex(0, kCol0 + 0), 100 / 255.0);
        CHECK_NEAR(g_draws[0].vertex(0, kCol0 + 1), 150 / 255.0);
        CHECK_NEAR(g_draws[0].vertex(0, kCol0 + 2), 200 / 255.0);
        CHECK_NEAR(g_draws[0].vertex(0, kCol0 + 3), 1.0);
        CHECK_NEAR(g_draws[0].vertex(0, kCol1 + 0), 51 / 255.0);
        CHECK_NEAR(g_draws[0].vertex(0, kCol1 + 1), 102 / 255.0);
        CHECK_NEAR(g_draws[0].vertex(0, kCol1 + 2), 153 / 255.0);
    }
    // Without vertex colors and with material from the register, the material color comes out.
    setup();
    feed_all(xf(0x100C, {0x336699CC}));
    feed_all(xf(0x100E, {0, 0, 0, 0}));
    feed_all(cp(0x50, vcd_lo(false, 1)));
    feed_all(cp(0x70, vat_a(1, 4, 0)));
    s = draw_cmd(kPoints, 0, 1);
    for (float f : {1.f, 2.f, 3.f}) putf(s, f);
    feed_all(s);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) {
        CHECK_NEAR(g_draws[0].vertex(0, kCol0 + 0), 0x33 / 255.0);
        CHECK_NEAR(g_draws[0].vertex(0, kCol0 + 1), 0x66 / 255.0);
        CHECK_NEAR(g_draws[0].vertex(0, kCol0 + 2), 0x99 / 255.0);
        CHECK_NEAR(g_draws[0].vertex(0, kCol0 + 3), 0xCC / 255.0);
    }
}

static void test_texcoords() {
    setup();
    // Texcoord 0: direct u16 (shift 8); texcoord 1: indexed 8 bit float ST from array 5 (4 + 1).
    uint8_t* arr = g_mem.translate(0x80000000u | kArrayPhys);
    Bytes e;
    putf(e, 0.25f);
    putf(e, 0.75f);
    putf(e, -1.5f);
    putf(e, 2.5f);
    std::memcpy(arr, e.data(), e.size());
    feed_all(cp(0xA0 + 5, kArrayPhys));
    feed_all(cp(0xB0 + 5, 8));
    feed_all(cp(0x50, vcd_lo(false, 1)));
    feed_all(cp(0x60, 1u | (2u << 2)));
    // VAT A: texcoord 0 ST u16 shift 8. VAT B: texcoord 1 ST float (cnt bit 0, fmt bits 1-3, shift 4-8).
    feed_all(cp(0x70, vat_a(1, 4, 0, 0, 0, 0, 0, 0, 0, 1, 2, 8)));
    feed_all(cp(0x80, 1u | (4u << 1)));
    Bytes s = draw_cmd(kTriangles, 0, 2);
    for (int v = 0; v < 2; ++v) {
        for (float f : {0.f, 0.f, 0.f}) putf(s, f);
        put16(s, v ? 512 : 128);  // 0.5 or 2.0
        put16(s, 64);             // 0.25
        s.push_back(uint8_t(v));  // texcoord 1 index
    }
    feed_all(s);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) {
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 0), 0.5);
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 1), 0.25);
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 2), 1.0);
        CHECK_NEAR(g_draws[0].vertex(1, kTex + 0), 2.0);
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 3), 0.25);
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 4), 0.75);
        CHECK_NEAR(g_draws[0].vertex(1, kTex + 3), -1.5);
        CHECK_NEAR(g_draws[0].vertex(1, kTex + 4), 2.5);
        // Texgens beyond GENMODE's count stay at (0, 0, 1).
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 6), 0.0);
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 8), 1.0);
    }
}

static void test_texcoord_slots_and_matrix_index() {
    // Texcoord 4 straddles VAT B bit 31 and VAT C bits 0-3 (shift field), texcoord 7 is in VAT C.
    setup();
    feed_all(bp(0x00, 8));  // eight texgens
    std::vector<uint32_t> rows;
    for (uint32_t i = 0; i < 8; ++i) rows.push_back((5u + i) << 7);
    feed_all(xf(0x1040, rows));
    feed_all(cp(0x50, vcd_lo(false, 1)));
    feed_all(cp(0x60, 1u << 8 | 1u << 14));  // texcoord 4 and 7 direct
    // texcoord 4: cnt (VAT B bit 27) = ST, fmt (28-30) = u8, shift 5 = bit 31 of B | (VAT C bits 0-3 << 1)
    const uint32_t shift4 = 0x15;  // 10101: B bit 31 = 1, C low bits = 0b1010
    feed_all(cp(0x80, (1u << 27) | (0u << 28) | ((shift4 & 1u) << 31)));
    // texcoord 7: cnt bit 22, fmt bits 23-25, shift bits 26-30 of VAT C
    feed_all(cp(0x90, ((shift4 >> 1) & 0xF) | (1u << 22) | (1u << 23) | (2u << 26)));
    feed_all(cp(0x70, vat_a(1, 4, 0)));
    Bytes s = draw_cmd(kPoints, 0, 1);
    for (float f : {0.f, 0.f, 0.f}) putf(s, f);
    s.push_back(96);  // texcoord 4 s: 96 / 2^21
    s.push_back(32);  // t
    s.push_back(uint8_t(int8_t(-64)));  // texcoord 7 s: s8 / 4
    s.push_back(8);
    feed_all(s);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) {
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 4 * 3 + 0), 96.0 / (1 << 21));
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 4 * 3 + 1), 32.0 / (1 << 21));
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 7 * 3 + 0), -16.0);
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 7 * 3 + 1), 2.0);
    }

    // Per-vertex texture matrix index: texcoord 1 uses matrix 3 (translated by (10, 20, 30)).
    setup();
    feed_all(cp(0x50, vcd_lo(false, 1, 0, 0, 0, 1u << 1)));  // texmidx[1]
    feed_all(cp(0x60, 1u << 2));                              // texcoord 1 direct
    feed_all(cp(0x70, vat_a(1, 4, 0)));
    feed_all(cp(0x80, 1u | (4u << 1)));
    s = draw_cmd(kPoints, 0, 1);
    s.push_back(3);
    for (float f : {0.f, 0.f, 0.f}) putf(s, f);
    putf(s, 1.f);
    putf(s, 2.f);
    feed_all(s);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) {
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 3), 11);
        CHECK_NEAR(g_draws[0].vertex(0, kTex + 4), 22);
    }
}

// All eight texcoord slots with different formats and shifts, to pin each VAT field position.
static void test_all_texcoord_slots() {
    setup();
    feed_all(bp(0x00, 8));
    std::vector<uint32_t> rows;
    for (uint32_t i = 0; i < 8; ++i) rows.push_back((5u + i) << 7);
    feed_all(xf(0x1040, rows));
    feed_all(cp(0x50, vcd_lo(false, 1)));
    feed_all(cp(0x60, 0x5555));  // all eight direct
    // {register, cnt bit, fmt bit, shift bit}: VAT A (0x70), B (0x80), C (0x90); slot 4's shift is split.
    struct Pos { int reg, cnt, fmt, shift; };
    const Pos pos[8] = {{0, 21, 22, 25}, {1, 0, 1, 4}, {1, 9, 10, 13}, {1, 18, 19, 22},
                        {1, 27, 28, 31}, {2, 4, 5, 8}, {2, 13, 14, 17}, {2, 22, 23, 26}};
    uint32_t vat[3] = {vat_a(1, 4, 0), 0, 0};
    for (uint32_t t = 0; t < 8; ++t) {
        const uint32_t fmt = t % 4, shift = t + 3;
        const Pos& q = pos[t];
        vat[q.reg] |= (1u << q.cnt) | (fmt << q.fmt);
        if (t == 4) {
            vat[1] |= (shift & 1u) << 31;
            vat[2] |= (shift >> 1) & 0xF;
        } else {
            vat[q.reg] |= shift << q.shift;
        }
    }
    feed_all(cp(0x70, vat[0]));
    feed_all(cp(0x80, vat[1]));
    feed_all(cp(0x90, vat[2]));
    Bytes s = draw_cmd(kPoints, 0, 1);
    for (float f : {0.f, 0.f, 0.f}) putf(s, f);
    for (uint32_t t = 0; t < 8; ++t) {
        for (uint32_t k = 0; k < 2; ++k) {
            const int v = int(10 + 7 * t + 3 * k);
            switch (t % 4) {
                case 0: s.push_back(uint8_t(v)); break;
                case 1: s.push_back(uint8_t(int8_t(-v))); break;
                case 2: put16(s, uint32_t(v) * 5); break;
                default: put16(s, uint16_t(int16_t(-v * 5))); break;
            }
        }
    }
    feed_all(s);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() != 1) return;
    for (uint32_t t = 0; t < 8; ++t) {
        const double scale = double(1u << (t + 3));
        const double sign = (t % 4 == 1 || t % 4 == 3) ? -1.0 : 1.0;
        const double mul = (t % 4 >= 2) ? 5.0 : 1.0;
        const double es = sign * mul * (10 + 7 * t) / scale, et = sign * mul * (13 + 7 * t) / scale;
        if (std::fabs(g_draws[0].vertex(0, kTex + 3 * t) - es) > 1e-6 || std::fabs(g_draws[0].vertex(0, kTex + 3 * t + 1) - et) > 1e-6) {
            std::printf("  texcoord slot %u: got (%g, %g), want (%g, %g)\n", t, g_draws[0].vertex(0, kTex + 3 * t),
                        g_draws[0].vertex(0, kTex + 3 * t + 1), es, et);
            ++g_failures;
        }
    }
}

static void test_normals() {
    // Normal lighting: one white light 10 units in front, clamped diffuse, ambient black.
    auto run = [](uint32_t nrm_mode, uint32_t fmt, uint32_t cnt, const Bytes& data,
                  float* out_r) {
        setup();
        feed_all(xf(0x100E, {0x107, 0x107, 0x107, 0x107}));  // vertex material, lighting, light 0, clamp
        std::vector<uint32_t> light(16, 0);
        light[3] = 0xFFFFFFFF;
        light[12] = fbits(10);  // position z
        feed_all(xf(0x600, light));
        feed_all(cp(0x50, vcd_lo(false, 1, nrm_mode, 1)));
        feed_all(cp(0x70, vat_a(1, 4, 0, cnt, fmt, 1, 5)));
        Bytes s = draw_cmd(kPoints, 0, 1);
        for (float f : {0.f, 0.f, 0.f}) putf(s, f);
        append(s, data);
        // color 0 direct RGBA8888 white follows the normal in the stream.
        append(s, Bytes{255, 255, 255, 255});
        feed_all(s);
        if (g_draws.size() != 1) {
            ++g_failures;
            std::printf("normal case drew %zu\n", g_draws.size());
            *out_r = -1;
            return;
        }
        *out_r = g_draws[0].vertex(0, kCol0);
    };
    float r;
    {  // direct f32 normal facing the light, then away
        Bytes d;
        putf(d, 0.f); putf(d, 0.f); putf(d, 1.f);
        run(1, 4, 0, d, &r);
        CHECK_NEAR(r, 1.0);
        Bytes e;
        putf(e, 0.f); putf(e, 0.f); putf(e, -1.f);
        run(1, 4, 0, e, &r);
        CHECK_NEAR(r, 0.0);
    }
    {  // s8 normal: 0x40 = 1.0 (6 fractional bits); (0, 0.6, 0.8) -> diffuse 0.8
        run(1, 1, 0, Bytes{0, 38, 51}, &r);
        CHECK_NEAR(r, 51.0 / std::sqrt(38.0 * 38.0 + 51.0 * 51.0));
    }
    {  // s16 normal: 14 fractional bits
        Bytes d;
        put16(d, 0); put16(d, 0); put16(d, 0x4000);
        run(1, 3, 0, d, &r);
        CHECK_NEAR(r, 1.0);
    }
    {  // NBT: nine floats are in the stream, only the first three are used
        Bytes d;
        for (float f : {0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f}) putf(d, f);
        run(1, 4, 1, d, &r);
        CHECK_NEAR(r, 1.0);
    }
    {  // indexed normal
        uint8_t* arr = g_mem.translate(0x80000000u | kArrayPhys);
        Bytes n;
        putf(n, 0.f); putf(n, 0.f); putf(n, 1.f);
        std::memcpy(arr + 12, n.data(), 12);
        setup();
        feed_all(xf(0x100E, {0x107, 0x107, 0x107, 0x107}));
        std::vector<uint32_t> light(16, 0);
        light[3] = 0xFFFFFFFF;
        light[12] = fbits(10);
        feed_all(xf(0x600, light));
        feed_all(cp(0xA0 + 1, kArrayPhys));
        feed_all(cp(0xB0 + 1, 12));
        feed_all(cp(0x50, vcd_lo(false, 1, 2, 1)));
        feed_all(cp(0x70, vat_a(1, 4, 0, 0, 4, 1, 5)));
        Bytes s = draw_cmd(kPoints, 0, 1);
        for (float f : {0.f, 0.f, 0.f}) putf(s, f);
        s.push_back(1);
        append(s, Bytes{255, 255, 255, 255});
        feed_all(s);
        CHECK_EQ(g_draws.size(), 1);
        if (g_draws.size() == 1) CHECK_NEAR(g_draws[0].vertex(0, kCol0), 1.0);
    }
}

// ---- Display lists ----------------------------------------------------------------------------

static Bytes call_cmd(uint32_t addr, uint32_t size) {
    Bytes b{0x40};
    put32(b, addr);
    put32(b, size);
    return b;
}

static void test_display_list_call() {
    setup();
    Bytes dl;
    append(dl, cp(0x50, vcd_lo(false, 1)));
    append(dl, cp(0x70, vat_a(1, 4, 0)));
    append(dl, bp(0x40, 0x424242));
    append(dl, draw_cmd(kTriangles, 0, 3));
    for (int i = 0; i < 9; ++i) putf(dl, float(i));
    uint8_t* mem = g_mem.translate(0x80300000);
    std::memcpy(mem, dl.data(), dl.size());
    feed_all(call_cmd(0x80300000, uint32_t(dl.size())));
    CHECK_EQ(g_state.bp[0x40], 0x424242);
    CHECK_EQ(g_draws.size(), 1);
    if (g_draws.size() == 1) CHECK_NEAR(g_draws[0].vertex(2, kPos + 1), 7);
    CHECK_EQ(g_stats.commands, 5);  // the call plus four commands inside
    CHECK_EQ(g_stats.unknown_commands, 0);

    // Nested lists: A calls B which calls A... recursion stops at a fixed depth instead of hanging.
    setup();
    Bytes self = call_cmd(0x80310000, 9);
    std::memcpy(g_mem.translate(0x80310000), self.data(), self.size());
    feed_all(call_cmd(0x80310000, 9));
    CHECK_EQ(g_stats.commands, 5);  // depth limit of 4 nested calls plus the outer one

    // A list that ends mid-command stops there; the following commands are not read past `size`.
    setup();
    Bytes cut = bp(0x40, 0x111111);
    append(cut, bp(0x41, 0x222222));
    std::memcpy(g_mem.translate(0x80320000), cut.data(), cut.size());
    feed_all(call_cmd(0x80320000, 7));
    CHECK_EQ(g_state.bp[0x40], 0x111111);
    CHECK_EQ(g_state.bp[0x41], 0);
}

static void set_fifo_regs(uint32_t pi_base, uint32_t pi_end, uint32_t pi_wr, uint32_t gp_base) {
    auto w32 = [](uint32_t off, uint32_t v) {
        g_mem.hw_regs[off] = uint8_t(v >> 24);
        g_mem.hw_regs[off + 1] = uint8_t(v >> 16);
        g_mem.hw_regs[off + 2] = uint8_t(v >> 8);
        g_mem.hw_regs[off + 3] = uint8_t(v);
    };
    w32(0x300C, pi_base);
    w32(0x3010, pi_end);
    w32(0x3014, pi_wr);
    g_mem.hw_regs[0x20] = uint8_t(gp_base >> 8);
    g_mem.hw_regs[0x21] = uint8_t(gp_base);
    g_mem.hw_regs[0x22] = uint8_t(gp_base >> 24);
    g_mem.hw_regs[0x23] = uint8_t(gp_base >> 16);
}

static void wgp_bytes(const Bytes& b) {
    for (uint8_t x : b) wgp_write(1, x);
}

static void test_wgp_write_paths() {
    // Same FIFO for CPU and GP (or none configured): executed.
    setup();
    wgp_bytes(bp(0x40, 0x123123));
    CHECK_EQ(g_state.bp[0x40], 0x123123);
    set_fifo_regs(0x00100000, 0x001000FF, 0x00100000, 0x00100000);
    wgp_bytes(bp(0x41, 0x321321));
    CHECK_EQ(g_state.bp[0x41], 0x321321);

    // Different FIFOs: the data is recorded in the CPU FIFO in memory and not executed.
    setup();
    set_fifo_regs(0x00100000, 0x001000FF, 0x00100000, 0x00200000);
    const Bytes cmd = bp(0x40, 0x5A5A5A);
    wgp_bytes(cmd);
    CHECK_EQ(g_state.bp[0x40], 0);
    CHECK_EQ(g_stats.commands, 0);
    const uint8_t* rec = g_mem.translate(0x80100000);
    CHECK(std::memcmp(rec, cmd.data(), cmd.size()) == 0);
    CHECK_EQ(be(g_mem.hw_regs + 0x3014), 0x00100000 + cmd.size());
    // Calling the recorded list runs it.
    feed_all(call_cmd(0x80100000, uint32_t(cmd.size())));
    CHECK_EQ(g_state.bp[0x40], 0x5A5A5A);

    // The CPU FIFO write pointer wraps at the end address, toggling the wrap bit.
    setup();
    set_fifo_regs(0x00100000, 0x00100007, 0x00100000, 0x00200000);
    wgp_write(4, 0x11223344);
    wgp_write(4, 0x55667788);
    CHECK_EQ(be(g_mem.hw_regs + 0x3014), 0x04100000);
    wgp_write(1, 0x99);
    CHECK_EQ(be(g_mem.hw_regs + 0x3014), 0x04100001);
    CHECK_EQ(g_mem.translate(0x80100007)[0], 0x88);
    CHECK_EQ(g_mem.translate(0x80100000)[0], 0x99);
}

static void test_wgp_store_sizes() {
    // 8-byte store: BP load + the first three bytes of the next one.
    setup();
    Bytes a = bp(0x40, 0x010203);
    uint64_t v = 0;
    for (int i = 0; i < 5; ++i) v = (v << 8) | a[i];
    v = (v << 24) | (uint64_t(a[0]) << 16) | (uint64_t(0x41) << 8) | 0x11;
    wgp_write(8, v);
    CHECK_EQ(g_state.bp[0x40], 0x010203);
    wgp_write(2, 0x2233);  // completes 61 41 11 22 33
    CHECK_EQ(g_state.bp[0x41], 0x112233);
}

int main() {
    CHECK(g_mem.init());
    test_cp_registers();
    test_xf_load();
    test_xf_indexed_load();
    test_bp_and_mask();
    test_tev_colors();
    test_tlut_load();
    test_sync_token_and_copy();
    test_split_stream();
    test_unknown_and_nop();
    test_draw_call_shape();
    test_position_formats();
    test_position_matrix_index();
    test_indexed_positions();
    test_colors();
    test_color_sources_and_indexed();
    test_texcoords();
    test_texcoord_slots_and_matrix_index();
    test_all_texcoord_slots();
    test_normals();
    test_display_list_call();
    test_wgp_write_paths();
    test_wgp_store_sizes();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gp_parser_test: all passed\n");
    return 0;
}
