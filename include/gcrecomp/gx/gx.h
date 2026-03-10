#pragma once
// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// GX Graphics API Header
//
// Implements the GameCube's GX graphics API on top of Direct3D 11.
// The TEV (Texture Environment) unit is the heart of GameCube rendering:
//   up to 16 stages, each computing: result = D + ((1-C)*A + C*B)
//
// This is a game-agnostic reimplementation of the GX API for use with any
// statically recompiled GameCube title.
//
// References:
//   - libogc (devkitPro) — GX API function signatures and enum values
//     https://github.com/devkitPro/libogc
//   - Pureikyubu (formerly Dolwin) — Hardware behavior and register documentation
//     https://github.com/ogamespec/pureikyubu
//   - GameCubeRecompiled — TEV shader generation approach
//   - US Patent 6,664,962 (Nintendo) — TEV pipeline description
// =============================================================================

#include <cstdint>
#include <string>

namespace gcrecomp::gx {

// =============================================================================
// GX Enums (matching Nintendo SDK values)
//
// These enum values are defined to match the official Nintendo GameCube SDK
// (and libogc) so that recompiled game code can pass them directly without
// translation. The numeric values are part of the hardware interface and
// must not be changed.
// =============================================================================

/// Vertex attribute indices.
/// Each attribute has a fixed index in the GX vertex descriptor (VCD) and
/// vertex attribute table (VAT). Values match libogc GX_VA_* constants.
/// Reference: libogc gx.h, Pureikyubu CP register documentation.
enum GXAttr : uint32_t {
    GX_VA_PNMTXIDX  = 0,   ///< Position/Normal matrix index
    GX_VA_TEX0MTXIDX = 1,   ///< Texture coordinate 0 matrix index
    GX_VA_POS       = 9,    ///< Position (x, y, z)
    GX_VA_NRM       = 10,   ///< Normal vector (nx, ny, nz)
    GX_VA_CLR0      = 11,   ///< Color channel 0 (RGBA)
    GX_VA_CLR1      = 12,   ///< Color channel 1 (RGBA)
    GX_VA_TEX0      = 13,   ///< Texture coordinate 0 (s, t)
    GX_VA_TEX1      = 14,   ///< Texture coordinate 1
    GX_VA_TEX2      = 15,   ///< Texture coordinate 2
    GX_VA_TEX3      = 16,   ///< Texture coordinate 3
    GX_VA_TEX4      = 17,   ///< Texture coordinate 4
    GX_VA_TEX5      = 18,   ///< Texture coordinate 5
    GX_VA_TEX6      = 19,   ///< Texture coordinate 6
    GX_VA_TEX7      = 20,   ///< Texture coordinate 7
    GX_VA_MAX_ATTR  = 21,   ///< Total number of vertex attributes
    GX_VA_NULL      = 0xFF, ///< Null attribute (unused)
};

/// Vertex attribute data source type.
/// Determines how vertex data is supplied for each attribute:
///   - NONE: Attribute is not present in the vertex
///   - DIRECT: Data is inlined in the command stream (FIFO)
///   - INDEX8/INDEX16: Data is indexed via an 8-bit or 16-bit index into an array
/// Reference: libogc GX_NONE, GX_DIRECT, GX_INDEX8, GX_INDEX16.
enum GXAttrType : uint32_t {
    GX_NONE    = 0, ///< Attribute not present
    GX_DIRECT  = 1, ///< Attribute data inline in FIFO
    GX_INDEX8  = 2, ///< 8-bit index into vertex array
    GX_INDEX16 = 3, ///< 16-bit index into vertex array
};

/// Primitive types for GXBegin().
/// The upper 3 bits encode the primitive type in the GX FIFO command byte.
/// These values are the actual command byte values used in display lists.
/// Reference: libogc GX_QUADS, etc.; Pureikyubu FIFO command format.
enum GXPrimitive : uint32_t {
    GX_QUADS         = 0x80, ///< Quadrilaterals (4 verts per quad)
    GX_TRIANGLES     = 0x90, ///< Independent triangles (3 verts each)
    GX_TRIANGLESTRIP = 0x98, ///< Triangle strip
    GX_TRIANGLEFAN   = 0xA0, ///< Triangle fan
    GX_LINES         = 0xA8, ///< Independent lines (2 verts each)
    GX_LINESTRIP     = 0xB0, ///< Connected line strip
    GX_POINTS        = 0xB8, ///< Individual points
};

/// Texture data formats.
/// The GameCube GPU stores textures in tiled (swizzled) formats optimized for
/// the texture cache. Each format has a specific tile size and bits per pixel.
/// Paletted formats (C4, C8, C14X2) require a separate TLUT (texture lookup table).
/// CMPR is Nintendo's variant of S3TC/DXT1 block compression.
/// Reference: libogc GX_TF_* constants; Pureikyubu texture unit documentation.
enum GXTexFmt : uint32_t {
    GX_TF_I4     = 0x0, ///< 4-bit intensity (grayscale), 8x8 tiles
    GX_TF_I8     = 0x1, ///< 8-bit intensity (grayscale), 8x4 tiles
    GX_TF_IA4    = 0x2, ///< 4-bit intensity + 4-bit alpha, 8x4 tiles
    GX_TF_IA8    = 0x3, ///< 8-bit intensity + 8-bit alpha, 4x4 tiles
    GX_TF_RGB565 = 0x4, ///< 16-bit RGB (5-6-5), 4x4 tiles
    GX_TF_RGB5A3 = 0x5, ///< 16-bit: RGB555 opaque or RGB4A3, 4x4 tiles
    GX_TF_RGBA8  = 0x6, ///< 32-bit RGBA (stored as AR+GB tile pairs), 4x4 tiles
    GX_TF_C4     = 0x8, ///< 4-bit paletted (16 colors via TLUT)
    GX_TF_C8     = 0x9, ///< 8-bit paletted (256 colors via TLUT)
    GX_TF_C14X2  = 0xA, ///< 14-bit paletted (16384 colors via TLUT)
    GX_TF_CMPR   = 0xE, ///< S3TC/DXT1 variant, 8x8 macro tiles of 4x4 sub-blocks
};

/// TEV (Texture Environment) stage indices.
/// The GameCube supports up to 16 cascaded TEV stages. Each stage performs
/// a configurable color and alpha blend operation. Games typically use 1-4
/// stages, but complex effects (cel shading, multi-texturing, indirect
/// texture distortion) can use many more.
/// Reference: libogc GX_TEVSTAGE0..15; US Patent 6,664,962.
enum GXTevStageID : uint32_t {
    GX_TEVSTAGE0  = 0,
    GX_TEVSTAGE1  = 1,
    GX_TEVSTAGE2  = 2,
    GX_TEVSTAGE3  = 3,
    GX_TEVSTAGE4  = 4,
    GX_TEVSTAGE5  = 5,
    GX_TEVSTAGE6  = 6,
    GX_TEVSTAGE7  = 7,
    GX_TEVSTAGE8  = 8,
    GX_TEVSTAGE9  = 9,
    GX_TEVSTAGE10 = 10,
    GX_TEVSTAGE11 = 11,
    GX_TEVSTAGE12 = 12,
    GX_TEVSTAGE13 = 13,
    GX_TEVSTAGE14 = 14,
    GX_TEVSTAGE15 = 15,
    GX_MAX_TEVSTAGE = 16, ///< Maximum number of TEV stages
};

/// TEV color combiner input arguments.
/// Each TEV stage selects 4 color inputs (A, B, C, D) from this set.
/// The combiner computes: result = D + ((1-C)*A + C*B) + bias, then scales.
/// "PREV" refers to the output of the previous TEV stage (or initial color
/// for stage 0). C0/C1/C2 are the 3 general-purpose TEV color registers.
/// Reference: libogc GX_CC_* constants; Pureikyubu TEV register docs.
enum GXTevColorArg : uint32_t {
    GX_CC_CPREV = 0,  ///< Previous stage color RGB
    GX_CC_APREV = 1,  ///< Previous stage alpha (broadcast to RGB)
    GX_CC_C0    = 2,  ///< TEV color register 0 RGB
    GX_CC_A0    = 3,  ///< TEV color register 0 alpha (broadcast)
    GX_CC_C1    = 4,  ///< TEV color register 1 RGB
    GX_CC_A1    = 5,  ///< TEV color register 1 alpha (broadcast)
    GX_CC_C2    = 6,  ///< TEV color register 2 RGB
    GX_CC_A2    = 7,  ///< TEV color register 2 alpha (broadcast)
    GX_CC_TEXC  = 8,  ///< Texture color RGB
    GX_CC_TEXA  = 9,  ///< Texture alpha (broadcast to RGB)
    GX_CC_RASC  = 10, ///< Rasterized (vertex/lighting) color RGB
    GX_CC_RASA  = 11, ///< Rasterized alpha (broadcast to RGB)
    GX_CC_ONE   = 12, ///< Constant 1.0
    GX_CC_HALF  = 13, ///< Constant 0.5
    GX_CC_KONST = 14, ///< Konstant color (per-stage selectable)
    GX_CC_ZERO  = 15, ///< Constant 0.0
};

/// TEV alpha combiner input arguments.
/// Similar to GXTevColorArg but for the alpha channel. Only scalar values.
/// Reference: libogc GX_CA_* constants.
enum GXTevAlphaArg : uint32_t {
    GX_CA_APREV = 0, ///< Previous stage alpha
    GX_CA_A0    = 1, ///< TEV color register 0 alpha
    GX_CA_A1    = 2, ///< TEV color register 1 alpha
    GX_CA_A2    = 3, ///< TEV color register 2 alpha
    GX_CA_TEXA  = 4, ///< Texture alpha
    GX_CA_RASA  = 5, ///< Rasterized (vertex/lighting) alpha
    GX_CA_KONST = 6, ///< Konstant alpha (per-stage selectable)
    GX_CA_ZERO  = 7, ///< Constant 0.0
};

/// TEV combiner operation.
/// Controls whether the combiner adds or subtracts. When bias is set to
/// "compare" mode (bias=3 in the register), the operation changes to one of
/// the comparison operations (R8/GR16/BGR24/RGB8 component comparisons).
/// Reference: libogc GX_TEV_ADD, GX_TEV_SUB, GX_TEV_COMP_* constants.
enum GXTevOp : uint32_t {
    GX_TEV_ADD           = 0,  ///< result = D + lerp(A,B,C) + bias
    GX_TEV_SUB           = 1,  ///< result = D - lerp(A,B,C) + bias
    // Comparison ops (used when bias = 3 in hardware register)
    GX_TEV_COMP_R8_GT    = 8,  ///< Per-component: (A.r > B.r) ? C : 0, + D
    GX_TEV_COMP_R8_EQ    = 9,  ///< Per-component: (A.r == B.r) ? C : 0, + D
    GX_TEV_COMP_GR16_GT  = 10, ///< 16-bit comparison on GR channels
    GX_TEV_COMP_GR16_EQ  = 11,
    GX_TEV_COMP_BGR24_GT = 12, ///< 24-bit comparison on BGR channels
    GX_TEV_COMP_BGR24_EQ = 13,
    GX_TEV_COMP_RGB8_GT  = 14, ///< Per-channel 8-bit comparison
    GX_TEV_COMP_RGB8_EQ  = 15,
    // Alpha comparison ops (same numeric value, context-dependent)
    GX_TEV_COMP_A8_GT    = 14, ///< Alpha: (A.a > B.a) ? C : 0, + D
    GX_TEV_COMP_A8_EQ    = 15, ///< Alpha: (A.a == B.a) ? C : 0, + D
};

/// TEV combiner bias.
/// Added to the combiner result before scaling.
/// Reference: libogc GX_TB_ZERO, GX_TB_ADDHALF, GX_TB_SUBHALF.
enum GXTevBias : uint32_t {
    GX_TB_ZERO     = 0, ///< No bias
    GX_TB_ADDHALF  = 1, ///< Add 0.5 (shifts range up)
    GX_TB_SUBHALF  = 2, ///< Subtract 0.5 (shifts range down)
};

/// TEV combiner output scale.
/// Multiplies the combiner result after bias.
/// Reference: libogc GX_CS_SCALE_* constants.
enum GXTevScale : uint32_t {
    GX_CS_SCALE_1  = 0, ///< Multiply by 1 (no scaling)
    GX_CS_SCALE_2  = 1, ///< Multiply by 2
    GX_CS_SCALE_4  = 2, ///< Multiply by 4
    GX_CS_DIVIDE_2 = 3, ///< Divide by 2 (multiply by 0.5)
};

/// Framebuffer blend mode.
/// Controls how pixel shader output is combined with the framebuffer.
/// Reference: libogc GX_BM_* constants; Pureikyubu blending processor docs.
enum GXBlendMode : uint32_t {
    GX_BM_NONE     = 0, ///< No blending (overwrite)
    GX_BM_BLEND    = 1, ///< Alpha blending: src*srcFactor + dst*dstFactor
    GX_BM_LOGIC    = 2, ///< Logic operation (AND, OR, XOR, etc.)
    GX_BM_SUBTRACT = 3, ///< Subtractive blending: dst - src
};

/// Blend factor for source and destination.
/// Reference: libogc GX_BL_* constants.
enum GXBlendFactor : uint32_t {
    GX_BL_ZERO        = 0, ///< Factor = 0
    GX_BL_ONE         = 1, ///< Factor = 1
    GX_BL_SRCCLR      = 2, ///< Factor = source color
    GX_BL_INVSRCCLR   = 3, ///< Factor = 1 - source color
    GX_BL_SRCALPHA    = 4, ///< Factor = source alpha
    GX_BL_INVSRCALPHA = 5, ///< Factor = 1 - source alpha
    GX_BL_DSTALPHA    = 6, ///< Factor = destination alpha
    GX_BL_INVDSTALPHA = 7, ///< Factor = 1 - destination alpha
};

/// Comparison function for depth test and alpha test.
/// Used by GXSetZMode and GXSetAlphaCompare.
/// Reference: libogc GX_NEVER..GX_ALWAYS.
enum GXCompare : uint32_t {
    GX_NEVER   = 0, ///< Never pass
    GX_LESS    = 1, ///< Pass if src < dst
    GX_EQUAL   = 2, ///< Pass if src == dst
    GX_LEQUAL  = 3, ///< Pass if src <= dst
    GX_GREATER = 4, ///< Pass if src > dst
    GX_NEQUAL  = 5, ///< Pass if src != dst
    GX_GEQUAL  = 6, ///< Pass if src >= dst
    GX_ALWAYS  = 7, ///< Always pass
};

/// Face culling mode.
/// Note: The GameCube uses opposite winding from some APIs. GX_CULL_FRONT
/// culls front-facing triangles (as determined by the GC's winding rule).
/// Reference: libogc GX_CULL_* constants; Pureikyubu rasterizer docs.
enum GXCullMode : uint32_t {
    GX_CULL_NONE  = 0, ///< No culling (render both sides)
    GX_CULL_FRONT = 1, ///< Cull front-facing polygons
    GX_CULL_BACK  = 2, ///< Cull back-facing polygons
    GX_CULL_ALL   = 3, ///< Cull all polygons
};

// =============================================================================
// TEV Stage Configuration
//
// Each TEV stage has independent color and alpha combiners. The combiner
// formula is: result = D op ((1-C)*A + C*B) + bias, then multiplied by scale,
// then optionally clamped to [0, 1].
//
// The output is written to one of 4 registers: PREV (default, flows to next
// stage), or C0/C1/C2 (general-purpose, persist across stages).
//
// Reference: Pureikyubu TEV stage register layout; libogc GXSetTevColorIn/Op.
// =============================================================================

struct GXTevStageConfig {
    /// Color combiner inputs: result = d + ((1-c)*a + c*b) + bias, scaled
    GXTevColorArg color_a, color_b, color_c, color_d;
    GXTevOp       color_op;
    GXTevBias     color_bias;
    GXTevScale    color_scale;
    bool          color_clamp;
    uint32_t      color_reg_id;  ///< Output register (0=PREV, 1=C0, 2=C1, 3=C2)

