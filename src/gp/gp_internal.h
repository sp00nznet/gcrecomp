// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

namespace gcrecomp::gp {

// Register state of the three units that matter for drawing.
struct State {
    // CP (command processor): vertex descriptor and attribute tables, array pointers.
    uint32_t vcd_lo = 0, vcd_hi = 0;
    uint32_t vat[3][8] = {};
    uint32_t array_base[16] = {};
    uint32_t array_stride[16] = {};
    uint32_t matidx_a = 0, matidx_b = 0;

    // XF (transform unit): matrix memory at 0x000-0x0FF (as 3x4 float matrices), registers at 0x1000.
    uint32_t xf[0x1100] = {};


    // TEV color registers (PREV, C0, C1, C2) and constant colors (K0-K3), RGBA as floats. Both are
    // written through BP 0xE0-0xE7; bit 23 of the data selects the bank.
    float tev_reg[4][4] = {};
    float tev_konst[4][4] = {};
    // BP (raster, TEV, texture, blend, copy): 256 registers of 24 bits.
    uint32_t bp[256] = {};

    float xf_float(uint32_t addr) const;
};

enum Primitive : uint32_t { kQuads = 0, kTriangles = 2, kTriangleStrip = 3, kTriangleFan = 4, kLines = 5, kLineStrip = 6, kPoints = 7 };

// Floats per vertex handed to the renderer: clip position (4), two colors (4+4), eight texcoords (3 each).
constexpr int kVertexFloats = 4 + 4 + 4 + 8 * 3;

struct DrawCall {
    Primitive primitive;
    uint32_t vertex_count;
    const float* vertices;  // vertex_count * kVertexFloats
};

struct CopyRequest {
    bool to_xfb;  // copy to the external frame buffer (a frame is complete)
    bool clear;   // clear the EFB afterwards
    int x, y, w, h;
};

// Implemented by gl_renderer.cpp.
namespace renderer {
bool init(int window_w, int window_h, const char* screenshot_dir, int max_screenshots);
void shutdown();
void draw(const DrawCall& call, const State& state);
void copy_efb(const CopyRequest& request, const State& state);
bool pump_events();
}  // namespace renderer

}  // namespace gcrecomp::gp
