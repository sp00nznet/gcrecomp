// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// GX Core - Main GX state machine
//
// Tracks all GX state and dispatches to the D3D11 backend. This is the
// central implementation of the GX API that recompiled game code calls.
// Each function mirrors a Nintendo GX SDK function and updates the
// internal GXState structure accordingly.
//
// References:
//   - libogc (devkitPro) — GX API function signatures and semantics
//   - Pureikyubu — Hardware register behavior and default values
// =============================================================================

#include "gcrecomp/gx/gx.h"
#include <cstdio>
#include <cstring>

namespace gcrecomp::gx {

static GXState g_state;

/// Reset GX state to hardware power-on defaults.
/// Reference: Pureikyubu GX register default values after GXInit.
void GXState::reset() {
    memset(this, 0, sizeof(*this));
    z_enable = true;
    z_write = true;
    z_func = GX_LEQUAL;
    cull_mode = GX_CULL_BACK;
    blend_mode = GX_BM_NONE;
    src_factor = GX_BL_ONE;
    dst_factor = GX_BL_ZERO;
    viewport_w = 640;
    viewport_h = 480;
    viewport_near = 0.0f;
    viewport_far = 1.0f;
    num_tev_stages = 1;
    num_tex_gens = 0;
    num_color_chans = 0;
    num_ind_stages = 0;
    alpha_func0 = GX_ALWAYS;
    alpha_func1 = GX_ALWAYS;
    alpha_op = 0;
    alpha_ref0 = 0;
    alpha_ref1 = 0;
    fog_type = 0; // FOG_NONE

    // Default konst colors to white (matches hardware default)
    for (int i = 0; i < 4; i++) {
        konst[i][0] = konst[i][1] = konst[i][2] = konst[i][3] = 1.0f;
    }

    // Default material colors to white, ambient to black
    for (int i = 0; i < 2; i++) {
        mat_color[i][0] = mat_color[i][1] = mat_color[i][2] = mat_color[i][3] = 1.0f;
        // amb_color already zeroed by memset
    }

    // Framebuffer writes enabled by default
    color_update = true;
    alpha_update = true;
    dither = true;
}

/// Initialize the GX graphics subsystem.
/// Resets all state to defaults and prepares for rendering.
void GXInit() {
    printf("[GX] Initializing GameCube graphics subsystem\n");
    g_state.reset();
}

/// Set the viewport transformation.
/// Maps normalized device coordinates to screen pixel coordinates.
/// The GameCube viewport origin is at the top-left corner.
void GXSetViewport(float x, float y, float w, float h, float near_z, float far_z) {
    g_state.viewport_x = x;
    g_state.viewport_y = y;
    g_state.viewport_w = w;
    g_state.viewport_h = h;
    g_state.viewport_near = near_z;
    g_state.viewport_far = far_z;
}

/// Set the scissor rectangle for pixel-level clipping.
/// Pixels outside this rectangle are discarded.
void GXSetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    g_state.scissor_x = x;
    g_state.scissor_y = y;
    g_state.scissor_w = w;
    g_state.scissor_h = h;
}

/// Set the face culling mode.
/// Controls which polygon faces are discarded during rasterization.
void GXSetCullMode(GXCullMode mode) {
    g_state.cull_mode = mode;
}

/// Configure framebuffer blending.
/// Determines how the pixel shader output is combined with the existing
/// framebuffer contents. Logic op is only used when type == GX_BM_LOGIC.
void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, uint32_t logic_op) {
    g_state.blend_mode = type;
    g_state.src_factor = src;
    g_state.dst_factor = dst;
}

/// Configure the depth (Z-buffer) test.
/// @param enable Whether depth testing is active
/// @param func Comparison function (e.g., GX_LEQUAL)
/// @param write_enable Whether passing fragments update the depth buffer
void GXSetZMode(bool enable, GXCompare func, bool write_enable) {
    g_state.z_enable = enable;
    g_state.z_func = func;
    g_state.z_write = write_enable;
}

/// Set how many TEV stages are active for rendering.
/// Only stages 0 through (count-1) will be executed.
void GXSetNumTevStages(uint32_t count) {
    g_state.num_tev_stages = count;
}