    /// Alpha combiner inputs (same formula, scalar values only)
    GXTevAlphaArg alpha_a, alpha_b, alpha_c, alpha_d;
    GXTevOp       alpha_op;
    GXTevBias     alpha_bias;
    GXTevScale    alpha_scale;
    bool          alpha_clamp;
    uint32_t      alpha_reg_id;  ///< Output register for alpha

    /// Texture and rasterizer channel assignments for this stage
    uint32_t      tex_coord_id;  ///< Which tex coord gen to use (0-7, or 0xFF for none)
    uint32_t      tex_map_id;    ///< Which texture map to sample (0-7, or 0xFF for none)
    uint32_t      channel_id;    ///< Rasterized color channel (0=COLOR0, 1=COLOR1, 0xFF=none)
};

// =============================================================================
// Indirect Texture Stage Configuration
//
// The GameCube supports up to 4 indirect texture stages that can modify
// texture coordinates before sampling. This enables effects like water
// distortion, heat haze, normal mapping, and environment mapping.
//
// Each indirect stage samples a texture, then uses the result to offset
// the texture coordinates of a regular TEV stage.
//
// Reference: Pureikyubu indirect texture documentation; US Patent 6,664,962.
// =============================================================================

/// Configuration for an indirect texture lookup stage (0-3).
struct GXIndTexStageConfig {
    uint32_t tex_coord;     ///< Texture coordinate source for indirect lookup
    uint32_t tex_map;       ///< Texture map to sample for indirect offset
};

/// Per-TEV-stage indirect texture application configuration.
struct GXIndTevConfig {
    uint8_t  ind_stage;     ///< Which indirect stage (0-3) to use
    uint8_t  format;        ///< Indirect format (controls bit depth: 3-8 bits)
    uint8_t  bias;          ///< Bias selection for offset components
    uint8_t  matrix;        ///< Indirect matrix to transform the offset
    uint8_t  wrap_s;        ///< S-axis wrapping mode for indirect coords
    uint8_t  wrap_t;        ///< T-axis wrapping mode for indirect coords
    bool     add_prev;      ///< Add previous stage's indirect offset
    bool     utc_lod;       ///< Use unmodified texture coordinate for LOD calculation
};

// =============================================================================
// GX State
//
// Central state structure that mirrors the GameCube GPU's register state.
// This is the complete rendering configuration needed to generate shaders
// and configure the D3D11 pipeline.
//
// Reference: Pureikyubu BP/CP/XF register documentation.
// =============================================================================

struct GXState {
    // TEV configuration
    uint32_t         num_tev_stages;                ///< Active TEV stages (1-16)
    GXTevStageConfig tev_stages[GX_MAX_TEVSTAGE];   ///< Per-stage TEV config

