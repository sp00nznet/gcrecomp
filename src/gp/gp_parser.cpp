// SPDX-License-Identifier: MIT
//
// GP command stream parser and vertex decoder: parses what the SDK's GX functions write to the write-gather pipe, keeps the
// CP/XF/BP register state, decodes vertices and hands primitives to the renderer.
//
// Register layouts follow the public GameCube documentation (YAGCD) and Dolphin's
// documentation of the same hardware. No Dolphin code is used.
#include "gcrecomp/gp/gp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gcrecomp/runtime.h"
#include "gp_internal.h"
#include "lighting.h"

namespace gcrecomp::gp {

float State::xf_float(uint32_t addr) const {
    float f;
    std::memcpy(&f, &xf[addr], 4);
    return f;
}

Stats g_stats;
State g_state;
uint64_t g_frames = 0;
bool g_graphics = false;

namespace {

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

enum AttrMode : uint32_t { kNone = 0, kDirect = 1, kIndex8 = 2, kIndex16 = 3 };

// Where each vertex attribute comes from, and its size inside the vertex, for one VCD/VAT pair.
struct Layout {
    bool pmidx = false;
    bool texmidx[8] = {};
    AttrMode pos = kNone, nrm = kNone, col[2] = {kNone, kNone}, tex[8] = {};
    uint32_t pos_cnt = 3, pos_fmt = 4, pos_shift = 0;
    uint32_t nrm_cnt = 3, nrm_fmt = 4;
    uint32_t col_cnt[2] = {}, col_fmt[2] = {};
    uint32_t tex_cnt[8] = {}, tex_fmt[8] = {}, tex_shift[8] = {};
    uint32_t size = 0;
    uint32_t fmt = 0;
};

uint32_t comp_size(uint32_t fmt) {
    switch (fmt) {
        case 0: case 1: return 1;
        case 2: case 3: return 2;
        default: return 4;
    }
}

uint32_t color_size(uint32_t fmt) {
    switch (fmt) {
        case 0: return 2;  // RGB565
        case 1: return 3;  // RGB888
        case 2: return 4;  // RGB888x
        case 3: return 2;  // RGBA4444
        case 4: return 3;  // RGBA6666
        default: return 4; // RGBA8888
    }
}

uint32_t attr_size(AttrMode m, uint32_t direct_size) {
    switch (m) {
        case kDirect: return direct_size;
        case kIndex8: return 1;
        case kIndex16: return 2;
        default: return 0;
    }
}

Layout make_layout(const State& s, uint32_t fmt) {
    Layout l;
    l.fmt = fmt;
    const uint32_t lo = s.vcd_lo, hi = s.vcd_hi;
    const uint32_t a = s.vat[0][fmt], b = s.vat[1][fmt], c = s.vat[2][fmt];
    l.pmidx = lo & 1;
    for (int i = 0; i < 8; ++i) l.texmidx[i] = (lo >> (1 + i)) & 1;
    l.pos = AttrMode((lo >> 9) & 3);
    l.nrm = AttrMode((lo >> 11) & 3);
    l.col[0] = AttrMode((lo >> 13) & 3);
    l.col[1] = AttrMode((lo >> 15) & 3);
    for (int i = 0; i < 8; ++i) l.tex[i] = AttrMode((hi >> (2 * i)) & 3);

    l.pos_cnt = (a & 1) ? 3 : 2;
    l.pos_fmt = (a >> 1) & 7;
    l.pos_shift = (a >> 4) & 0x1F;
    l.nrm_cnt = ((a >> 9) & 1) ? 9 : 3;
    l.nrm_fmt = (a >> 10) & 7;
    l.col_cnt[0] = (a >> 13) & 1;
    l.col_fmt[0] = (a >> 14) & 7;
    l.col_cnt[1] = (a >> 17) & 1;
    l.col_fmt[1] = (a >> 18) & 7;
    l.tex_cnt[0] = (a >> 21) & 1;
    l.tex_fmt[0] = (a >> 22) & 7;
    l.tex_shift[0] = (a >> 25) & 0x1F;
    l.tex_cnt[1] = b & 1;
    l.tex_fmt[1] = (b >> 1) & 7;
    l.tex_shift[1] = (b >> 4) & 0x1F;
    l.tex_cnt[2] = (b >> 9) & 1;
    l.tex_fmt[2] = (b >> 10) & 7;
    l.tex_shift[2] = (b >> 13) & 0x1F;
    l.tex_cnt[3] = (b >> 18) & 1;
    l.tex_fmt[3] = (b >> 19) & 7;
    l.tex_shift[3] = (b >> 22) & 0x1F;
    l.tex_cnt[4] = (b >> 27) & 1;
    l.tex_fmt[4] = (b >> 28) & 7;
    l.tex_shift[4] = ((b >> 31) & 1) | ((c & 0xF) << 1);
    l.tex_cnt[5] = (c >> 4) & 1;
    l.tex_fmt[5] = (c >> 5) & 7;
    l.tex_shift[5] = (c >> 8) & 0x1F;
    l.tex_cnt[6] = (c >> 13) & 1;
    l.tex_fmt[6] = (c >> 14) & 7;
    l.tex_shift[6] = (c >> 17) & 0x1F;
    l.tex_cnt[7] = (c >> 22) & 1;
    l.tex_fmt[7] = (c >> 23) & 7;
    l.tex_shift[7] = (c >> 26) & 0x1F;

    uint32_t size = l.pmidx ? 1 : 0;
    for (int i = 0; i < 8; ++i) size += l.texmidx[i] ? 1 : 0;
    size += attr_size(l.pos, l.pos_cnt * comp_size(l.pos_fmt));
    size += attr_size(l.nrm, l.nrm_cnt * comp_size(l.nrm_fmt));
    for (int i = 0; i < 2; ++i) size += attr_size(l.col[i], color_size(l.col_fmt[i]));
    for (int i = 0; i < 8; ++i) size += attr_size(l.tex[i], (l.tex_cnt[i] + 1) * comp_size(l.tex_fmt[i]));
    l.size = size;
    return l;
}

float read_component(const uint8_t* p, uint32_t fmt, uint32_t shift) {
    float v;
    switch (fmt) {
        case 0: v = float(p[0]); break;
        case 1: v = float(int8_t(p[0])); break;
        case 2: v = float(be16(p)); break;
        case 3: v = float(int16_t(be16(p))); break;
        default: {
            uint32_t bits = be32(p);
            std::memcpy(&v, &bits, 4);
            return v;
        }
    }
    return shift ? v / float(1u << shift) : v;
}

void read_color(const uint8_t* p, uint32_t fmt, float out[4]) {
    uint32_t r = 255, g = 255, b = 255, a = 255;
    switch (fmt) {
        case 0: {  // RGB565
            const uint32_t v = be16(p);
            r = ((v >> 11) & 31) * 255 / 31;
            g = ((v >> 5) & 63) * 255 / 63;
            b = (v & 31) * 255 / 31;
            break;
        }
        case 1: case 2: r = p[0]; g = p[1]; b = p[2]; break;
        case 3: {  // RGBA4444
            const uint32_t v = be16(p);
            r = ((v >> 12) & 15) * 17;
            g = ((v >> 8) & 15) * 17;
            b = ((v >> 4) & 15) * 17;
            a = (v & 15) * 17;
            break;
        }
        case 4: {  // RGBA6666
            const uint32_t v = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
            r = ((v >> 18) & 63) * 255 / 63;
            g = ((v >> 12) & 63) * 255 / 63;
            b = ((v >> 6) & 63) * 255 / 63;
            a = (v & 63) * 255 / 63;
            break;
        }
        default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
    }
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

void unpack_rgba8(uint32_t packed, float out[4]) {
    out[0] = ((packed >> 24) & 255) / 255.0f;
    out[1] = ((packed >> 16) & 255) / 255.0f;
    out[2] = ((packed >> 8) & 255) / 255.0f;
    out[3] = (packed & 255) / 255.0f;
}

class Parser {
public:
    void feed(const uint8_t* data, size_t n) {
        buf_.insert(buf_.end(), data, data + n);
        for (size_t i = 0; i < n; ++i) raw_[raw_pos_++ & 255] = data[i];
        size_t pos = 0;
        while (pos < buf_.size()) {
            const size_t used = parse_one(buf_.data() + pos, buf_.size() - pos);
            if (used) remember(buf_[pos], used);
            if (used == 0) break;
            pos += used;
        }
        if (pos) buf_.erase(buf_.begin(), buf_.begin() + pos);
    }

private:
    std::vector<uint8_t> buf_;
    std::vector<float> vertices_;
    int dl_depth_ = 0;

    // Returns the number of bytes consumed, or 0 when the command is not complete yet.
    size_t parse_one(const uint8_t* p, size_t n) {
        const uint8_t cmd = p[0];
        switch (cmd) {
            case 0x00: ++g_stats.commands; return 1;
            case 0x44: case 0x48: ++g_stats.commands; return 1;  // metric, invalidate vertex cache
            case 0x08:
                if (n < 6) return 0;
                ++g_stats.commands;
                cp_write(p[1], be32(p + 2));
                return 6;
            case 0x10: {
                if (n < 5) return 0;
                const uint32_t header = be32(p + 1);
                const uint32_t count = (header >> 16) + 1, addr = header & 0xFFFF;
                if (count > 1000 && !huge_reported_) {
                    huge_reported_ = true;
                    dump_raw("huge XF load header");
                }
                if (n < 5 + 4 * size_t(count)) return 0;
                ++g_stats.commands;
                if (count > 64 && std::getenv("GP_TRACE")) {
                    std::fprintf(stderr, "[GP] XF load addr=0x%04X count=%u; bytes:", addr, count);
                    for (int i = 0; i < 24; ++i) std::fprintf(stderr, " %02X", p[i]);
                    std::fprintf(stderr, "\n");
                }
                for (uint32_t i = 0; i < count; ++i) {
                    if (addr + i < 0x1100) g_state.xf[addr + i] = be32(p + 5 + 4 * i);
                }
                g_stats.xf_writes += count;
                if ((addr == 0x101A || addr == 0x1020) && trace_xf_ < 10 && std::getenv("GP_TRACE")) {
                    ++trace_xf_;
                    std::fprintf(stderr, "[GP] XF 0x%04X count=%u lr=0x%08X:", addr, count, g_ctx.lr);
                    for (uint32_t i = 0; i < count && i < 8; ++i) {
                        const uint32_t w = be32(p + 5 + 4 * i);
                        float f;
                        std::memcpy(&f, &w, 4);
                        std::fprintf(stderr, " %08X(%g)", w, f);
                    }
                    std::fprintf(stderr, "\n");
                }
                return 5 + 4 * size_t(count);
            }
            case 0x20: case 0x28: case 0x30: case 0x38: {
                if (n < 5) return 0;
                ++g_stats.commands;
                xf_indexed_load((cmd >> 3) & 3, be32(p + 1));
                return 5;
            }
            case 0x40: {
                if (n < 9) return 0;
                ++g_stats.commands;
                call_display_list(be32(p + 1), be32(p + 5));
                return 9;
            }
            case 0x61:
                if (n < 5) return 0;
                ++g_stats.commands;
                bp_write(be32(p + 1));
                return 5;
            default: break;
        }
        if (cmd >= 0x80) {
            if (n < 3) return 0;
            const uint32_t count = be16(p + 1);
            const Layout layout = make_layout(g_state, cmd & 7);
            const size_t total = 3 + size_t(count) * layout.size;
            if (n < total) return 0;
            ++g_stats.commands;
            if (trace_draws_ < 12 && std::getenv("GP_TRACE")) {
                ++trace_draws_;
                std::fprintf(stderr, "[GP] draw cmd=0x%02X prim=%u count=%u fmt=%u vsize=%u vcd=%06X/%04X vat=%08X/%08X/%08X\n", cmd,
                             (cmd >> 3) & 7, count, cmd & 7, layout.size, g_state.vcd_lo, g_state.vcd_hi, g_state.vat[0][cmd & 7],
                             g_state.vat[1][cmd & 7], g_state.vat[2][cmd & 7]);
            }
            draw(Primitive((cmd >> 3) & 7), count, layout, p + 3);
            return total;
        }
        ++g_stats.unknown_commands;
        if (g_stats.unknown_commands == 1) {
            dump_raw("first unknown command");
            std::fprintf(stderr, "[GP] unknown command 0x%02X; previous commands (opcode:bytes), oldest first:", cmd);
            for (int i = 0; i < 24; ++i) {
                const auto& h = history_[(history_pos_ + i) % 24];
                std::fprintf(stderr, " %02X:%zu", h.first, h.second);
            }
            std::fprintf(stderr, "\n");
        }
        if (g_stats.unknown_commands <= 8) std::fprintf(stderr, "[GP] unknown command 0x%02X\n", cmd);
        return 1;
    }

    bool huge_reported_ = false;
    int trace_xf_ = 0;
    uint8_t raw_[256] = {};
    size_t raw_pos_ = 0;
    void dump_raw(const char* why) {
        std::fprintf(stderr, "[GP] %s; last 160 stream bytes:", why);
        for (size_t i = raw_pos_ - 160; i < raw_pos_; ++i) {
            if ((i - (raw_pos_ - 160)) % 32 == 0) std::fprintf(stderr, "\n   ");
            std::fprintf(stderr, " %02X", raw_[i & 255]);
        }
        std::fprintf(stderr, "\n");
    }
    std::pair<uint8_t, size_t> history_[24] = {};
    int history_pos_ = 0;
    int trace_draws_ = 0;
    void remember(uint8_t cmd, size_t size) {
        history_[history_pos_] = {cmd, size};
        history_pos_ = (history_pos_ + 1) % 24;
    }

    void call_display_list(uint32_t addr, uint32_t size) {
        if (dl_depth_ >= 4) return;
        const uint8_t* mem = g_mem.translate(addr);
        ++dl_depth_;
        size_t pos = 0;
        while (pos < size) {
            const size_t used = parse_one(mem + pos, size - pos);
            if (used == 0) break;
            pos += used;
        }
        --dl_depth_;
    }

    void xf_indexed_load(uint32_t which, uint32_t header) {
        // which: 0 = A (position matrices), 1 = B (normal matrices), 2 = C (post matrices), 3 = D (lights)
        const uint32_t index = header >> 16, len = ((header >> 12) & 0xF) + 1, addr = header & 0xFFF;
        const uint32_t base = g_state.array_base[12 + which], stride = g_state.array_stride[12 + which];
        const uint8_t* src = g_mem.translate(base + index * stride);
        for (uint32_t i = 0; i < len; ++i) {
            if (addr + i < 0x1100) g_state.xf[addr + i] = be32(src + 4 * i);
        }
        g_stats.xf_writes += len;
    }

    void cp_write(uint8_t reg, uint32_t value) {
        ++g_stats.cp_writes;
        State& s = g_state;
        if (reg == 0x30) s.matidx_a = value;
        else if (reg == 0x40) s.matidx_b = value;
        else if (reg == 0x50) s.vcd_lo = value;
        else if (reg == 0x60) s.vcd_hi = value;
        else if (reg >= 0x70 && reg < 0x78) s.vat[0][reg - 0x70] = value;
        else if (reg >= 0x80 && reg < 0x88) s.vat[1][reg - 0x80] = value;
        else if (reg >= 0x90 && reg < 0x98) s.vat[2][reg - 0x90] = value;
        else if (reg >= 0xA0 && reg < 0xB0) s.array_base[reg - 0xA0] = value & 0x03FFFFFF;
        else if (reg >= 0xB0) s.array_stride[reg - 0xB0] = value & 0xFF;
    }

    void bp_write(uint32_t word) {
        ++g_stats.bp_writes;
        const uint32_t reg = word >> 24;
        const uint32_t data = word & 0xFFFFFF;
        State& s = g_state;
        if (reg == 0xFE) {
            s.bp[0xFE] = data;
            return;
        }
        const uint32_t mask = s.bp[0xFE] & 0xFFFFFF;
        s.bp[reg] = (s.bp[reg] & ~mask) | (data & mask);
        s.bp[0xFE] = 0xFFFFFF;
        const uint32_t value = s.bp[reg];

        if (reg >= 0xE0 && reg <= 0xE7) {
            const uint32_t slot = (reg - 0xE0) / 2;
            const bool konst = (value >> 23) & 1;
            auto s10 = [](uint32_t v) {
                v &= 0x7FF;
                return (v & 0x400 ? int(v) - 0x800 : int(v)) / 255.0f;
            };
            float* dst = konst ? s.tev_konst[slot] : s.tev_reg[slot];
            if ((reg & 1) == 0) {
                dst[0] = s10(value);
                dst[3] = s10(value >> 12);
            } else {
                dst[2] = s10(value);
                dst[1] = s10(value >> 12);
            }
        }
        if (reg == 0x65) {
            // LOADTLUT1: copy `count` 32-byte lines from the address given by LOADTLUT0 into TLUT memory.
            const uint32_t src = (s.bp[0x64] & 0x1FFFFF) << 5;
            const uint32_t offset = (value & 0x3FF) * 512;
            const uint32_t bytes = ((value >> 10) & 0x7FF) * 32;
            if (offset + bytes <= sizeof(s.tlut)) std::memcpy(s.tlut + offset, g_mem.translate(src), bytes);
        }
        if (reg == 0x47 || reg == 0x48) {
            // Draw-sync token: GXReadDrawSync reads it from the PE register at 0xCC00100E.
            uint8_t* pe = g_mem.hw_regs + (0xCC00100E - Memory::HW_REG_BASE);
            pe[0] = uint8_t(value >> 8);
            pe[1] = uint8_t(value);
        } else if (reg == 0x52) {
            efb_copy(value);
        }
    }

    void efb_copy(uint32_t value) {
        const State& s = g_state;
        CopyRequest r;
        r.to_xfb = (value >> 14) & 1;
        r.clear = (value >> 11) & 1;
        const uint32_t tl = s.bp[0x49], br = s.bp[0x4A];
        r.x = int(tl & 0x3FF);
        r.y = int((tl >> 10) & 0x3FF);
        r.w = int(br & 0x3FF) + 1;
        r.h = int((br >> 10) & 0x3FF) + 1;
        if (g_graphics) renderer::copy_efb(r, g_state);
        if (r.to_xfb) ++g_frames;
    }

    void dump_state() const {
        const State& s = g_state;
        std::fprintf(stderr, "[STATE] gen=%06X zmode=%02X blend=%04X/%04X acmp=%06X scissor=%06X/%06X cullmode=%u\n", s.bp[0], s.bp[0x40], s.bp[0x41], s.bp[0x42],
                     s.bp[0xF3], s.bp[0x20], s.bp[0x21], (s.bp[0] >> 14) & 3);
        const int nst = int(((s.bp[0] >> 10) & 0xF) + 1);
        for (int i = 0; i < nst; ++i)
            std::fprintf(stderr, "[STATE] stage%d color=%06X alpha=%06X tref=%06X ksel=%06X\n", i, s.bp[0xC0 + 2 * i], s.bp[0xC1 + 2 * i], s.bp[0x28 + i / 2],
                         s.bp[0xF6 + i / 2]);
        for (int i = 0; i < 4; ++i)
            std::fprintf(stderr, "[STATE] reg%d=(%g %g %g %g) konst%d=(%g %g %g %g)\n", i, s.tev_reg[i][0], s.tev_reg[i][1], s.tev_reg[i][2], s.tev_reg[i][3], i,
                         s.tev_konst[i][0], s.tev_konst[i][1], s.tev_konst[i][2], s.tev_konst[i][3]);
        for (int i = 0; i < 8; ++i)
            std::fprintf(stderr, "[STATE] texgen%d info=%05X  map%d mode0=%06X img0=%06X img3=%06X\n", i, s.xf[0x1040 + i], i, s.bp[0x80 + (i < 4 ? i : i + 0x1C)],
                         s.bp[0x88 + (i < 4 ? i : i + 0x1C)], s.bp[0x94 + (i < 4 ? i : i + 0x1C)]);
        std::fprintf(stderr, "[STATE] fog=%06X/%06X/%06X/%06X ind=%06X genmode_ind=%u\n", s.bp[0xEE], s.bp[0xEF], s.bp[0xF0], s.bp[0xF1], s.bp[0x10],
                     (s.bp[0] >> 16) & 7);
    }

    void draw(Primitive prim, uint32_t count, const Layout& l, const uint8_t* data) {
        if (count == 0) return;
        ++g_stats.draws;
        g_stats.vertices += count;
        if (!g_graphics) return;

        const State& s = g_state;
        vertices_.resize(size_t(count) * kVertexFloats);

        // Projection (XF 0x1020..0x1026).
        const float p0 = s.xf_float(0x1020), p1 = s.xf_float(0x1021), p2 = s.xf_float(0x1022),
                    p3 = s.xf_float(0x1023), p4 = s.xf_float(0x1024), p5 = s.xf_float(0x1025);
        const bool ortho = s.xf[0x1026] != 0;
        // Viewport depth mapping (XF 0x101C zRange, 0x101F far).
        const float z_range = s.xf_float(0x101C), far_z = s.xf_float(0x101F);

        float mat_color[2][4];
        unpack_rgba8(s.xf[0x100C], mat_color[0]);
        unpack_rgba8(s.xf[0x100D], mat_color[1]);

        const uint32_t num_texgens = s.bp[0x00] & 0xF;
        static const long trace_frame = std::getenv("GP_TRACE_FRAME") ? std::atol(std::getenv("GP_TRACE_FRAME")) : -1;
        const bool trace = trace_frame >= 0 && uint64_t(trace_frame) == g_frames;
        static int trace_n = 0;
        if (trace) {
            ++trace_n;
            // GP_TRACE_STATE=n[,m...]: full TEV / texture / raster state at those draws of the traced frame.
            if (const char* list = std::getenv("GP_TRACE_STATE")) {
                for (const char* q = list; *q;) {
                    char* end;
                    const long n = std::strtol(q, &end, 10);
                    if (n == trace_n) dump_state();
                    q = *end ? end + 1 : end;
                }
            }
            std::fprintf(stderr,
                         "[DRAW] #%d prim=%u n=%u vsize=%u vcd=%06X/%04X gen=%06X tev=%u z=%02X blend=%04X acmp=%06X cc0=%06X ca0=%06X tref0=%06X img0=%06X/%06X ntexg=%u proj=%g %g %g %g %g %g o=%u vp=%g %g %g %g\n",
                         trace_n, unsigned(prim), count, l.size, s.vcd_lo, s.vcd_hi, s.bp[0], ((s.bp[0] >> 10) & 0xF) + 1, s.bp[0x40], s.bp[0x41], s.bp[0xF3],
                         s.bp[0xC0], s.bp[0xC1], s.bp[0x28], s.bp[0x88], s.bp[0x94], num_texgens, p0, p1, p2, p3, p4, p5, ortho, s.xf_float(0x101A),
                         s.xf_float(0x101B), s.xf_float(0x101D), s.xf_float(0x101E));
        }
        const uint8_t* p = data;
        for (uint32_t v = 0; v < count; ++v) {
            float* out = &vertices_[size_t(v) * kVertexFloats];
            uint32_t pos_mtx = s.matidx_a & 0x3F;
            uint32_t tex_mtx[8];
            for (int i = 0; i < 8; ++i)
                tex_mtx[i] = i < 4 ? (s.matidx_a >> (6 * (i + 1))) & 0x3F : (s.matidx_b >> (6 * (i - 4))) & 0x3F;
            if (l.pmidx) pos_mtx = *p++ & 0x3F;
            for (int i = 0; i < 8; ++i)
                if (l.texmidx[i]) tex_mtx[i] = *p++ & 0x3F;

            auto fetch = [&](AttrMode mode, uint32_t array, uint32_t direct_size) -> const uint8_t* {
                if (mode == kDirect) {
                    const uint8_t* q = p;
                    p += direct_size;
                    return q;
                }
                uint32_t index;
                if (mode == kIndex8) index = *p++;
                else {
                    index = be16(p);
                    p += 2;
                }
                return g_mem.translate(s.array_base[array] + index * s.array_stride[array]);
            };

            float pos[3] = {0, 0, 0};
            if (l.pos != kNone) {
                const uint32_t csize = comp_size(l.pos_fmt);
                const uint8_t* q = fetch(l.pos, 0, l.pos_cnt * csize);
                for (uint32_t i = 0; i < l.pos_cnt; ++i) pos[i] = read_component(q + i * csize, l.pos_fmt, l.pos_shift);
            }
            // Normal (the first three components; binormal and tangent of NBT are not used).
            float nrm[3] = {0, 0, 0};
            if (l.nrm != kNone) {
                const uint32_t csize = comp_size(l.nrm_fmt);
                const uint8_t* q = fetch(l.nrm, 1, l.nrm_cnt * csize);
                const uint32_t shift = l.nrm_fmt >= 4 ? 0 : (csize == 1 ? 6 : 14);
                for (uint32_t i = 0; i < 3; ++i) nrm[i] = read_component(q + i * csize, l.nrm_fmt, shift);
            }

            // Vertex colors; the material registers stand in where the vertex has none.
            float color[2][4];
            for (int c = 0; c < 2; ++c) {
                std::memcpy(color[c], mat_color[c], sizeof(color[c]));
                if (l.col[c] != kNone) {
                    const uint8_t* q = fetch(l.col[c], 2 + c, color_size(l.col_fmt[c]));
                    read_color(q, l.col_fmt[c], color[c]);
                }
            }

            float st[8][2] = {};
            bool has_tex[8] = {};
            for (int t = 0; t < 8; ++t) {
                if (l.tex[t] == kNone) continue;
                const uint32_t csize = comp_size(l.tex_fmt[t]);
                const uint32_t comps = l.tex_cnt[t] + 1;
                const uint8_t* q = fetch(l.tex[t], 4 + t, comps * csize);
                for (uint32_t i = 0; i < comps; ++i) st[t][i] = read_component(q + i * csize, l.tex_fmt[t], l.tex_shift[t]);
                has_tex[t] = true;
            }

            // Position: model-view (3x4 at XF row index pos_mtx), then projection.
            const uint32_t m = pos_mtx * 4;
            float view[3];
            for (int r = 0; r < 3; ++r) {
                view[r] = s.xf_float(m + r * 4 + 0) * pos[0] + s.xf_float(m + r * 4 + 1) * pos[1] +
                          s.xf_float(m + r * 4 + 2) * pos[2] + s.xf_float(m + r * 4 + 3);
            }
            float cx, cy, cz, cw;
            if (ortho) {
                cx = p0 * view[0] + p1;
                cy = p2 * view[1] + p3;
                cz = p4 * view[2] + p5;
                cw = 1.0f;
            } else {
                cx = p0 * view[0] + p1 * view[2];
                cy = p2 * view[1] + p3 * view[2];
                cz = p4 * view[2] + p5;
                cw = -view[2];
            }
            const float zgc = cw != 0.0f ? cz / cw : 0.0f;
            const float depth = (far_z + zgc * z_range) / 16777215.0f;
            // Channel colors: material and ambient sources, lights (XF color/alpha channels).
            float lit[2][4];
            float view_nrm[3];
            transform_normal(s.xf, pos_mtx, nrm, view_nrm);
            light_vertex(s.xf, color, view, view_nrm, lit);
            if (trace && trace_n <= 60 && v == 0) {
                std::fprintf(stderr, "[DRAW]   ndc=(%g %g %g) w=%g lit0=(%g %g %g %g) nrm=(%g %g %g) view_nrm=(%g %g %g) ctrl0=%X ctrl1=%X amb0=%08X mat0=%08X L0col=%08X L1col=%08X L0pos=(%g %g %g) L0dir=(%g %g %g) vcol=(%g %g %g %g)\n",
                             cx / cw, cy / cw, zgc, cw, lit[0][0], lit[0][1], lit[0][2], lit[0][3], nrm[0], nrm[1], nrm[2], view_nrm[0], view_nrm[1], view_nrm[2], s.xf[0x100E], s.xf[0x1010],
                             s.xf[0x100A], s.xf[0x100C], s.xf[0x603 + 0], s.xf[0x603 + 16], s.xf_float(0x60A), s.xf_float(0x60B), s.xf_float(0x60C),
                             s.xf_float(0x60D), s.xf_float(0x60E), s.xf_float(0x60F), color[0][0], color[0][1], color[0][2], color[0][3]);
            }
            if (trace && trace_n <= 3 && v < 4) {
                std::fprintf(stderr, "[DRAW]   v%u mtx=%u pos=(%g %g %g) view=(%g %g %g) clip=(%g %g %g %g) rows:", v, pos_mtx, pos[0], pos[1], pos[2], view[0], view[1], view[2], cx, cy, cz, cw);
                for (int r = 0; r < 12; ++r) std::fprintf(stderr, " %g", s.xf_float(m + r));
                std::fprintf(stderr, "\n");
            }
            out[0] = cx;
            out[1] = cy;
            out[2] = (2.0f * depth - 1.0f) * cw;
            out[3] = cw;
            std::memcpy(out + 4, lit[0], sizeof(float) * 4);
            std::memcpy(out + 8, lit[1], sizeof(float) * 4);

            // Texture coordinates: texgen i reads its source row and applies a 2x4 or 3x4 matrix.
            for (uint32_t i = 0; i < 8; ++i) {
                float* uv = out + 12 + i * 3;
                uv[0] = uv[1] = 0.0f;
                uv[2] = 1.0f;
                if (i >= num_texgens) continue;
                const uint32_t info = s.xf[0x1040 + i];
                const bool projective = (info >> 1) & 1;
                const uint32_t row = (info >> 7) & 0x1F;
                float src[4] = {0, 0, 1, 1};
                if (row == 0) {
                    src[0] = pos[0]; src[1] = pos[1]; src[2] = pos[2]; src[3] = 1;
                } else if (row >= 5 && row < 13) {
                    src[0] = st[row - 5][0]; src[1] = st[row - 5][1]; src[2] = 1; src[3] = 1;
                }
                const uint32_t tm = tex_mtx[i] * 4;
                float res[3];
                for (int r = 0; r < 3; ++r) {
                    res[r] = s.xf_float(tm + r * 4 + 0) * src[0] + s.xf_float(tm + r * 4 + 1) * src[1] +
                             s.xf_float(tm + r * 4 + 2) * src[2] + s.xf_float(tm + r * 4 + 3) * src[3];
                }
                uv[0] = res[0];
                uv[1] = res[1];
                uv[2] = projective ? res[2] : 1.0f;
                if (trace && trace_n <= 40 && v < 3 && i == 0)
                    std::fprintf(stderr, "[DRAW]   uv0 v%u=(%g %g %g) texgen0=%05X mtx=%u rows %g %g %g %g / %g %g %g %g\n", v, uv[0], uv[1], uv[2], info,
                                 tex_mtx[0], s.xf_float(tm), s.xf_float(tm + 1), s.xf_float(tm + 2), s.xf_float(tm + 3), s.xf_float(tm + 4),
                                 s.xf_float(tm + 5), s.xf_float(tm + 6), s.xf_float(tm + 7));
            }
        }
        // GP_TRACE_DRAWS=first-last: in the traced frame, only draws numbered first..last (as printed) are rendered, to see
        // what a group of draws contributes.
        if (trace) {
            static unsigned lo = 0, hi = ~0u;
            static const bool have_range = std::getenv("GP_TRACE_DRAWS") && std::sscanf(std::getenv("GP_TRACE_DRAWS"), "%u-%u", &lo, &hi) == 2;
            if (have_range && (unsigned(trace_n) < lo || unsigned(trace_n) > hi)) return;
        }
        DrawCall call{prim, count, vertices_.data()};
        renderer::draw(call, g_state);
    }
};

Parser g_parser;

}  // namespace

void reset() {
    std::memset(&g_state, 0, sizeof(g_state));
    g_state.bp[0xFE] = 0xFFFFFF;
    g_stats = Stats{};
    g_frames = 0;
    g_parser = Parser{};
}

void feed(const uint8_t* data, size_t n) { g_parser.feed(data, n); }

// Where a write-gather store goes depends on the FIFO registers. The CPU FIFO (PI registers at
// 0xCC00300C base, 0xCC003010 end, 0xCC003014 write pointer) is where the hardware stores the
// gathered data in main memory. When it is the same FIFO the graphics processor reads (CP registers
// 0xCC000020/22 base), the data is executed; otherwise the game is recording a display list and
// the data only goes to memory, to be called later.
void wgp_write(uint32_t size, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < size; ++i) bytes[i] = uint8_t(value >> (8 * (size - 1 - i)));