/// Assign resources to a TEV stage.
/// @param stage TEV stage index
/// @param coord Texture coordinate generator index (0-7 or GX_TEXCOORDNULL)
/// @param map Texture map index (0-7 or GX_TEXMAP_NULL)
/// @param color Rasterized color channel (0=COLOR0, 1=COLOR1, 0xFF=none)
void GXSetTevOrder(GXTevStageID stage, uint32_t coord, uint32_t map, uint32_t color) {
    if (stage >= GX_MAX_TEVSTAGE) return;
    g_state.tev_stages[stage].tex_coord_id = coord;
    g_state.tev_stages[stage].tex_map_id = map;
    g_state.tev_stages[stage].channel_id = color;
}

/// Set the 4 color combiner inputs (A, B, C, D) for a TEV stage.
/// The combiner computes: result = D op ((1-C)*A + C*B).
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    if (stage >= GX_MAX_TEVSTAGE) return;
    g_state.tev_stages[stage].color_a = a;
    g_state.tev_stages[stage].color_b = b;
    g_state.tev_stages[stage].color_c = c;
    g_state.tev_stages[stage].color_d = d;
}

/// Set the color combiner operation parameters for a TEV stage.
/// Controls the math operation, bias, scale, clamp, and output register.
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp, uint32_t reg) {
    if (stage >= GX_MAX_TEVSTAGE) return;
    g_state.tev_stages[stage].color_op = op;
    g_state.tev_stages[stage].color_bias = bias;
    g_state.tev_stages[stage].color_scale = scale;
    g_state.tev_stages[stage].color_clamp = clamp;
    g_state.tev_stages[stage].color_reg_id = reg;
}

/// Set the 4 alpha combiner inputs (A, B, C, D) for a TEV stage.
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    if (stage >= GX_MAX_TEVSTAGE) return;
    g_state.tev_stages[stage].alpha_a = a;
    g_state.tev_stages[stage].alpha_b = b;
    g_state.tev_stages[stage].alpha_c = c;
    g_state.tev_stages[stage].alpha_d = d;
}

/// Set the alpha combiner operation parameters for a TEV stage.
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp, uint32_t reg) {
    if (stage >= GX_MAX_TEVSTAGE) return;
    g_state.tev_stages[stage].alpha_op = op;
    g_state.tev_stages[stage].alpha_bias = bias;
    g_state.tev_stages[stage].alpha_scale = scale;
    g_state.tev_stages[stage].alpha_clamp = clamp;
    g_state.tev_stages[stage].alpha_reg_id = reg;
}

/// Begin submitting vertices for a primitive.
/// After calling GXBegin, the game code writes vertex attributes into the
/// FIFO until num_verts vertices have been submitted, then calls GXEnd.
/// Forwards to the vertex assembly module to initialize the vertex buffer.
/// Reference: libogc GXBegin.
void GXBegin(GXPrimitive prim, uint32_t vtx_fmt, uint32_t num_verts) {
    vtx_begin(prim, vtx_fmt, num_verts);
}

/// End vertex submission and flush the current primitive batch to the GPU.
/// Forwards to the vertex assembly module to finalize the vertex buffer,
/// then the D3D11 backend can consume it for drawing.
/// Reference: libogc GXEnd.
void GXEnd() {
    vtx_end();

    // Dispatch the assembled vertex buffer to the D3D11 backend
    const float* vb_data = get_vertex_buffer_data();
    uint32_t vb_count = get_vertex_buffer_count();
    if (vb_data && vb_count > 0) {
        GXDrawPrimitive((uint32_t)get_current_primitive(), vb_data, vb_count, 30);
    }
}

/// Set the vertex attribute format for a given format index.
/// Defines how vertex data is interpreted (component type, count, and
/// fixed-point fraction bits). Forwards to the vertex assembly module.
/// Reference: libogc GXSetVtxAttrFmt.
void GXSetVtxAttrFmt(uint32_t fmt, GXAttr attr, uint32_t comp_type, uint32_t comp_size, uint32_t frac) {
    vtx_set_attr_fmt(fmt, attr, comp_type, comp_size, frac);
}