    // Indirect texture configuration
    uint32_t          num_ind_stages;                ///< Active indirect stages (0-4)
    GXIndTexStageConfig ind_stages[4];               ///< Indirect lookup configs
    GXIndTevConfig      ind_tev[GX_MAX_TEVSTAGE];    ///< Per-TEV indirect configs

    // Konst color registers (4 RGBA colors available to TEV via KONST input)
    float            konst[4][4];   ///< Konstant colors [register][RGBA]

    // TEV color registers (PREV, REG0, REG1, REG2)
    float            tev_reg[4][4]; ///< TEV output registers [register][RGBA]

    // Konst color/alpha selection per TEV stage
    uint8_t          kcolor_sel[GX_MAX_TEVSTAGE]; ///< Konst color swizzle per stage
    uint8_t          kalpha_sel[GX_MAX_TEVSTAGE]; ///< Konst alpha swizzle per stage

    // Vertex format
    GXAttrType       vtx_attr_type[GX_VA_MAX_ATTR]; ///< Per-attribute data source

    // Blending
    GXBlendMode      blend_mode;    ///< Framebuffer blend mode
    GXBlendFactor    src_factor;    ///< Source blend factor
    GXBlendFactor    dst_factor;    ///< Destination blend factor

    // Depth
    bool             z_enable;      ///< Depth test enable
    bool             z_write;       ///< Depth write enable
    GXCompare        z_func;        ///< Depth comparison function

