// SPDX-License-Identifier: MIT
//
// GX vertex lighting. Register layouts follow the public GameCube documentation (YAGCD, "XF") and
// the GX SDK's documented semantics; the attenuation formulas follow Dolphin's documentation of
// the hardware (behavioral reference only, no Dolphin code is used).
//
// XF registers (word addresses):
//   0x0400-0x045F  normal matrices; matrix i is 3x3 row-major at 0x0400 + 3*i, i = position matrix
//                  index of the vertex (the SDK allocates ids 0, 3, 6, ...), masked to 5 bits
//   0x0600-0x067F  8 lights, 16 words each:
//                    +0..2 reserved
//                    +3    color, RGBA8 (R in the top byte)
//                    +4..6 angle attenuation a0 a1 a2 (float)
//                    +7..9 distance attenuation k0 k1 k2 (float)
//                    +10..12 position (view space; directional lights sit far away) or, for
//                            specular lights, the half-angle vector's source position
//                    +13..15 direction (spot axis, or the specular direction)
//   0x100A/0x100B  ambient color 0/1, RGBA8
//   0x100C/0x100D  material color 0/1, RGBA8
//   0x100E/0x100F  COLOR0CNTRL / COLOR1CNTRL (RGB), 0x1010/0x1011 ALPHA0CNTRL / ALPHA1CNTRL
//     channel control bits:
//       0     material source (0 = register, 1 = vertex color)
//       1     lighting enable
//       2-5   light mask for lights 0-3
//       6     ambient source (0 = register, 1 = vertex color)
//       7-8   diffuse function (0 none, 1 sign, 2 clamp)
//       9     attenuation enable
//       10    attenuation select (0 specular, 1 spotlight/distance)
//       11-14 light mask for lights 4-7
//     Bits 9-10 therefore read as: 0/2 = no attenuation (GX_AF_NONE), 1 = specular (GX_AF_SPEC),
//     3 = spot + distance (GX_AF_SPOT).
//
// Channel result with lighting enabled (GX documentation):
//   out = clamp(material * clamp(ambient + sum over enabled lights of light_color * attn * diffuse))
// The RGB channel uses the color control and the lights' RGB, the alpha channel uses the alpha
// control and the lights' alpha.
#include "lighting.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gcrecomp::gp {