/// Clear all vertex attribute descriptors, setting all to GX_NONE.
/// Typically called before reconfiguring the vertex format.
void GXClearVtxDesc() {
    for (int i = 0; i < GX_VA_MAX_ATTR; i++) {
        g_state.vtx_attr_type[i] = GX_NONE;
    }
}

/// Set a vertex attribute's data source type.
/// Determines whether the attribute is absent, inline, or indexed.
void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    if (attr < GX_VA_MAX_ATTR) {
        g_state.vtx_attr_type[attr] = type;
    }
}

/// Return a const reference to the current GX state.
/// Used by the D3D11 backend to read TEV configuration, blend modes,
/// depth settings, and other pipeline state when issuing draw calls.
const GXState& GXGetState() {
    return g_state;
}

/// Copy the Embedded Frame Buffer to an External Frame Buffer.
/// On real hardware this triggers a copy from EFB to XFB in main memory.
/// In our implementation this maps to presenting the D3D11 render target.
/// Reference: libogc GXCopyDisp; Pureikyubu EFB-to-XFB copy documentation.
void GXCopyDisp(void* dest, bool clear) {
    GXPresent();
}

/// Set the clear color and depth for the next GXCopyDisp with clear=true.
void GXSetCopyClear(uint32_t color, uint32_t z) {
    // Set clear color/depth for GXCopyDisp
}

/// Wait for the GPU to finish all pending work.
/// In our synchronous implementation this is a no-op since all rendering
/// is executed immediately.
void GXDrawDone() {
    // Wait for GPU to finish (no-op for us, we're synchronous)
}

/// Flush the GX command buffer to the GPU.
/// No-op in our implementation since commands are executed immediately.
void GXFlush() {
    // Flush command buffer (no-op, we execute immediately)
}

// ---- Konst Color Registers ----

/// Set a TEV konstant color register.
/// These 4 registers provide additional color constants that can be
/// selected per-stage via GXSetTevKColorSel/GXSetTevKAlphaSel.
/// The color is packed as RGBA8 with R in bits 31-24.
void GXSetTevKColor(uint32_t reg, uint32_t color) {
    if (reg >= 4) return;
    g_state.konst[reg][0] = ((color >> 24) & 0xFF) / 255.0f; // R
    g_state.konst[reg][1] = ((color >> 16) & 0xFF) / 255.0f; // G
    g_state.konst[reg][2] = ((color >> 8)  & 0xFF) / 255.0f; // B
    g_state.konst[reg][3] = ((color >> 0)  & 0xFF) / 255.0f; // A
}

/// Select the konstant color swizzle for a TEV stage's GX_CC_KONST input.
/// See Pureikyubu documentation for the full swizzle selection table.
void GXSetTevKColorSel(GXTevStageID stage, uint8_t sel) {
    if (stage >= GX_MAX_TEVSTAGE) return;
    g_state.kcolor_sel[stage] = sel;
}

/// Select the konstant alpha swizzle for a TEV stage's GX_CA_KONST input.
void GXSetTevKAlphaSel(GXTevStageID stage, uint8_t sel) {
    if (stage >= GX_MAX_TEVSTAGE) return;
    g_state.kalpha_sel[stage] = sel;
}

// ---- TEV Color Registers ----

/// Set a TEV color register value.
/// Register 0 is PREV (the implicit output), 1-3 are C0/C1/C2.
/// These registers persist across TEV stages and can be read/written
/// by any stage.
void GXSetTevColor(uint32_t reg, uint32_t color) {
    if (reg >= 4) return;
    g_state.tev_reg[reg][0] = ((color >> 24) & 0xFF) / 255.0f;
    g_state.tev_reg[reg][1] = ((color >> 16) & 0xFF) / 255.0f;
    g_state.tev_reg[reg][2] = ((color >> 8)  & 0xFF) / 255.0f;
    g_state.tev_reg[reg][3] = ((color >> 0)  & 0xFF) / 255.0f;
}

// ---- Indirect Texture ----

/// Set the number of active indirect texture stages (0-4).
/// Indirect texture stages sample a texture to produce coordinate offsets
/// for regular TEV stages.
void GXSetNumIndStages(uint32_t count) {
    g_state.num_ind_stages = count;
}

