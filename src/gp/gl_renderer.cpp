// SPDX-License-Identifier: MIT
//
// OpenGL 3.3 renderer behind the GP command stream, using SDL3 for the window and context.
//
// One fragment shader implements the TEV (texture environment) pipeline with the stage
// configuration passed as uniforms. The EFB is a 640x528 framebuffer object; a copy to the
// external frame buffer blits it to the window.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "gcrecomp/gx/gx.h"
#include "gcrecomp/runtime.h"
#include "efb_copy.h"
#include "gl_loader.h"
#include "gp_decode.h"
#include "gp_internal.h"
#include "input.h"

namespace gcrecomp::gx {
// Defined in src/gx/gx_texture.cpp.
std::vector<uint8_t> decode_texture(const uint8_t* src, uint32_t width, uint32_t height, GXTexFmt fmt);
}  // namespace gcrecomp::gx

namespace gcrecomp::gp::renderer {

namespace {

constexpr int kEfbWidth = 640;
constexpr int kEfbHeight = 528;

SDL_Window* g_window = nullptr;
SDL_GLContext g_context = nullptr;
int g_window_w = 0, g_window_h = 0;
bool g_closed = false;
const char* g_shot_dir = nullptr;
int g_shot_max = 0;
int g_shot_count = 0;

GLuint g_fbo = 0, g_color = 0, g_depth = 0;
GLuint g_vao = 0, g_vbo = 0, g_program = 0;
GLuint g_white = 0;
uint64_t g_indirect_draws = 0;
// How often the game used the features that are only visible on screen, reported at shutdown.
struct FeatureUse {
    uint64_t c14x2_textures = 0, mip_textures = 0, fog_draws = 0, const_alpha_draws = 0, logic_op_draws = 0, early_z_passes = 0;
} g_use;

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_c0;
layout(location = 2) in vec4 a_c1;
layout(location = 3) in vec3 a_uv0;
layout(location = 4) in vec3 a_uv1;
layout(location = 5) in vec3 a_uv2;
layout(location = 6) in vec3 a_uv3;
layout(location = 7) in vec3 a_uv4;
layout(location = 8) in vec3 a_uv5;
layout(location = 9) in vec3 a_uv6;
layout(location = 10) in vec3 a_uv7;
out vec4 v_c0;
out vec4 v_c1;
out vec3 v_uv[8];
void main() {
    gl_Position = a_pos;
    v_c0 = a_c0;
    v_c1 = a_c1;
    v_uv[0] = a_uv0; v_uv[1] = a_uv1; v_uv[2] = a_uv2; v_uv[3] = a_uv3;
    v_uv[4] = a_uv4; v_uv[5] = a_uv5; v_uv[6] = a_uv6; v_uv[7] = a_uv7;
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec4 v_c0;
in vec4 v_c1;
in vec3 v_uv[8];
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_src1;   // TEV alpha, for blend factors when a constant alpha replaces o_color.a

uniform sampler2D u_tex0; uniform sampler2D u_tex1; uniform sampler2D u_tex2; uniform sampler2D u_tex3;
uniform sampler2D u_tex4; uniform sampler2D u_tex5; uniform sampler2D u_tex6; uniform sampler2D u_tex7;
uniform int u_nstages;
uniform uvec4 u_stage[16];   // color env, alpha env, tref (map | coord << 3 | enable << 6 | chan << 7), ksel
uniform ivec4 u_swap[4];     // component selectors r, g, b, a of each swap table
uniform vec4 u_reg[4];       // PREV, C0, C1, C2
uniform vec4 u_konst[4];
uniform vec4 u_fog_f;        // x: A, y: C
uniform ivec4 u_fog_i;       // x: B magnitude, y: B shift, z: orthographic, w: type (0 off, 2 linear, 4-7 exp)
uniform vec3 u_fog_color;
uniform float u_const_alpha; // alpha written to the EFB when >= 0 (GXSetDstAlpha), else the TEV alpha
uniform uvec2 u_alpha_cmp;   // x: comparison codes (c0 | c1 << 3 | logic << 6), y: refs (ref0 | ref1 << 8)

vec4 sample_map(int map, vec2 uv) {
    if (map == 0) return texture(u_tex0, uv);
    if (map == 1) return texture(u_tex1, uv);
    if (map == 2) return texture(u_tex2, uv);
    if (map == 3) return texture(u_tex3, uv);
    if (map == 4) return texture(u_tex4, uv);
    if (map == 5) return texture(u_tex5, uv);
    if (map == 6) return texture(u_tex6, uv);
    return texture(u_tex7, uv);
}

vec4 swizzle(vec4 v, int table) {
    ivec4 s = u_swap[table];
    float c[4] = float[4](v.r, v.g, v.b, v.a);
    return vec4(c[s.x], c[s.y], c[s.z], c[s.w]);
}


float frac8(uint sel) {
    float f[8] = float[8](1.0, 0.875, 0.75, 0.625, 0.5, 0.375, 0.25, 0.125);
    return f[sel & 7u];
}

vec3 kcolor_sel(uint sel) {
    if (sel < 8u) return vec3(frac8(sel));
    if (sel >= 12u && sel < 16u) return u_konst[sel - 12u].rgb;
    if (sel >= 16u && sel < 20u) return vec3(u_konst[sel - 16u].r);
    if (sel >= 20u && sel < 24u) return vec3(u_konst[sel - 20u].g);
    if (sel >= 24u && sel < 28u) return vec3(u_konst[sel - 24u].b);
    if (sel >= 28u && sel < 32u) return vec3(u_konst[sel - 28u].a);
    return vec3(1.0);
}

float kalpha_sel(uint sel) {
    if (sel < 8u) return frac8(sel);
    if (sel >= 16u && sel < 20u) return u_konst[sel - 16u].r;
    if (sel >= 20u && sel < 24u) return u_konst[sel - 20u].g;
    if (sel >= 24u && sel < 28u) return u_konst[sel - 24u].b;
    if (sel >= 28u && sel < 32u) return u_konst[sel - 28u].a;
    return 1.0;
}

bool compare(uint func, float a, float ref) {
    if (func == 0u) return false;
    if (func == 1u) return a < ref;
    if (func == 2u) return a == ref;
    if (func == 3u) return a <= ref;
    if (func == 4u) return a > ref;
    if (func == 5u) return a != ref;
    if (func == 6u) return a >= ref;
    return true;
}

// Mirrors fog_factor in gp_decode.cpp.
float fog_amount() {
    int type = u_fog_i.w;
    if (type == 0) return 0.0;
    int z24 = int(gl_FragCoord.z * 16777215.0 + 0.5);
    float ze;
    if (u_fog_i.z != 0) {
        ze = u_fog_f.x * float(z24) / 16777216.0;
    } else {
        int denom = u_fog_i.x - (z24 >> u_fog_i.y);
        ze = denom != 0 ? u_fog_f.x * 16777216.0 / float(denom) : 1e30;
    }
    float f = clamp(ze - u_fog_f.y, 0.0, 1.0);
    if (type == 4) return 1.0 - exp2(-8.0 * f);
    if (type == 5) return 1.0 - exp2(-8.0 * f * f);
    if (type == 6) return exp2(-8.0 * (1.0 - f));
    if (type == 7) return exp2(-8.0 * (1.0 - f) * (1.0 - f));
    return f;
}

void main() {
    vec4 reg[4] = vec4[4](u_reg[0], u_reg[1], u_reg[2], u_reg[3]);
    for (int i = 0; i < u_nstages; ++i) {
        uint ce = u_stage[i].x, ae = u_stage[i].y, tr = u_stage[i].z, ks = u_stage[i].w;
        int texmap = int(tr & 7u);
        int texcoord = int((tr >> 3) & 7u);
        bool texen = ((tr >> 6) & 1u) != 0u;
        uint chan = (tr >> 7) & 7u;

        vec4 tex = vec4(1.0);
        if (texen) {
            vec3 uv = v_uv[texcoord];
            vec2 st = uv.z != 0.0 ? uv.xy / uv.z : uv.xy;
            tex = swizzle(sample_map(texmap, st), int((ae >> 2) & 3u));
        }
        vec4 ras = vec4(0.0);
        if (chan == 0u) ras = v_c0;
        else if (chan == 1u) ras = v_c1;
        ras = swizzle(ras, int(ae & 3u));

        vec3 kc = kcolor_sel(ks & 31u);
        float ka = kalpha_sel((ks >> 5) & 31u);

        vec3 cin[16] = vec3[16](
            reg[0].rgb, vec3(reg[0].a), reg[1].rgb, vec3(reg[1].a),
            reg[2].rgb, vec3(reg[2].a), reg[3].rgb, vec3(reg[3].a),
            tex.rgb, vec3(tex.a), ras.rgb, vec3(ras.a),
            vec3(1.0), vec3(0.5), kc, vec3(0.0));
        vec3 ca = cin[(ce >> 12) & 15u], cb = cin[(ce >> 8) & 15u], cc = cin[(ce >> 4) & 15u], cd = cin[ce & 15u];
        float bias_c[4] = float[4](0.0, 0.5, -0.5, 0.0);
        float scale[4] = float[4](1.0, 2.0, 4.0, 0.5);
        float sign_c = ((ce >> 18) & 1u) != 0u ? -1.0 : 1.0;
        vec3 color = (cd + sign_c * mix(ca, cb, cc) + bias_c[(ce >> 16) & 3u]) * scale[(ce >> 20) & 3u];
        if (((ce >> 19) & 1u) != 0u) color = clamp(color, 0.0, 1.0);

        float ain[8] = float[8](reg[0].a, reg[1].a, reg[2].a, reg[3].a, tex.a, ras.a, ka, 0.0);
        float aa = ain[(ae >> 13) & 7u], ab = ain[(ae >> 10) & 7u], ac = ain[(ae >> 7) & 7u], ad = ain[(ae >> 4) & 7u];
        float sign_a = ((ae >> 18) & 1u) != 0u ? -1.0 : 1.0;
        float alpha = (ad + sign_a * mix(aa, ab, ac) + bias_c[(ae >> 16) & 3u]) * scale[(ae >> 20) & 3u];
        if (((ae >> 19) & 1u) != 0u) alpha = clamp(alpha, 0.0, 1.0);

        uint cdest = (ce >> 22) & 3u, adest = (ae >> 22) & 3u;
        reg[cdest].rgb = color;
        reg[adest].a = alpha;
    }
    vec4 result = vec4(reg[0].rgb, reg[0].a);
    result.rgb = mix(result.rgb, u_fog_color, fog_amount());

    uint c0 = u_alpha_cmp.x & 7u, c1 = (u_alpha_cmp.x >> 3) & 7u, logic = (u_alpha_cmp.x >> 6) & 3u;
    float r0 = float(u_alpha_cmp.y & 255u) / 255.0, r1 = float((u_alpha_cmp.y >> 8) & 255u) / 255.0;
    bool p0 = compare(c0, result.a, r0), p1 = compare(c1, result.a, r1);
    bool pass = logic == 0u ? (p0 && p1) : logic == 1u ? (p0 || p1) : logic == 2u ? (p0 != p1) : (p0 == p1);
    if (!pass) discard;
    o_src1 = vec4(vec3(result.a), result.a);
    if (u_const_alpha >= 0.0) result.a = u_const_alpha;
    o_color = result;
}
)";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[GL] shader compile failed:\n%s\n", log);
        return 0;
    }
    return shader;
}