    // Alpha compare (pixel discard before blending)
    GXCompare        alpha_func0;   ///< First alpha compare function
    GXCompare        alpha_func1;   ///< Second alpha compare function
    uint8_t          alpha_op;      ///< Combine op: 0=AND, 1=OR, 2=XOR, 3=XNOR
    uint8_t          alpha_ref0;    ///< First alpha reference value (0-255)
    uint8_t          alpha_ref1;    ///< Second alpha reference value (0-255)

    // Culling
    GXCullMode       cull_mode;     ///< Face culling mode

    // Fog
    uint8_t          fog_type;      ///< Fog type (0=none, 2=linear, 4=exp, 5=exp2)
    float            fog_start, fog_end;  ///< Fog range
    float            fog_near, fog_far;   ///< Near/far planes for fog
    float            fog_color[4];  ///< Fog color RGBA

    // Viewport (maps normalized device coords to screen pixels)
    float            viewport_x, viewport_y;
    float            viewport_w, viewport_h;
    float            viewport_near, viewport_far;

    // Scissor rectangle
    uint32_t         scissor_x, scissor_y;
    uint32_t         scissor_w, scissor_h;

    // Number of active texture coordinate generators (0-8)
    uint32_t         num_tex_gens;

    // Number of active color channels (0-2)
    uint32_t         num_color_chans;