/// Configure an indirect texture stage's texture coordinate and map sources.
void GXSetIndTexOrder(uint32_t stage, uint32_t coord, uint32_t map) {
    if (stage >= 4) return;
    g_state.ind_stages[stage].tex_coord = coord;
    g_state.ind_stages[stage].tex_map = map;
}

/// Configure how a TEV stage applies indirect texture offsets.
/// The indirect offset is fetched from the specified indirect stage,
/// formatted, biased, matrix-transformed, and then added to the TEV
/// stage's texture coordinates before sampling.
void GXSetTevIndirect(GXTevStageID stage, uint32_t ind_stage, uint32_t fmt,
                       uint32_t bias, uint32_t matrix, uint32_t wrap_s,
                       uint32_t wrap_t, bool add_prev, bool utc_lod) {
    if (stage >= GX_MAX_TEVSTAGE) return;
    g_state.ind_tev[stage].ind_stage = (uint8_t)ind_stage;
    g_state.ind_tev[stage].format    = (uint8_t)fmt;
    g_state.ind_tev[stage].bias      = (uint8_t)bias;
    g_state.ind_tev[stage].matrix    = (uint8_t)matrix;
    g_state.ind_tev[stage].wrap_s    = (uint8_t)wrap_s;
    g_state.ind_tev[stage].wrap_t    = (uint8_t)wrap_t;
    g_state.ind_tev[stage].add_prev  = add_prev;
    g_state.ind_tev[stage].utc_lod   = utc_lod;
}

// ---- Alpha Compare ----

/// Configure the alpha compare test (pixel kill before blending).
/// Two comparison functions are combined with a logic operation:
///   pass = func0(alpha, ref0) OP func1(alpha, ref1)
/// where OP is AND (0), OR (1), XOR (2), or XNOR (3).
void GXSetAlphaCompare(GXCompare func0, uint8_t ref0, uint8_t op, GXCompare func1, uint8_t ref1) {
    g_state.alpha_func0 = func0;
    g_state.alpha_ref0  = ref0;
    g_state.alpha_op    = op;
    g_state.alpha_func1 = func1;
    g_state.alpha_ref1  = ref1;
}

// ---- Fog ----

/// Configure fog parameters.
/// Fog is applied after TEV stages and alpha test, blending the pixel
/// color toward the fog color based on depth.
/// Type 0 = none, 2 = linear, 4 = exponential, 5 = exponential squared.
void GXSetFog(uint8_t type, float start, float end, float near_z, float far_z, uint32_t color) {
    g_state.fog_type  = type;
    g_state.fog_start = start;
    g_state.fog_end   = end;
    g_state.fog_near  = near_z;
    g_state.fog_far   = far_z;
    g_state.fog_color[0] = ((color >> 24) & 0xFF) / 255.0f;
    g_state.fog_color[1] = ((color >> 16) & 0xFF) / 255.0f;
    g_state.fog_color[2] = ((color >> 8)  & 0xFF) / 255.0f;
    g_state.fog_color[3] = ((color >> 0)  & 0xFF) / 255.0f;
}

// ---- Tex Gen / Color Channels ----

/// Set the number of active texture coordinate generators.
/// Tex coord generators produce texture coordinates from vertex data
/// using configurable matrix transforms.
void GXSetNumTexGens(uint32_t count) {
    g_state.num_tex_gens = count;
}

/// Set the number of active color channels.
/// Color channels apply lighting computations to produce rasterized colors.
void GXSetNumChans(uint32_t count) {
    g_state.num_color_chans = count;
}

// ---- Texture Coordinate Generation ----

/// Configure a texture coordinate generator.
/// @param coord Tex coord index (GX_TEXCOORD0..7)
/// @param type Generation type: 0=MTX3x4, 1=MTX2x4, 2=BUMP, 10=SRTG
/// @param src Source parameter: 0=POS, 1=NRM, 4+=TEX0-7
/// @param mtx XF matrix index (30,33,...,57 or 0x3C=identity)
/// Reference: libogc GXSetTexCoordGen; Pureikyubu XF documentation.
void GXSetTexCoordGen(uint32_t coord, uint32_t type, uint32_t src, uint32_t mtx) {
    if (coord >= 8) return;
    g_state.tex_gens[coord].type      = type;
    g_state.tex_gens[coord].src_param = src;
    g_state.tex_gens[coord].mtx       = mtx;
}