struct CachedTexture {
    GLuint id = 0;
    uint64_t hash = 0;
};
std::unordered_map<uint64_t, CachedTexture> g_textures;

uint64_t hash_bytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

GLuint texture_for_map(const State& s, int map) {
    const uint32_t img0 = s.bp[map < 4 ? 0x88 + map : 0xA8 + (map - 4)];
    const uint32_t img3 = s.bp[map < 4 ? 0x94 + map : 0xB4 + (map - 4)];
    const uint32_t mode0 = s.bp[map < 4 ? 0x80 + map : 0xA0 + (map - 4)];
    const uint32_t mode1 = s.bp[map < 4 ? 0x84 + map : 0xA4 + (map - 4)];
    const uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 0xF;
    const uint32_t addr = (img3 & 0xFFFFFF) << 5;
    const bool indexed = fmt == kTexC4 || fmt == kTexC8 || fmt == kTexC14X2;
    const uint32_t settlut = indexed ? s.bp[map < 4 ? 0x98 + map : 0xB8 + (map - 4)] : 0;
    const uint32_t levels = mip_level_count(w, h, mode0, mode1);
    const size_t bytes = mip_chain_bytes(w, h, fmt, levels);
    const uint8_t* src = g_mem.translate(addr);
    const uint64_t key = (uint64_t(addr) << 24) ^ (uint64_t(w) << 12) ^ (uint64_t(h) << 4) ^ fmt ^ (uint64_t(levels) << 56);
    uint64_t hash = hash_bytes(src, bytes);
    if (indexed) {
        // Hash the part of the TLUT the format can reach: 16, 256 or 16384 entries of 2 bytes.
        const size_t entries = fmt == kTexC4 ? 16 : fmt == kTexC8 ? 256 : 16384;
        const size_t base = size_t(settlut & 0x3FF) * 512;
        const size_t len = std::min(entries * 2, sizeof(s.tlut) - base);
        hash ^= hash_bytes(s.tlut + base, len) * 31 + settlut;
    }
    CachedTexture& t = g_textures[key];
    if (t.id == 0 || t.hash != hash) {
        auto decode_level = [&](uint32_t level) {
            const uint32_t lw = mip_dim(w, level), lh = mip_dim(h, level);
            const uint8_t* lsrc = src + mip_level_offset(w, h, fmt, level);
            return indexed ? decode_indexed(s.tlut, sizeof(s.tlut), lsrc, lw, lh, fmt, settlut)
                           : gx::decode_texture(lsrc, lw, lh, gx::GXTexFmt(fmt));
        };
        const std::vector<uint8_t> rgba = decode_level(0);
        if (const char* dir = std::getenv("GP_DUMP_TEX")) {
            // GP_DUMP_TEX_MAX: how many textures to write (default 24); GP_DUMP_TEX_AFTER: only once this many
            // frames have been presented (skips the textures of earlier scenes, such as a movie).
            static int dumped = 0;
            static const int max_dump = std::getenv("GP_DUMP_TEX_MAX") ? std::atoi(std::getenv("GP_DUMP_TEX_MAX")) : 24;
            static const long after = std::getenv("GP_DUMP_TEX_AFTER") ? std::atol(std::getenv("GP_DUMP_TEX_AFTER")) : 0;
            // A texture is written once per (address, format, size): a movie re-uploading into one buffer is dumped once.
            static std::unordered_map<uint64_t, bool> seen;
            const uint64_t key = (uint64_t(addr) << 32) ^ (uint64_t(fmt) << 28) ^ (uint64_t(w) << 14) ^ h;
            if (dumped < max_dump && long(g_frames) >= after && seen.emplace(key, true).second) {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/tex_%02d_fmt%X_%ux%u_%06X.ppm", dir, dumped++, fmt, w, h, addr);
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                    for (size_t i = 0; i < size_t(w) * h; ++i) {
                        const uint8_t a = rgba[i * 4 + 3];
                        const uint8_t px[3] = {uint8_t(rgba[i * 4] * a / 255 + 64 * (255 - a) / 255),
                                               uint8_t(rgba[i * 4 + 1] * a / 255 + 64 * (255 - a) / 255),
                                               uint8_t(rgba[i * 4 + 2] * a / 255 + 64 * (255 - a) / 255)};
                        std::fwrite(px, 1, 3, f);
                    }
                    std::fclose(f);
                }
            }
        }
        if (t.id == 0) glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        for (uint32_t level = 1; level < levels; ++level) {
            const std::vector<uint8_t> mip = decode_level(level);
            glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, mip_dim(w, level), mip_dim(h, level), 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, mip.data());
        }
        if (fmt == kTexC14X2) ++g_use.c14x2_textures;
        if (levels > 1) ++g_use.mip_textures;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(levels - 1));
        t.hash = hash;
    }
    glBindTexture(GL_TEXTURE_2D, t.id);
    const GLint wrap[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[(mode0 & 3) % 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[((mode0 >> 2) & 3) % 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (mode0 & 0x10) ? GL_LINEAR : GL_NEAREST);
    static const GLint min_filters[6] = {GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                         GL_LINEAR,  GL_LINEAR_MIPMAP_NEAREST,  GL_LINEAR_MIPMAP_LINEAR};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filters[uint32_t(effective_min_filter(mode0, levels))]);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, lod_bias_of(mode0));
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, min_lod_of(mode1));
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, max_lod_of(mode1));
    return t.id;
}