    uint8_t* hw = g_mem.hw_regs;
    auto reg32 = [&](uint32_t addr) { return be32(hw + (addr - Memory::HW_REG_BASE)); };
    const uint32_t pi_base = reg32(0xCC00300C) & 0x03FFFFFF;
    const uint32_t gp_base = ((uint32_t(be16(hw + 0x22)) << 16) | be16(hw + 0x20)) & 0x03FFFFFF;
    if (pi_base == 0 || gp_base == 0 || pi_base == gp_base) {
        feed(bytes, size);
        return;
    }

    const uint32_t end = reg32(0xCC003010) & 0x03FFFFFF;
    uint32_t wr = reg32(0xCC003014);
    uint32_t phys = wr & 0x03FFFFFF;
    uint8_t* dst = g_mem.translate(0x80000000u | phys);
    std::memcpy(dst, bytes, size);
    phys += size;
    uint32_t wrap = wr & 0x04000000;
    if (phys > end) {  // `end` is the address of the last byte of the FIFO
        phys = pi_base;
        wrap ^= 0x04000000;
    }
    wr = wrap | phys;
    uint8_t* p = hw + (0xCC003014 - Memory::HW_REG_BASE);
    p[0] = uint8_t(wr >> 24);
    p[1] = uint8_t(wr >> 16);
    p[2] = uint8_t(wr >> 8);
    p[3] = uint8_t(wr);
}

uint64_t frames_presented() { return g_frames; }
const Stats& stats() { return g_stats; }

}  // namespace gcrecomp::gp