// ---- Color Channel Control ----

/// Configure a color channel's lighting parameters.
/// @param chan Channel index: 0=COLOR0, 1=ALPHA0, 2=COLOR1, 3=ALPHA1
/// @param enable Enable lighting for this channel
/// @param amb_src Ambient source: 0=register, 1=vertex color
/// @param mat_src Material source: 0=register, 1=vertex color
/// @param light_mask Bitmask of active lights (bits 0-7)
/// @param diff_fn Diffuse function: 0=none, 1=signed, 2=clamped
/// @param attn_fn Attenuation function: 0=specular, 1=spotlight, 2=none
/// Reference: libogc GXSetChanCtrl; Pureikyubu XF lighting registers.
void GXSetChanCtrl(uint32_t chan, bool enable, uint32_t amb_src, uint32_t mat_src,
                   uint32_t light_mask, uint32_t diff_fn, uint32_t attn_fn) {
    if (chan >= 4) return;
    g_state.chan_ctrl[chan].enable     = enable;
    g_state.chan_ctrl[chan].amb_src    = amb_src;
    g_state.chan_ctrl[chan].mat_src    = mat_src;
    g_state.chan_ctrl[chan].light_mask = light_mask;
    g_state.chan_ctrl[chan].diff_fn    = diff_fn;
    g_state.chan_ctrl[chan].attn_fn    = attn_fn;
}

/// Set the material color register for a color channel.
/// @param chan Channel index (0 or 1)
/// @param color Packed RGBA8 color
void GXSetChanMatColor(uint32_t chan, uint32_t color) {
    if (chan >= 2) return;
    g_state.mat_color[chan][0] = ((color >> 24) & 0xFF) / 255.0f;
    g_state.mat_color[chan][1] = ((color >> 16) & 0xFF) / 255.0f;
    g_state.mat_color[chan][2] = ((color >>  8) & 0xFF) / 255.0f;
    g_state.mat_color[chan][3] = ((color >>  0) & 0xFF) / 255.0f;
}

/// Set the ambient color register for a color channel.
/// @param chan Channel index (0 or 1)
/// @param color Packed RGBA8 color
void GXSetChanAmbColor(uint32_t chan, uint32_t color) {
    if (chan >= 2) return;
    g_state.amb_color[chan][0] = ((color >> 24) & 0xFF) / 255.0f;
    g_state.amb_color[chan][1] = ((color >> 16) & 0xFF) / 255.0f;
    g_state.amb_color[chan][2] = ((color >>  8) & 0xFF) / 255.0f;
    g_state.amb_color[chan][3] = ((color >>  0) & 0xFF) / 255.0f;
}

// ---- Vertex Arrays ----

/// Set a vertex array base pointer and stride for indexed vertex access.
/// @param attr Attribute type (GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, etc.)
/// @param base Base address of the array data
/// @param stride Stride in bytes between elements
/// Reference: libogc GXSetArray; Pureikyubu CP register documentation.
void GXSetArray(GXAttr attr, const void* base, uint32_t stride) {
    uint32_t idx = static_cast<uint32_t>(attr);
    if (idx >= GX_VA_MAX_ATTR) return;
    g_state.vtx_arrays[idx].base   = static_cast<const uint8_t*>(base);
    g_state.vtx_arrays[idx].stride = stride;
}

// ---- Framebuffer Write Masks ----

/// Enable or disable color writes to the framebuffer.
void GXSetColorUpdate(bool enable) {
    g_state.color_update = enable;
}

/// Enable or disable alpha writes to the framebuffer.
void GXSetAlphaUpdate(bool enable) {
    g_state.alpha_update = enable;
}

/// Enable or disable dithering.
void GXSetDither(bool enable) {
    g_state.dither = enable;
}

} // namespace gcrecomp::gx