GLenum blend_factor(uint32_t f, bool is_src) {
    switch (f) {
        case 0: return GL_ZERO;
        case 1: return GL_ONE;
        case 2: return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
        case 3: return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
        case 4: return GL_SRC_ALPHA;
        case 5: return GL_ONE_MINUS_SRC_ALPHA;
        case 6: return GL_DST_ALPHA;
        default: return GL_ONE_MINUS_DST_ALPHA;
    }
}

GLenum compare_func(uint32_t f) {
    static const GLenum table[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return table[f & 7];
}

void set_uniform_1i(const char* name, int v) { glUniform1i(glGetUniformLocation(g_program, name), v); }

void apply_state(const State& s) {
    glUseProgram(g_program);

    // Viewport and scissor, in EFB coordinates (origin top-left) converted to GL (origin bottom-left).
    const float wd = s.xf_float(0x101A), ht = s.xf_float(0x101B), xo = s.xf_float(0x101D), yo = s.xf_float(0x101E);
    const float x_off = float((s.bp[0x59] & 0x3FF) * 2), y_off = float(((s.bp[0x59] >> 10) & 0x3FF) * 2);
    const float vx = xo - wd - x_off, vy = yo + ht - y_off, vw = 2.0f * wd, vh = -2.0f * ht;
    if (vw > 0 && vh > 0) glViewport(int(vx), int(kEfbHeight - (vy + vh)), int(vw), int(vh));

    const uint32_t tl = s.bp[0x20], br = s.bp[0x21];
    const int sx = int((tl >> 12) & 0x7FF) - 342, sy = int(tl & 0x7FF) - 342;
    const int sx2 = int((br >> 12) & 0x7FF) - 342, sy2 = int(br & 0x7FF) - 342;
    const int sw = sx2 - sx + 1, sh = sy2 - sy + 1;
    glEnable(GL_SCISSOR_TEST);
    if (sw > 0 && sh > 0) glScissor(sx, kEfbHeight - (sy + sh), sw, sh);

    const uint32_t cull = (s.bp[0x00] >> 14) & 3;
    if (cull == 0) glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        glCullFace(cull == 1 ? GL_BACK : cull == 2 ? GL_FRONT : GL_FRONT_AND_BACK);
    }

    const uint32_t zmode = s.bp[0x40];
    if (zmode & 1) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(compare_func((zmode >> 1) & 7));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask((zmode & 0x10) ? GL_TRUE : GL_FALSE);

    const BlendState blend = decode_blend(s.bp[0x41], s.bp[0x42]);
    // Constant alpha: o_color.a carries the constant, so the blend factors that use the source alpha read
    // the TEV alpha from the second shader output, and the alpha channel is written as it comes.
    const bool const_alpha = blend.const_alpha;
    auto factor = [&](uint32_t f, bool is_src) {
        if (const_alpha && f == 4) return GLenum(GL_SRC1_COLOR);
        if (const_alpha && f == 5) return GLenum(GL_ONE_MINUS_SRC1_COLOR);
        return blend_factor(f, is_src);
    };
    if (blend.blend) {
        glEnable(GL_BLEND);
        const GLenum src = factor(blend.src_factor, true), dst = factor(blend.dst_factor, false);
        if (const_alpha) glBlendFuncSeparate(src, dst, GL_ONE, GL_ZERO);
        else glBlendFunc(src, dst);
        const GLenum rgb_eq = blend.subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
        glBlendEquationSeparate(rgb_eq, const_alpha ? GL_FUNC_ADD : rgb_eq);
    } else {
        glDisable(GL_BLEND);
    }
    if (const_alpha) ++g_use.const_alpha_draws;
    if (blend.logic_op) {
        ++g_use.logic_op_draws;
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(GL_CLEAR + blend.logic_mode);  // GX_LO_* follows the GL enum order
    } else {
        glDisable(GL_COLOR_LOGIC_OP);
    }
    glUniform1f(glGetUniformLocation(g_program, "u_const_alpha"), const_alpha ? float(blend.const_alpha_value) / 255.0f : -1.0f);
    glColorMask(blend.color_update, blend.color_update, blend.color_update, blend.alpha_update);

    // TEV stages.
    const int nstages = int(((s.bp[0x00] >> 10) & 0xF) + 1);
    uint32_t stage[16][4] = {};
    for (int i = 0; i < 16; ++i) {
        const uint32_t tref = s.bp[0x28 + i / 2];
        const uint32_t bits = (i & 1) ? (tref >> 12) : tref;
        const uint32_t map = bits & 7, coord = (bits >> 3) & 7, enable = (bits >> 6) & 1, chan = (bits >> 7) & 7;
        const uint32_t ksel = s.bp[0xF6 + i / 2];
        const uint32_t kc = (i & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31;
        const uint32_t ka = (i & 1) ? (ksel >> 19) & 31 : (ksel >> 9) & 31;
        stage[i][0] = s.bp[0xC0 + 2 * i];
        stage[i][1] = s.bp[0xC1 + 2 * i];
        stage[i][2] = map | (coord << 3) | (enable << 6) | (chan << 7);
        stage[i][3] = kc | (ka << 5);
    }
    glUniform1i(glGetUniformLocation(g_program, "u_nstages"), nstages);
    glUniform4uiv(glGetUniformLocation(g_program, "u_stage"), 16, &stage[0][0]);

    int swap[4][4];
    for (int t = 0; t < 4; ++t) {
        const uint32_t lo = s.bp[0xF6 + 2 * t], hi = s.bp[0xF7 + 2 * t];
        swap[t][0] = int(lo & 3);
        swap[t][1] = int((lo >> 2) & 3);
        swap[t][2] = int(hi & 3);
        swap[t][3] = int((hi >> 2) & 3);
    }
    glUniform4iv(glGetUniformLocation(g_program, "u_swap"), 4, &swap[0][0]);
    glUniform4fv(glGetUniformLocation(g_program, "u_reg"), 4, &s.tev_reg[0][0]);
    glUniform4fv(glGetUniformLocation(g_program, "u_konst"), 4, &s.tev_konst[0][0]);

    const uint32_t ac = s.bp[0xF3];
    const uint32_t cmp = ((ac >> 16) & 7) | (((ac >> 19) & 7) << 3) | (((ac >> 22) & 3) << 6);
    const uint32_t refs = (ac & 0xFF) | (((ac >> 8) & 0xFF) << 8);
    glUniform2ui(glGetUniformLocation(g_program, "u_alpha_cmp"), cmp, refs);

    const FogParams fog = decode_fog(s.bp);
    if (fog.type != kFogOff) ++g_use.fog_draws;
    glUniform4f(glGetUniformLocation(g_program, "u_fog_f"), fog.a, fog.c, 0.0f, 0.0f);
    glUniform4i(glGetUniformLocation(g_program, "u_fog_i"), GLint(fog.b_mag), GLint(fog.b_shift), fog.ortho ? 1 : 0,
                GLint(fog.type));
    glUniform3fv(glGetUniformLocation(g_program, "u_fog_color"), 1, fog.color);

    // Textures: bind the maps the stages use; the others get the white texture.
    bool used[8] = {};
    for (int i = 0; i < nstages; ++i)
        if ((stage[i][2] >> 6) & 1) used[stage[i][2] & 7] = true;
    for (int m = 0; m < 8; ++m) {
        glActiveTexture(GL_TEXTURE0 + m);
        if (used[m]) texture_for_map(s, m);
        else glBindTexture(GL_TEXTURE_2D, g_white);
        char name[16];
        std::snprintf(name, sizeof(name), "u_tex%d", m);
        set_uniform_1i(name, m);
    }
}

void print_feature_use() {
    std::fprintf(stderr, "[GL] feature use: C14X2 textures %llu, mipmapped textures %llu, fog draws %llu, constant-alpha draws %llu, logic-op draws %llu, early-Z passes %llu\n",
                 (unsigned long long)g_use.c14x2_textures, (unsigned long long)g_use.mip_textures, (unsigned long long)g_use.fog_draws,
                 (unsigned long long)g_use.const_alpha_draws, (unsigned long long)g_use.logic_op_draws, (unsigned long long)g_use.early_z_passes);
    if (g_indirect_draws) std::fprintf(stderr, "[GL] %llu draw(s) used indirect texturing (not implemented)\n", (unsigned long long)g_indirect_draws);
}

void write_screenshot() {
    static uint64_t presented = 0;
    static const int every = [] {
        const char* v = std::getenv("GP_SHOT_EVERY");
        return v ? std::max(1, std::atoi(v)) : 1;
    }();
    if (presented++ % every != 0) return;
    if (!g_shot_dir || (g_shot_max && g_shot_count >= g_shot_max)) return;
    std::vector<uint8_t> px(size_t(g_window_w) * g_window_h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // The blit that precedes this call leaves the EFB bound for reading: read the window instead.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, g_window_w, g_window_h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    char path[512];
    std::snprintf(path, sizeof(path), "%s/frame_%04d.ppm", g_shot_dir, g_shot_count++);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", g_window_w, g_window_h);
        for (int y = g_window_h - 1; y >= 0; --y) std::fwrite(&px[size_t(y) * g_window_w * 3], 1, size_t(g_window_w) * 3, f);
        std::fclose(f);
    }
}

}  // namespace

