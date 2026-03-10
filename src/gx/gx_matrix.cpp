// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// GX Matrix Operations
//
// The GameCube has a 64-entry matrix memory (XF registers) for vertex
// transforms. Each entry stores a 3x4 matrix (12 floats). Games use
// these for model-view transforms, skeletal animation, texture
// coordinate generation, and more.
//
// References:
//   - libogc — GXLoadPosMtxImm, GXSetCurrentMtx, GXSetProjection signatures
//   - Pureikyubu — XF (Transform Unit) register documentation
// =============================================================================

#include "gcrecomp/gx/gx.h"
#include <cstring>
#include <cmath>

namespace gcrecomp::gx {

// GX matrix memory: 64 entries of 4x3 matrices (256 floats)
static float g_matrix_mem[256];

// Position matrix (model-view)
static float g_pos_matrix[4][4];

// Projection matrix
static float g_proj_matrix[4][4];

void GXLoadPosMtxImm(const float mtx[3][4], uint32_t id) {
    memcpy(&g_matrix_mem[id * 4], mtx, 12 * sizeof(float));
}

void GXSetCurrentMtx(uint32_t id) {
    // Set which matrix in memory is the current position matrix
    float* src = &g_matrix_mem[id * 4];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            g_pos_matrix[r][c] = src[r * 4 + c];
        }
    }
    g_pos_matrix[3][0] = 0; g_pos_matrix[3][1] = 0;
    g_pos_matrix[3][2] = 0; g_pos_matrix[3][3] = 1;
}

void GXSetProjection(const float mtx[4][4], uint32_t type) {
    memcpy(g_proj_matrix, mtx, 16 * sizeof(float));
}

/// Retrieve the current model-view and projection matrices.
/// The model-view matrix is derived from GXLoadPosMtxImm/GXSetCurrentMtx.
/// The projection matrix is set by GXSetProjection.
/// Both are needed by the vertex shader constant buffer for vertex
/// transformation.
/// Reference: Pureikyubu XF (Transform Unit) register documentation.
void GXGetMatrices(float model_view[4][4], float projection[4][4]) {
    memcpy(model_view, g_pos_matrix, 16 * sizeof(float));
    memcpy(projection, g_proj_matrix, 16 * sizeof(float));
}

} // namespace gcrecomp::gx
