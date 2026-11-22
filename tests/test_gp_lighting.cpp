// SPDX-License-Identifier: MIT
// Unit tests for GX vertex lighting with synthetic XF memory.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "lighting.h"

using namespace gcrecomp::gp;

namespace {

int g_failures = 0;

void check_near(const char* what, float got, float want, float eps = 1.5e-3f) {
    if (std::fabs(got - want) > eps) {
        std::printf("FAIL %s: got %g, want %g\n", what, got, want);
        ++g_failures;
    }
}

struct Xf {
    std::vector<uint32_t> mem = std::vector<uint32_t>(0x1100, 0);
    void setf(uint32_t addr, float f) { std::memcpy(&mem[addr], &f, 4); }
    static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t(r) << 24) | (g << 16) | (b << 8) | a; }
    void set_ctrl(uint32_t channel, uint32_t value) {
        mem[kXfColorCtrl0 + channel] = value;
        mem[kXfAlphaCtrl0 + channel] = value;
    }
    void light(uint32_t i, uint32_t color, const float pos[3], const float dir[3], const float a[3], const float k[3]) {
        const uint32_t b = kXfLights + i * 16;
        mem[b + 3] = color;
        for (int j = 0; j < 3; ++j) {
            setf(b + 4 + j, a[j]);
            setf(b + 7 + j, k[j]);
            setf(b + 10 + j, pos[j]);
            setf(b + 13 + j, dir[j]);
        }
    }
    // Identity normal matrix 0.
    void identity_normal() {
        setf(kXfNormalMatrices + 0, 1);
        setf(kXfNormalMatrices + 4, 1);
        setf(kXfNormalMatrices + 8, 1);
    }
};

// Control register fields.
constexpr uint32_t kMatVertex = 1, kLit = 2, kAmbVertex = 1 << 6;
constexpr uint32_t mask(uint32_t lights) { return ((lights & 0xF) << 2) | (((lights >> 4) & 0xF) << 11); }
constexpr uint32_t diffuse(uint32_t f) { return f << 7; }
constexpr uint32_t kAttnSpecBits = 1 << 9, kAttnSpotBits = (1 << 9) | (1 << 10), kAttnNoneBits = 1 << 10;

const float kOneA[3] = {1, 0, 0};  // a0=1: no angle falloff
const float kNoDist[3] = {1, 0, 0};

void test_unlit_passes_material() {
    Xf xf;
    xf.mem[kXfMaterial0] = Xf::rgba(51, 102, 153, 204);
    xf.set_ctrl(0, 0);  // register material, no lighting
    xf.set_ctrl(1, kMatVertex);
    const float vc[2][4] = {{1, 1, 1, 1}, {0.25f, 0.5f, 0.75f, 0.125f}};
    const float pos[3] = {0, 0, 0}, n[3] = {0, 0, 1};
    float out[2][4];
    light_vertex(xf.mem.data(), vc, pos, n, out);
    check_near("unlit reg r", out[0][0], 0.2f);
    check_near("unlit reg g", out[0][1], 0.4f);
    check_near("unlit reg b", out[0][2], 0.6f);
    check_near("unlit reg a", out[0][3], 0.8f);
    for (int i = 0; i < 4; ++i) check_near("unlit vertex", out[1][i], vc[1][i], 1e-6f);
}

void test_alpha_channel_has_own_source() {
    Xf xf;
    xf.mem[kXfMaterial0] = Xf::rgba(255, 0, 0, 255);
    xf.mem[kXfColorCtrl0] = kMatVertex;  // RGB from the vertex
    xf.mem[kXfAlphaCtrl0] = 0;           // alpha from the register (1.0)
    const float vc[2][4] = {{0, 1, 0, 0.5f}, {}};
    const float pos[3] = {}, n[3] = {0, 0, 1};
    float out[2][4];
    light_vertex(xf.mem.data(), vc, pos, n, out);
    check_near("rgb from vertex g", out[0][1], 1.0f);
    check_near("rgb from vertex r", out[0][0], 0.0f);
    check_near("alpha from register", out[0][3], 1.0f);
}