bool init(int window_w, int window_h, const char* screenshot_dir, int max_screenshots) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[GL] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("tos-recomp", window_w, window_h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        std::fprintf(stderr, "[GL] cannot create a window: %s\n", SDL_GetError());
        return false;
    }
    g_context = SDL_GL_CreateContext(g_window);
    if (!g_context) {
        std::fprintf(stderr, "[GL] cannot create an OpenGL 3.3 context: %s\n", SDL_GetError());
        return false;
    }
    if (!gl_load_functions()) {
        std::fprintf(stderr, "[GL] the driver does not expose the required OpenGL 3.3 entry points\n");
        return false;
    }
    SDL_GL_SetSwapInterval(0);
    SDL_GetWindowSizeInPixels(g_window, &g_window_w, &g_window_h);
    g_shot_dir = screenshot_dir;
    g_shot_max = max_screenshots;
    std::fprintf(stderr, "[GL] %s, %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                 reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    glGenFramebuffers(1, &g_fbo);
    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEfbWidth, kEfbHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenRenderbuffers(1, &g_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kEfbWidth, kEfbHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[GL] EFB framebuffer is incomplete\n");
        return false;
    }
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;
    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glLinkProgram(g_program);
    GLint linked = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048];
        glGetProgramInfoLog(g_program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[GL] link failed:\n%s\n", log);
        return false;
    }

    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    const GLsizei stride = kVertexFloats * sizeof(float);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(8 * sizeof(float)));
    for (int i = 0; i < 8; ++i) glVertexAttribPointer(3 + i, 3, GL_FLOAT, GL_FALSE, stride, (void*)((12 + 3 * i) * sizeof(float)));
    for (int i = 0; i < 11; ++i) glEnableVertexAttribArray(i);

    const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    return true;
}

