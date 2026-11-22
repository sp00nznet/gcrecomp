// SPDX-License-Identifier: MIT
// Renders a few full-screen triangles through the real OpenGL path and checks the EFB pixels. Needs a GL
// 3.3 context but no window system: run it as
//   SDL_VIDEO_DRIVER=offscreen ./gp_render_check
// (it is not part of ctest because CI machines may have no EGL). Covers the blend-state features that
// are only visible on screen: logic ops, constant alpha, fog.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gl_loader.h"
#include "gp_internal.h"

using namespace gcrecomp::gp;

static State s;
static float verts[3 * kVertexFloats];
static int g_failures = 0;

static void set_xf_float(uint32_t addr, float f) { std::memcpy(&s.xf[addr], &f, 4); }

static void set_color(float r, float g, float b, float a) {
    for (int i = 0; i < 3; ++i) {
        float* c = &verts[i * kVertexFloats + 4];
        c[0] = r; c[1] = g; c[2] = b; c[3] = a;
    }
}

static void set_depth(float clip_z) {
    for (int i = 0; i < 3; ++i) verts[i * kVertexFloats + 2] = clip_z;
}

static void clear() {
    s.bp[0x4F] = 0; s.bp[0x50] = 0; s.bp[0x51] = 0xFFFFFF;
    CopyRequest r{};
    r.clear = true;
    renderer::copy_efb(r, s);
}

static void draw() {
    const DrawCall call{kTriangles, 3, verts};
    renderer::draw(call, s);
}

static void expect(const char* what, int r, int g, int b, int a) {
    unsigned char p[4];
    glReadPixels(320, 264, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    const bool ok = std::abs(p[0] - r) <= 1 && std::abs(p[1] - g) <= 1 && std::abs(p[2] - b) <= 1 && std::abs(p[3] - a) <= 1 &&
                    glGetError() == GL_NO_ERROR;
    std::printf("%s %-34s got %3d %3d %3d %3d, expected %3d %3d %3d %3d\n", ok ? "ok  " : "FAIL", what, p[0], p[1], p[2], p[3], r, g, b, a);
    if (!ok) ++g_failures;
}

int main() {
    if (!renderer::init(640, 528, nullptr, 0)) {
        std::fprintf(stderr, "cannot create a GL context (try SDL_VIDEO_DRIVER=offscreen)\n");
        return 2;
    }
    const float pos[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};  // covers the whole viewport, depth 0.5
    for (int i = 0; i < 3; ++i) std::memcpy(&verts[i * kVertexFloats], pos[i], sizeof(pos[i]));
    set_xf_float(0x101A, 320); set_xf_float(0x101B, -240); set_xf_float(0x101D, 320); set_xf_float(0x101E, 288);
    s.bp[0x20] = (342 << 12) | 342;
    s.bp[0x21] = ((342 + 639) << 12) | (342 + 527);
    s.bp[0xC0] = 10;       // TEV color: d = rasterized color
    s.bp[0xC1] = 5 << 4;   // TEV alpha: d = rasterized alpha
    s.bp[0xF6] = 4; s.bp[0xF7] = 14; s.bp[0xF8] = 4; s.bp[0xF9] = 14;  // identity swap tables
    s.bp[0xF3] = (7 << 16) | (7 << 19);                                  // alpha test: always
    const uint32_t kUpdate = 8 | 0x10;                                   // color and alpha update
    const uint32_t kCopy = 3u << 12;

    s.bp[0x41] = kUpdate | kCopy;
    set_color(0.2f, 0.4f, 0.6f, 1.0f);
    draw();
    expect("plain write", 51, 102, 153, 255);

    s.bp[0x41] = kUpdate | 2 | (6u << 12);  // logic op XOR with white inverts every channel
    set_color(1, 1, 1, 1);
    draw();
    expect("logic op xor", 204, 153, 102, 0);

    s.bp[0x41] = kUpdate | kCopy;
    set_color(0, 0, 0, 1);
    draw();
    s.bp[0x41] = kUpdate | 1 | (5u << 5) | (4u << 8);  // blend: src alpha, inverse src alpha
    set_color(1, 0, 0, 0.5f);
    draw();
    expect("blend, TEV alpha .5", 128, 0, 0, 191);

    s.bp[0x42] = 0x100 | 0x40;  // constant alpha 0x40: written alpha changes, blend factors do not
    draw();
    expect("blend, constant alpha 0x40", 191, 0, 0, 64);

    s.bp[0x41] = kUpdate | kCopy;
    s.bp[0x42] = 0x100 | 0x20;
    set_color(0.3f, 0.3f, 0.3f, 0.9f);
    draw();
    expect("no blend, constant alpha 0x20", 77, 77, 77, 32);

    s.bp[0x42] = 0;
    s.bp[0xEE] = 0x3F800;                 // A = 1.0
    s.bp[0xF1] = (2u << 21) | (1u << 20); // linear, orthographic, C = 0
    s.bp[0xF2] = 0x00FF00;                // green
    set_color(1, 0, 0, 1);
    draw();
    expect("orthographic linear fog at z=.5", 128, 128, 0, 255);

    // Z compare location: an alpha-rejected pixel updates depth only with early Z (BP 0x43 bit 6).
    // A is rejected by the alpha test at depth .5; B, green and behind A at depth .75, must then fail the
    // depth test with early Z and pass it with late Z.
    s.bp[0x41] = kUpdate | kCopy;
    s.bp[0x40] = 1 | (1 << 1) | 0x10;           // depth test LESS, depth update on
    s.bp[0xF3] = (4 << 16) | (7 << 19) | 128;   // alpha > 128
    for (int early = 1; early >= 0; --early) {
        clear();
        s.bp[0x43] = early ? 0x40 : 0;
        set_depth(0.0f);
        set_color(1, 1, 1, 0.0f);
        draw();
        set_depth(0.5f);
        set_color(0, 1, 0, 1.0f);
        draw();
        if (early) expect("early Z: behind a rejected pixel", 0, 0, 0, 0);
        else expect("late Z: behind a rejected pixel", 0, 255, 0, 255);
    }

    renderer::shutdown();
    return g_failures ? 1 : 0;
}