void test_ambient_only() {
    Xf xf;
    xf.mem[kXfMaterial0] = Xf::rgba(255, 128, 0, 255);
    xf.mem[kXfAmbient0] = Xf::rgba(128, 128, 128, 0);
    xf.set_ctrl(0, kLit);  // lighting on, no lights in the mask
    const float vc[2][4] = {};
    const float pos[3] = {}, n[3] = {0, 0, 1};
    float out[2][4];
    light_vertex(xf.mem.data(), vc, pos, n, out);
    check_near("ambient r", out[0][0], 1.0f * 128 / 255);
    check_near("ambient g", out[0][1], 128.0f / 255 * 128 / 255);
    check_near("ambient b", out[0][2], 0.0f);
    check_near("ambient alpha (ambient a = 0)", out[0][3], 0.0f);
}

void test_ambient_from_vertex() {
    Xf xf;
    xf.mem[kXfMaterial0] = Xf::rgba(255, 255, 255, 255);
    xf.mem[kXfAmbient0] = Xf::rgba(255, 255, 255, 255);  // would give 1.0 if used
    xf.set_ctrl(0, kLit | kAmbVertex);
    const float vc[2][4] = {{0.5f, 0.25f, 0, 1}, {}};
    const float pos[3] = {}, n[3] = {0, 0, 1};
    float out[2][4];
    light_vertex(xf.mem.data(), vc, pos, n, out);
    check_near("amb vertex r", out[0][0], 0.5f);
    check_near("amb vertex g", out[0][1], 0.25f);
}

void test_directional_diffuse() {
    // A directional light is a point light very far away; SDK convention: position = -dir * 2^20.
    Xf xf;
    xf.mem[kXfMaterial0] = Xf::rgba(255, 255, 255, 255);
    xf.set_ctrl(0, kLit | mask(1) | diffuse(2));  // clamp diffuse, no attenuation
    const float dir[3] = {0, 0, -1};              // light travels along -z, so it comes from +z
    const float pos[3] = {0, 0, 1048576.0f};
    xf.light(0, Xf::rgba(255, 128, 0, 255), pos, dir, kOneA, kNoDist);
    const float vc[2][4] = {};
    const float origin[3] = {};
    float out[2][4];

    const float facing[3] = {0, 0, 1};
    light_vertex(xf.mem.data(), vc, origin, facing, out);
    check_near("facing r", out[0][0], 1.0f);
    check_near("facing g", out[0][1], 128.0f / 255);
    check_near("facing b", out[0][2], 0.0f);

    const float tilted[3] = {0.6f, 0, 0.8f};
    light_vertex(xf.mem.data(), vc, origin, tilted, out);
    check_near("tilted r", out[0][0], 0.8f);

    const float away[3] = {0, 0, -1};
    light_vertex(xf.mem.data(), vc, origin, away, out);
    check_near("away (clamped) r", out[0][0], 0.0f);

    // Sign mode lets a back-facing normal produce a negative term; the sum is clamped at zero.
    xf.set_ctrl(0, kLit | mask(1) | diffuse(1));
    xf.mem[kXfAmbient0] = Xf::rgba(51, 51, 51, 255);
    light_vertex(xf.mem.data(), vc, origin, away, out);
    check_near("sign mode, back-facing darkens below ambient", out[0][0], 0.0f);
}