    /// Reset all state to hardware defaults.
    void reset();
};

// =============================================================================
// GX API Functions (called by recompiled game code)
//
// These mirror the Nintendo GX SDK functions. Recompiled game code calls
// these directly; the implementations update GXState and/or dispatch to
// the D3D11 backend.
//
// Reference: libogc function signatures; Nintendo GameCube SDK documentation.
// =============================================================================

/// Initialize the GX subsystem. Must be called before any other GX function.
void GXInit();

/// Set the viewport transform (maps clip space to screen coordinates).
/// Parameters match the GameCube's GXSetViewport with origin at top-left.
void GXSetViewport(float x, float y, float w, float h, float near, float far);

/// Set the scissor rectangle for pixel clipping.
void GXSetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/// Set the polygon face culling mode.
void GXSetCullMode(GXCullMode mode);

/// Configure framebuffer blending.
/// @param type Blend mode (none, blend, logic, subtract)
/// @param src Source blend factor
/// @param dst Destination blend factor
/// @param logic_op Logic operation (only used when type == GX_BM_LOGIC)
void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, uint32_t logic_op);

/// Configure depth (Z-buffer) testing and writing.
void GXSetZMode(bool enable, GXCompare func, bool write_enable);

/// Set the number of active TEV stages (1-16).
void GXSetNumTevStages(uint32_t count);