void shutdown() {
    print_feature_use();
    if (g_context) SDL_GL_DestroyContext(g_context);
    if (g_window) SDL_DestroyWindow(g_window);
    g_context = nullptr;
    g_window = nullptr;
    SDL_Quit();
}

void draw(const DrawCall& call, const State& s) {
    static uint64_t calls = 0;
    ++calls;
    if ((calls == 1 || calls == 5000 || calls == 30000 || calls == 100000) && std::getenv("GP_TRACE")) {
        std::fprintf(stderr, "[GL] ---- draw #%llu\n", (unsigned long long)calls);
        const int nst = int(((s.bp[0x00] >> 10) & 0xF) + 1);
        std::fprintf(stderr, "[GL] draw prim=%u n=%u genmode=%06X ntev=%d zmode=%02X blend=%04X acmp=%06X\n", unsigned(call.primitive),
                     call.vertex_count, s.bp[0], nst, s.bp[0x40], s.bp[0x41], s.bp[0xF3]);
        std::fprintf(stderr, "[GL]   viewport wd=%g ht=%g xo=%g yo=%g zr=%g far=%g proj=%g %g %g %g %g %g ortho=%u\n",
                     s.xf_float(0x101A), s.xf_float(0x101B), s.xf_float(0x101D), s.xf_float(0x101E), s.xf_float(0x101C),
                     s.xf_float(0x101F), s.xf_float(0x1020), s.xf_float(0x1021), s.xf_float(0x1022), s.xf_float(0x1023),
                     s.xf_float(0x1024), s.xf_float(0x1025), s.xf[0x1026]);
        for (int i = 0; i < nst && i < 4; ++i)
            std::fprintf(stderr, "[GL]   stage %d: color=%06X alpha=%06X tref=%06X ksel=%06X\n", i, s.bp[0xC0 + 2 * i], s.bp[0xC1 + 2 * i],
                         s.bp[0x28 + i / 2], s.bp[0xF6 + i / 2]);
        for (uint32_t v = 0; v < call.vertex_count && v < 4; ++v) {
            const float* f = call.vertices + size_t(v) * kVertexFloats;
            std::fprintf(stderr, "[GL]   v%u clip=(%g %g %g %g) c0=(%g %g %g %g) uv0=(%g %g %g)\n", v, f[0], f[1], f[2], f[3], f[4], f[5],
                         f[6], f[7], f[12], f[13], f[14]);
        }
        std::fprintf(stderr, "[GL]   tex0 img0=%06X img3=%06X mode0=%06X\n", s.bp[0x88], s.bp[0x94], s.bp[0x80]);
    }
    if (const uint32_t ind = indirect_tev_stages(s.bp)) {
        // Indirect texturing is not implemented: the draw is rendered as if the stages were direct.
        // Say which games need it, once per distinct configuration.
        ++g_indirect_draws;
        static uint32_t seen[8];
        static int nseen = 0;
        const uint32_t sig = ind ^ (((s.bp[0x00] >> 16) & 7) << 16);
        bool known = false;
        for (int i = 0; i < nseen; ++i) known |= seen[i] == sig;
        if (!known && nseen < 8) {
            seen[nseen++] = sig;
            std::fprintf(stderr, "[GL] indirect texturing used (draw #%llu): %u indirect stage(s), TEV stages 0x%04X; not implemented, drawn as direct\n",
                         (unsigned long long)calls, (s.bp[0x00] >> 16) & 7, ind);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    apply_state(s);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    const float* data = call.vertices;
    GLenum mode = GL_TRIANGLES;
    uint32_t count = call.vertex_count;
    std::vector<float> expanded;
    switch (call.primitive) {
        case kQuads: {
            expanded.reserve(size_t(count) / 4 * 6 * kVertexFloats);
            for (uint32_t q = 0; q + 3 < count; q += 4) {
                static const int order[6] = {0, 1, 2, 0, 2, 3};
                for (int k : order) {
                    const float* v = data + size_t(q + k) * kVertexFloats;
                    expanded.insert(expanded.end(), v, v + kVertexFloats);
                }
            }
            data = expanded.data();
            count = uint32_t(expanded.size() / kVertexFloats);
            mode = GL_TRIANGLES;
            break;
        }
        case kTriangles: mode = GL_TRIANGLES; break;
        case kTriangleStrip: mode = GL_TRIANGLE_STRIP; break;
        case kTriangleFan: mode = GL_TRIANGLE_FAN; break;
        case kLines: mode = GL_LINES; break;
        case kLineStrip: mode = GL_LINE_STRIP; break;
        case kPoints: mode = GL_POINTS; break;
        default: return;
    }
    if (count == 0) return;
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(size_t(count) * kVertexFloats * sizeof(float)), data, GL_STREAM_DRAW);
    if (std::getenv("GP_XHIST")) {
        static float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        static int frame_draws = 0;
        for (uint32_t v = 0; v < count; ++v) {
            const float w = data[size_t(v) * kVertexFloats + 3];
            const float cx = data[size_t(v) * kVertexFloats] / w, cy = data[size_t(v) * kVertexFloats + 1] / w;
            minx = std::min(minx, cx); maxx = std::max(maxx, cx); miny = std::min(miny, cy); maxy = std::max(maxy, cy);
        }
        ++frame_draws;
        if (frame_draws == 60) {
            std::fprintf(stderr, "[GL] clip bbox over 60 draws: x %g..%g y %g..%g\n", minx, maxx, miny, maxy);
            frame_draws = 0; minx = miny = 1e9f; maxx = maxy = -1e9f;
        }
    }
    glDrawArrays(mode, 0, GLsizei(count));
    if (early_z_writes_rejected_pixels(s.bp)) {
        // Z compare before texturing: pixels the alpha test rejected still update depth. Replay the
        // draw without color and without the alpha test; pixels that passed already wrote their depth
        // and fail the depth test against themselves, the rejected ones write it now. apply_state
        // restores the color mask and the alpha test uniform for the next draw.
        ++g_use.early_z_passes;
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glUniform2ui(glGetUniformLocation(g_program, "u_alpha_cmp"), 7u | (7u << 3), 0u);
        glDrawArrays(mode, 0, GLsizei(count));
    }
}

// Copy a region of the EFB to main memory in the console's tiled layout (BP 0x52 with the XFB bit
// clear). Reads back from the GPU. The texture cache keys on a hash of the guest bytes, so entries
// overlapping the destination are re-decoded on their next use without explicit invalidation.
static void copy_efb_to_memory(const State& s, uint32_t copy_cmd) {
    EfbCopyParams p = make_efb_copy_params(copy_cmd, s.bp[0x49], s.bp[0x4A], s.bp[0x4D], s.bp[0x4E],
                                           (s.bp[0x43] & 7) == 3);
    p.w = std::min(p.w, kEfbWidth - p.x);
    p.h = std::min(p.h, kEfbHeight - p.y);
    const uint32_t addr = (s.bp[0x4B] & 0xFFFFFF) << 5;
    if (p.w <= 0 || p.h <= 0) return;
    if (uint64_t(addr) + efb_copy_extent(p) > g_mem.ram_size) {
        std::fprintf(stderr, "[GL] EFB copy to 0x%06X (+%zu) is outside RAM, ignored\n", addr, efb_copy_extent(p));
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const int gl_y = kEfbHeight - (p.y + p.h);
    std::vector<uint8_t> rgba(size_t(p.w) * p.h * 4);
    std::vector<uint32_t> depth;
    if (p.depth) {
        depth.resize(size_t(p.w) * p.h);
        glReadPixels(p.x, gl_y, p.w, p.h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, depth.data());
        for (uint32_t& d : depth) d >>= 8;
    } else {
        glReadPixels(p.x, gl_y, p.w, p.h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    // GL rows run bottom to top.
    auto flip = [&](auto& v, size_t row_elems) {
        for (int a = 0, b = p.h - 1; a < b; ++a, --b)
            std::swap_ranges(v.begin() + a * row_elems, v.begin() + (a + 1) * row_elems, v.begin() + b * row_elems);
    };
    if (p.depth) flip(depth, size_t(p.w)); else flip(rgba, size_t(p.w) * 4);
    encode_efb_copy(p, rgba.data(), p.depth ? depth.data() : nullptr, g_mem.translate(addr));
}

void copy_efb(const CopyRequest& r, const State& s) {
    static int traced = 0;
    if (traced < 6 && std::getenv("GP_TRACE")) {
        ++traced;
        std::fprintf(stderr, "[GL] copy to_xfb=%d clear=%d src=(%d,%d) %dx%d bp4D=%06X bp4E=%06X window=%dx%d\n", r.to_xfb, r.clear, r.x, r.y,
                     r.w, r.h, s.bp[0x4D], s.bp[0x4E], g_window_w, g_window_h);
    }
    if (r.to_xfb) {
        static uint64_t presented = 0;
        if (++presented % 600 == 0 && std::getenv("GP_TRACE")) print_feature_use();
        // Blit the copied EFB region to the window and present.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, g_window_w, g_window_h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        const int sy0 = kEfbHeight - (r.y + r.h), sy1 = kEfbHeight - r.y;
        glBlitFramebuffer(r.x, sy0, r.x + r.w, sy1, 0, 0, g_window_w, g_window_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        write_screenshot();
        SDL_GL_SwapWindow(g_window);
        SDL_GetWindowSizeInPixels(g_window, &g_window_w, &g_window_h);
    }
    if (!r.to_xfb) copy_efb_to_memory(s, s.bp[0x52]);
    if (r.clear) {
        const uint32_t ar = s.bp[0x4F], gb = s.bp[0x50], z = s.bp[0x51];
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(((ar >> 8) & 255) / 255.0f, ((gb >> 8) & 255) / 255.0f, (gb & 255) / 255.0f, (ar & 255) / 255.0f);
        glClearDepth((z & 0xFFFFFF) / 16777215.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
}

bool pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_closed = true;
        input::handle_event(e);
    }
    input::update();
    return !g_closed;
}

}  // namespace gcrecomp::gp::renderer