void test_light_mask_selects_lights() {
    Xf xf;
    xf.mem[kXfMaterial0] = Xf::rgba(255, 255, 255, 255);
    const float dir[3] = {0, 0, -1}, pos[3] = {0, 0, 1048576.0f};
    xf.light(0, Xf::rgba(51, 0, 0, 255), pos, dir, kOneA, kNoDist);
    xf.light(5, Xf::rgba(0, 102, 0, 255), pos, dir, kOneA, kNoDist);
    const float vc[2][4] = {};
    const float origin[3] = {}, n[3] = {0, 0, 1};
    float out[2][4];
    xf.set_ctrl(0, kLit | mask(0x21) | diffuse(2));  // lights 0 and 5 (the high mask bits)
    light_vertex(xf.mem.data(), vc, origin, n, out);
    check_near("light 0", out[0][0], 0.2f);
    check_near("light 5 via bits 11-14", out[0][1], 0.4f);
    xf.set_ctrl(0, kLit | mask(0x20) | diffuse(2));
    light_vertex(xf.mem.data(), vc, origin, n, out);
    check_near("light 0 masked out", out[0][0], 0.0f);
    check_near("light 5 only", out[0][1], 0.4f);
}

void test_point_light_distance_attenuation() {
    // Spot-type attenuation with a0=1 (no angle falloff) and k = (1, 0, 0.25): 1 / (1 + 0.25 d^2).
    Xf xf;
    xf.mem[kXfMaterial0] = Xf::rgba(255, 255, 255, 255);
    xf.set_ctrl(0, kLit | mask(1) | diffuse(2) | kAttnSpotBits);
    const float pos[3] = {0, 0, 2};  // 2 units above the vertex
    const float dir[3] = {0, 0, -1};
    const float k[3] = {1, 0, 0.25f};
    xf.light(0, Xf::rgba(255, 255, 255, 255), pos, dir, kOneA, k);
    const float vc[2][4] = {};
    const float origin[3] = {}, n[3] = {0, 0, 1};
    float out[2][4];
    light_vertex(xf.mem.data(), vc, origin, n, out);
    check_near("distance 2: 1/(1+1)", out[0][0], 0.5f);
    const float far_vertex[3] = {0, 0, -2};  // distance 4: 1/(1+4)
    light_vertex(xf.mem.data(), vc, far_vertex, n, out);
    check_near("distance 4: 1/(1+4)", out[0][0], 0.2f);
}

void test_spot_angle_attenuation() {
    // Angle attenuation a = (0, 0, 1): intensity = cos^2 of the angle between the stored light
    // direction and the direction from the vertex to the light.
    Xf xf;
    xf.mem[kXfMaterial0] = Xf::rgba(255, 255, 255, 255);
    xf.set_ctrl(0, kLit | mask(1) | diffuse(0) | kAttnSpotBits);  // diffuse off: attenuation only
    const float pos[3] = {0, 0, 1};
    const float a[3] = {0, 0, 1}, k[3] = {1, 0, 0};
    const float vc[2][4] = {};
    const float n[3] = {0, 0, 1};
    float out[2][4];
    const float on_axis[3] = {0, 0, 0}, off_axis[3] = {1, 0, 0};
    const float s = std::sqrt(0.5f);

    const float toward_light[3] = {0, 0, 1};  // vertex -> light from the origin
    xf.light(0, Xf::rgba(255, 255, 255, 255), pos, toward_light, a, k);
    light_vertex(xf.mem.data(), vc, on_axis, n, out);
    check_near("on axis", out[0][0], 1.0f);
    // From (1,0,0) the light is at direction (-s,0,s): 45 degrees from +z, cos^2 = 0.5.
    light_vertex(xf.mem.data(), vc, off_axis, n, out);
    check_near("45 degrees: cos^2 = 0.5", out[0][0], 0.5f);
    const float aligned[3] = {-s, 0, s};
    xf.light(0, Xf::rgba(255, 255, 255, 255), pos, aligned, a, k);
    light_vertex(xf.mem.data(), vc, off_axis, n, out);
    check_near("aligned: cos^2 = 1", out[0][0], 1.0f);
    const float opposite[3] = {0, 0, -1};  // cone points the other way: max(0, cos) = 0
    xf.light(0, Xf::rgba(255, 255, 255, 255), pos, opposite, a, k);
    light_vertex(xf.mem.data(), vc, on_axis, n, out);
    check_near("outside the cone", out[0][0], 0.0f);
}