/// Assign texture coordinate, texture map, and color channel to a TEV stage.
void GXSetTevOrder(GXTevStageID stage, uint32_t coord, uint32_t map, uint32_t color);

/// Set color combiner inputs for a TEV stage.
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d);

/// Set color combiner operation, bias, scale, clamp, and output register for a TEV stage.
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp, uint32_t reg);

/// Set alpha combiner inputs for a TEV stage.
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d);

/// Set alpha combiner operation, bias, scale, clamp, and output register for a TEV stage.
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp, uint32_t reg);

/// Begin submitting vertices for a primitive.
/// @param prim Primitive type
/// @param vtx_fmt Vertex format index (0-7)
/// @param num_verts Number of vertices to follow
void GXBegin(GXPrimitive prim, uint32_t vtx_fmt, uint32_t num_verts);

/// End vertex submission (flushes the current primitive batch).
void GXEnd();

/// Set vertex attribute format for a given format index.
void GXSetVtxAttrFmt(uint32_t fmt, GXAttr attr, uint32_t comp_type, uint32_t comp_size, uint32_t frac);

/// Clear all vertex attribute descriptors (set all to GX_NONE).
void GXClearVtxDesc();

/// Set a single vertex attribute descriptor.
void GXSetVtxDesc(GXAttr attr, GXAttrType type);