namespace {

float xf_f(const uint32_t* xf, uint32_t addr) {
    float f;
    std::memcpy(&f, &xf[addr], 4);
    return f;
}

void unpack_rgba8(uint32_t packed, float out[4]) {
    out[0] = ((packed >> 24) & 255) / 255.0f;
    out[1] = ((packed >> 16) & 255) / 255.0f;
    out[2] = ((packed >> 8) & 255) / 255.0f;
    out[3] = (packed & 255) / 255.0f;
}

float clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

enum AttnFunc : uint32_t { kAttnNone = 0, kAttnSpec = 1, kAttnNoneAlt = 2, kAttnSpot = 3 };
enum DiffuseFunc : uint32_t { kDiffNone = 0, kDiffSign = 1, kDiffClamp = 2 };

// Scalar light intensity (attenuation * diffuse) of one light at the vertex.
float light_factor(const uint32_t* xf, uint32_t light, uint32_t ctrl, const float pos[3], const float normal[3]) {
    const uint32_t base = kXfLights + light * 16;
    const float a[3] = {xf_f(xf, base + 4), xf_f(xf, base + 5), xf_f(xf, base + 6)};
    const float k[3] = {xf_f(xf, base + 7), xf_f(xf, base + 8), xf_f(xf, base + 9)};
    const float lpos[3] = {xf_f(xf, base + 10), xf_f(xf, base + 11), xf_f(xf, base + 12)};
    const float ldirv[3] = {xf_f(xf, base + 13), xf_f(xf, base + 14), xf_f(xf, base + 15)};

    const uint32_t diffuse = (ctrl >> 7) & 3;
    const uint32_t attn_func = (ctrl >> 9) & 3;

    float to_light[3] = {lpos[0] - pos[0], lpos[1] - pos[1], lpos[2] - pos[2]};
    const float dist2 = dot(to_light, to_light);
    const float dist = std::sqrt(dist2);
    if (dist > 0.0f) {
        for (float& c : to_light) c /= dist;
    }

    float attn = 1.0f;
    switch (attn_func) {
        case kAttnSpec: {
            // Specular: the "direction" is the half-angle axis; attenuation is a rational
            // function of N . H, and only facing lights contribute.
            const float n_l = dot(to_light, normal);
            const float x = n_l >= 0.0f ? std::max(0.0f, dot(ldirv, normal)) : 0.0f;
            float kk[3] = {k[0], k[1], k[2]};
            if (diffuse != kDiffNone) {
                const float len = std::sqrt(dot(kk, kk));
                if (len > 0.0f) {
                    for (float& c : kk) c /= len;
                }
            }
            const float num = a[0] + a[1] * x + a[2] * x * x;
            const float den = kk[0] + kk[1] * x + kk[2] * x * x;
            attn = den != 0.0f ? num / den : 0.0f;
            break;
        }
        case kAttnSpot: {
            const float cos_angle = std::max(0.0f, dot(to_light, ldirv));
            const float cos_att = a[0] + a[1] * cos_angle + a[2] * cos_angle * cos_angle;
            const float dist_att = k[0] + k[1] * dist + k[2] * dist2;
            attn = dist_att != 0.0f ? std::max(0.0f, cos_att) / dist_att : 0.0f;
            break;
        }
        default:  // no attenuation
            if (dist == 0.0f) {
                // A light exactly at the vertex has no direction: use the normal (full diffuse).
                to_light[0] = normal[0];
                to_light[1] = normal[1];
                to_light[2] = normal[2];
            }
            break;
    }

    if (diffuse == kDiffNone) return attn;
    const float n_dot_l = dot(to_light, normal);
    return attn * (diffuse == kDiffClamp ? std::max(0.0f, n_dot_l) : n_dot_l);
}

// Lights one channel's RGB (3 floats out, color control) or alpha (1 float out, alpha control).
void light_component(const uint32_t* xf, uint32_t channel, bool alpha, const float vertex_color[4], const float pos[3],
                     const float normal[3], float* out_rgb_or_alpha) {
    const uint32_t ctrl = xf[(alpha ? kXfAlphaCtrl0 : kXfColorCtrl0) + channel];
    float material[4], ambient[4];
    unpack_rgba8(xf[kXfMaterial0 + channel], material);
    unpack_rgba8(xf[kXfAmbient0 + channel], ambient);
    const float* mat = (ctrl & 1) ? vertex_color : material;
    const int first = alpha ? 3 : 0, count = alpha ? 1 : 3;
    if (!((ctrl >> 1) & 1)) {
        for (int i = 0; i < count; ++i) out_rgb_or_alpha[i] = mat[first + i];
        return;
    }
    const float* amb = ((ctrl >> 6) & 1) ? vertex_color : ambient;
    float sum[3] = {};
    for (int i = 0; i < count; ++i) sum[i] = amb[first + i];
    const uint32_t mask = ((ctrl >> 2) & 0xF) | (((ctrl >> 11) & 0xF) << 4);
    for (uint32_t l = 0; l < 8; ++l) {
        if (!((mask >> l) & 1)) continue;
        const float f = light_factor(xf, l, ctrl, pos, normal);
        float color[4];
        unpack_rgba8(xf[kXfLights + l * 16 + 3], color);
        for (int i = 0; i < count; ++i) sum[i] += color[first + i] * f;
    }
    for (int i = 0; i < count; ++i) out_rgb_or_alpha[i] = clamp01(mat[first + i] * clamp01(sum[i]));
}

}  // namespace

void light_vertex(const uint32_t* xf, const float vertex_color[2][4], const float view_pos[3], const float normal[3],
                  float out[2][4]) {
    for (uint32_t c = 0; c < 2; ++c) {
        light_component(xf, c, false, vertex_color[c], view_pos, normal, out[c]);
        light_component(xf, c, true, vertex_color[c], view_pos, normal, out[c] + 3);
    }
}

void transform_normal(const uint32_t* xf, uint32_t index, const float model_normal[3], float out[3]) {
    const uint32_t base = kXfNormalMatrices + (index & 31) * 3;
    for (int r = 0; r < 3; ++r) {
        out[r] = xf_f(xf, base + r * 3 + 0) * model_normal[0] + xf_f(xf, base + r * 3 + 1) * model_normal[1] +
                 xf_f(xf, base + r * 3 + 2) * model_normal[2];
    }
    const float len = std::sqrt(dot(out, out));
    if (len > 0.0f) {
        for (int r = 0; r < 3; ++r) out[r] /= len;
    }
}

}  // namespace gcrecomp::gp