void test_specular() {
    // Specular channel: attenuation = (a0 + a1 x + a2 x^2) / (k0 + k1 x + k2 x^2) with x = max(0, N.H),
    // zero when the light is behind the surface. Diffuse none -> k is used as given.
    Xf xf;
    xf.mem[kXfMaterial0 + 1] = Xf::rgba(255, 255, 255, 255);
    xf.set_ctrl(1, kLit | mask(1) | diffuse(0) | kAttnSpecBits);
    const float pos[3] = {0, 0, 1048576.0f};
    const float half[3] = {0, 0, 1};
    const float a[3] = {0, 0, 1}, k[3] = {1, 0, 0};  // intensity = x^2
    xf.light(0, Xf::rgba(255, 255, 255, 255), pos, half, a, k);
    const float vc[2][4] = {};
    const float origin[3] = {};
    float out[2][4];
    const float n1[3] = {0, 0, 1};
    light_vertex(xf.mem.data(), vc, origin, n1, out);
    check_near("specular peak", out[1][0], 1.0f);
    const float n2[3] = {0.6f, 0, 0.8f};
    light_vertex(xf.mem.data(), vc, origin, n2, out);
    check_near("specular falloff 0.8^2", out[1][0], 0.64f);
    const float n3[3] = {0, 0, -1};
    light_vertex(xf.mem.data(), vc, origin, n3, out);
    check_near("specular back-facing", out[1][0], 0.0f);
}

void test_channel_isolation() {
    // Channel 1 is lit, channel 0 is not: the lights must not leak into channel 0.
    Xf xf;
    xf.mem[kXfMaterial0] = Xf::rgba(255, 0, 0, 255);
    xf.mem[kXfMaterial0 + 1] = Xf::rgba(255, 255, 255, 255);
    xf.set_ctrl(0, 0);
    xf.set_ctrl(1, kLit | mask(1) | diffuse(2));
    const float pos[3] = {0, 0, 1048576.0f}, dir[3] = {0, 0, -1};
    xf.light(0, Xf::rgba(0, 255, 0, 255), pos, dir, kOneA, kNoDist);
    const float vc[2][4] = {};
    const float origin[3] = {}, n[3] = {0, 0, 1};
    float out[2][4];
    light_vertex(xf.mem.data(), vc, origin, n, out);
    check_near("ch0 unlit r", out[0][0], 1.0f);
    check_near("ch0 unlit g", out[0][1], 0.0f);
    check_near("ch1 lit g", out[1][1], 1.0f);
    check_near("ch1 lit r", out[1][0], 0.0f);
}

void test_normal_transform() {
    Xf xf;
    // Matrix 1 (index 3 = SDK id for the second matrix): rotate 90 degrees about z, scaled by 2.
    const uint32_t b = kXfNormalMatrices + 3 * 3;
    xf.setf(b + 0, 0); xf.setf(b + 1, -2); xf.setf(b + 2, 0);
    xf.setf(b + 3, 2); xf.setf(b + 4, 0);  xf.setf(b + 5, 0);
    xf.setf(b + 6, 0); xf.setf(b + 7, 0);  xf.setf(b + 8, 2);
    const float n[3] = {1, 0, 0};
    float out[3];
    transform_normal(xf.mem.data(), 3, n, out);
    check_near("rotated x", out[0], 0.0f);
    check_near("rotated y", out[1], 1.0f);
    check_near("rotated z", out[2], 0.0f);
}

}  // namespace

int main() {
    test_unlit_passes_material();
    test_alpha_channel_has_own_source();
    test_ambient_only();
    test_ambient_from_vertex();
    test_directional_diffuse();
    test_light_mask_selects_lights();
    test_point_light_distance_attenuation();
    test_spot_angle_attenuation();
    test_specular();
    test_channel_isolation();
    test_normal_transform();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all lighting tests passed\n");
    return 0;
}