/// Copy the Embedded Frame Buffer (EFB) to an external frame buffer (XFB).
/// @param dest Destination address in GameCube memory
/// @param clear If true, clear the EFB after copying
void GXCopyDisp(void* dest, bool clear);

/// Set the clear color and depth values for GXCopyDisp.
void GXSetCopyClear(uint32_t color, uint32_t z);

/// Wait for the GPU to finish all pending rendering.
void GXDrawDone();

/// Flush the GX command buffer to the GPU.
void GXFlush();

/// Set a TEV konstant color register (0-3).
/// @param reg Register index (0-3)
/// @param color Packed RGBA8 color (R in bits 31-24)
void GXSetTevKColor(uint32_t reg, uint32_t color);

/// Select which konstant color swizzle to use for a TEV stage's color input.
void GXSetTevKColorSel(GXTevStageID stage, uint8_t sel);

/// Select which konstant alpha swizzle to use for a TEV stage's alpha input.
void GXSetTevKAlphaSel(GXTevStageID stage, uint8_t sel);

/// Set a TEV color register (PREV=0, C0=1, C1=2, C2=3).
void GXSetTevColor(uint32_t reg, uint32_t color);

/// Set the number of active indirect texture stages (0-4).
void GXSetNumIndStages(uint32_t count);

/// Configure an indirect texture stage's coordinate and map sources.
void GXSetIndTexOrder(uint32_t stage, uint32_t coord, uint32_t map);

/// Configure how a TEV stage uses indirect texture offsets.
void GXSetTevIndirect(GXTevStageID stage, uint32_t ind_stage, uint32_t fmt,
                       uint32_t bias, uint32_t matrix, uint32_t wrap_s,
                       uint32_t wrap_t, bool add_prev, bool utc_lod);

/// Configure the alpha compare test (pixel discard before blending).
/// Two comparison functions are combined with a logic operation.
void GXSetAlphaCompare(GXCompare func0, uint8_t ref0, uint8_t op, GXCompare func1, uint8_t ref1);

/// Configure fog parameters.
void GXSetFog(uint8_t type, float start, float end, float near_z, float far_z, uint32_t color);

/// Set the number of active texture coordinate generators (0-8).
void GXSetNumTexGens(uint32_t count);

/// Set the number of active color channels (0-2).
void GXSetNumChans(uint32_t count);

/// Load a 3x4 position/normal matrix into GX matrix memory.
/// @param mtx 3x4 matrix data (row-major)
/// @param id Matrix memory slot (0-9, each slot is 12 floats)
void GXLoadPosMtxImm(const float mtx[3][4], uint32_t id);

/// Select which matrix memory slot to use as the current position matrix.
void GXSetCurrentMtx(uint32_t id);

/// Set the projection matrix.
/// @param mtx 4x4 projection matrix
/// @param type 0 = perspective, 1 = orthographic
void GXSetProjection(const float mtx[4][4], uint32_t type);

/// Execute a precompiled display list (GX command buffer).
/// @param data Pointer to display list data
/// @param size Size of display list in bytes
void GXCallDisplayList(const uint8_t* data, uint32_t size);

/// Generate an HLSL pixel shader from the current TEV configuration.
/// This is the core of the TEV-to-shader translation pipeline.
/// Reference: GameCubeRecompiled shader generation approach.
std::string generate_tev_shader(const GXState& state);

/// Initialize the D3D11 rendering backend.
/// @param hwnd Window handle (nullptr to create a new window)
/// @param width Window width in pixels
/// @param height Window height in pixels
bool GXInitBackend(void* hwnd, uint32_t width, uint32_t height);

/// Shut down the D3D11 backend and release all resources.
void GXShutdownBackend();

/// Present the rendered frame (swap buffers).
void GXPresent();

} // namespace gcrecomp::gx
