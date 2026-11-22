// SPDX-License-Identifier: MIT
//
// GX vertex lighting: the XF color and alpha channels. Self-contained (no GP state, no display) so
// it can be unit-tested with synthetic XF memory.
#pragma once

#include <cstdint>

namespace gcrecomp::gp {

// XF register addresses used by the lighting unit.
constexpr uint32_t kXfNormalMatrices = 0x0400;  // 3 words per matrix index, 3x3 row-major
constexpr uint32_t kXfLights = 0x0600;          // 8 lights of 16 words
constexpr uint32_t kXfAmbient0 = 0x100A;        // + channel: ambient color, RGBA8
constexpr uint32_t kXfMaterial0 = 0x100C;       // + channel: material color, RGBA8
constexpr uint32_t kXfColorCtrl0 = 0x100E;      // + channel: color (RGB) channel control
constexpr uint32_t kXfAlphaCtrl0 = 0x1010;      // + channel: alpha channel control

// Light colors for the two channels of one vertex.
//   xf            XF memory (at least 0x1100 words; floats are stored as their bit patterns)
//   vertex_color  per-channel vertex color (RGBA 0..1), used where the control selects the vertex
//                 as source for material or ambient
//   view_pos      vertex position after the position matrix (view space; light positions are too)
//   normal        unit normal in view space (see transform_normal)
//   out           resulting RGBA per channel
// With lighting disabled in a control register the material source passes through unchanged.
void light_vertex(const uint32_t* xf, const float vertex_color[2][4], const float view_pos[3], const float normal[3],
                  float out[2][4]);

// Transforms a model-space normal by the 3x3 normal matrix `index` (the vertex's position matrix
// index) and normalizes it.
void transform_normal(const uint32_t* xf, uint32_t index, const float model_normal[3], float out[3]);

}  // namespace gcrecomp::gp
