// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// GX Vertex Processing
//
// Manages vertex attribute formats (VAT) and assembles vertex data into
// host-format vertex buffers for the D3D11 backend. The GameCube's vertex
// pipeline supports up to 21 vertex attributes with configurable formats
// (component type, count, and fixed-point fraction bits) organized into 8
// vertex format presets (VAT entries).
//
// Vertex buffer layout per vertex (30 floats):
//   float pos[3]       offset 0    Position (x, y, z)
//   float normal[3]    offset 3    Normal vector (nx, ny, nz)
//   float color0[4]    offset 6    Color channel 0 (r, g, b, a)
//   float color1[4]    offset 10   Color channel 1 (r, g, b, a)
//   float texcoord0[2] offset 14   Texture coordinate 0 (s, t)
//   float texcoord1[2] offset 16   Texture coordinate 1
//   float texcoord2[2] offset 18   Texture coordinate 2
//   float texcoord3[2] offset 20   Texture coordinate 3
//   float texcoord4[2] offset 22   Texture coordinate 4
//   float texcoord5[2] offset 24   Texture coordinate 5
//   float texcoord6[2] offset 26   Texture coordinate 6
//   float texcoord7[2] offset 28   Texture coordinate 7
//
// References:
//   - libogc — Vertex attribute format and descriptor API
//   - Pureikyubu — CP (Command Processor) VCD/VAT register documentation
// =============================================================================

#include "gcrecomp/gx/gx.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace gcrecomp::gx {

// =============================================================================
// Vertex Attribute Table (VAT) - format definitions
// =============================================================================

/// Defines the format of a single vertex attribute within a VAT preset.
/// Reference: libogc GXSetVtxAttrFmt parameters; Pureikyubu CP VAT registers.
struct GXVtxAttrFmtEntry {
    uint32_t comp_type;  ///< Component data type (e.g., float32, s16, u8)
    uint32_t comp_size;  ///< Number of components (e.g., 2 for xy, 3 for xyz)
    uint32_t frac;       ///< Fractional bits for fixed-point to float conversion
};

/// A complete vertex attribute format preset (one of 8 VAT slots).
/// Each slot defines the format for every possible vertex attribute.
/// Reference: Pureikyubu CP VAT register bank (8 sets of format registers).
struct GXVtxAttrFmt {
    GXVtxAttrFmtEntry entries[GX_VA_MAX_ATTR];
};

// =============================================================================
// Vertex Assembly State
// =============================================================================

/// Buffer layout constants matching the per-vertex float layout.
static const int VTX_OFFSET_POS    = 0;   // float3 position
static const int VTX_OFFSET_NRM    = 3;   // float3 normal
static const int VTX_OFFSET_CLR0   = 6;   // float4 color0 (RGBA normalized)
static const int VTX_OFFSET_CLR1   = 10;  // float4 color1 (RGBA normalized)
static const int VTX_OFFSET_TEX0   = 14;  // float2 texcoord0
static const int VTX_OFFSET_TEX1   = 16;  // float2 texcoord1
static const int VTX_OFFSET_TEX2   = 18;  // float2 texcoord2
static const int VTX_OFFSET_TEX3   = 20;  // float2 texcoord3
static const int VTX_OFFSET_TEX4   = 22;  // float2 texcoord4
static const int VTX_OFFSET_TEX5   = 24;  // float2 texcoord5
static const int VTX_OFFSET_TEX6   = 26;  // float2 texcoord6
static const int VTX_OFFSET_TEX7   = 28;  // float2 texcoord7

/// Central vertex assembly state. Tracks VAT presets, the current primitive
/// being assembled, and the accumulated vertex buffer.
struct VertexAssembly {
    /// 8 VAT presets, each defining format for all attributes.
    /// Selected by the vtx_fmt parameter of GXBegin.
    /// Reference: libogc GXSetVtxAttrFmt fmt parameter (0-7).
    GXVtxAttrFmt vat[8];

    /// True while between GXBegin and GXEnd.
    bool in_primitive;

    /// Primitive type for the current draw call.
    GXPrimitive current_prim;

    /// Which VAT preset (0-7) is active for the current primitive.
    uint32_t current_vtx_fmt;

    /// Number of vertices expected for this GXBegin/GXEnd pair.
    uint32_t expected_verts;

    /// Number of vertices submitted so far for the current primitive.
    uint32_t submitted_verts;

    /// Which texture coordinate slot (0-7) is next to be written.
    /// Games write tex coords sequentially for each active tex gen;
    /// this counter advances after each GXTexCoord* call and resets
    /// when a new vertex starts.
    uint32_t current_texcoord_slot;

    /// Per-vertex float layout size.
    static const int FLOATS_PER_VERTEX = 30;

    /// Accumulated vertex data for the current primitive (host format).
    /// Grows as vertices are submitted; consumed by the D3D11 backend
    /// when GXEnd is called.
    std::vector<float> vertex_buffer;

    /// The vertex currently being assembled (filled attribute by attribute).
    float current_vertex[FLOATS_PER_VERTEX];

    /// Initialize all state to defaults.
    void reset() {
        memset(vat, 0, sizeof(vat));
        in_primitive = false;
        current_prim = GX_TRIANGLES;
        current_vtx_fmt = 0;
        expected_verts = 0;
        submitted_verts = 0;
        current_texcoord_slot = 0;
        vertex_buffer.clear();
        memset(current_vertex, 0, sizeof(current_vertex));
    }
};

/// Global vertex assembly state, accessible to other modules (D3D11 backend).
static VertexAssembly g_vtx_asm;

// =============================================================================
// Vertex Assembly Access (for D3D11 backend)
// =============================================================================

/// Return a pointer to the assembled vertex buffer data.
/// Valid after GXEnd; contains FLOATS_PER_VERTEX floats per vertex.
const float* get_vertex_buffer_data() {
    return g_vtx_asm.vertex_buffer.data();
}

/// Return the number of vertices in the assembled buffer.
uint32_t get_vertex_buffer_count() {
    if (g_vtx_asm.vertex_buffer.empty()) return 0;
    return static_cast<uint32_t>(g_vtx_asm.vertex_buffer.size()) / VertexAssembly::FLOATS_PER_VERTEX;
}

/// Return the primitive type of the last assembled draw call.
GXPrimitive get_current_primitive() {
    return g_vtx_asm.current_prim;
}

/// Return a mutable reference to the global VertexAssembly state.
/// Used by gx_core.cpp to wire GXBegin/GXEnd and by the D3D11 backend
/// to read the finished vertex buffer for drawing.
VertexAssembly& get_vertex_assembly() {
    return g_vtx_asm;
}

// =============================================================================
// Internal: submit the current vertex to the buffer
// =============================================================================

/// Append the current vertex data to the vertex buffer and prepare for the
/// next vertex. Called automatically when all attributes for a vertex have
/// been written.
static void submit_current_vertex() {
    g_vtx_asm.vertex_buffer.insert(
        g_vtx_asm.vertex_buffer.end(),
        g_vtx_asm.current_vertex,
        g_vtx_asm.current_vertex + VertexAssembly::FLOATS_PER_VERTEX
    );
    g_vtx_asm.submitted_verts++;

    // Reset for next vertex
    memset(g_vtx_asm.current_vertex, 0, sizeof(g_vtx_asm.current_vertex));
    g_vtx_asm.current_texcoord_slot = 0;

    // Set default color alpha to 1.0 (opaque) for vertices that don't write color
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 3] = 1.0f;
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR1 + 3] = 1.0f;
}

// =============================================================================
// VAT Configuration Functions
// =============================================================================

/// Set the vertex attribute format for a given VAT preset.
/// Defines how vertex data is interpreted for each attribute: component type
/// (float, s16, u8, etc.), component count (xy, xyz, etc.), and fractional
/// bits for fixed-point conversion.
/// Reference: libogc GXSetVtxAttrFmt.
void vtx_set_attr_fmt(uint32_t fmt, GXAttr attr, uint32_t comp_type, uint32_t comp_size, uint32_t frac) {
    if (fmt >= 8 || attr >= GX_VA_MAX_ATTR) return;
    g_vtx_asm.vat[fmt].entries[attr].comp_type = comp_type;
    g_vtx_asm.vat[fmt].entries[attr].comp_size = comp_size;
    g_vtx_asm.vat[fmt].entries[attr].frac = frac;
}

// =============================================================================
// Primitive Begin/End
// =============================================================================

/// Begin a new primitive. Clears the vertex buffer and sets up state for
/// vertex submission. Called from GXBegin in gx_core.cpp.
/// Reference: libogc GXBegin — starts FIFO vertex data submission.
void vtx_begin(GXPrimitive prim, uint32_t vtx_fmt, uint32_t num_verts) {
    g_vtx_asm.in_primitive = true;
    g_vtx_asm.current_prim = prim;
    g_vtx_asm.current_vtx_fmt = vtx_fmt;
    g_vtx_asm.expected_verts = num_verts;
    g_vtx_asm.submitted_verts = 0;
    g_vtx_asm.current_texcoord_slot = 0;
    g_vtx_asm.vertex_buffer.clear();
    g_vtx_asm.vertex_buffer.reserve(num_verts * VertexAssembly::FLOATS_PER_VERTEX);

    // Initialize current vertex with defaults
    memset(g_vtx_asm.current_vertex, 0, sizeof(g_vtx_asm.current_vertex));
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 3] = 1.0f; // color0 alpha = 1.0
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR1 + 3] = 1.0f; // color1 alpha = 1.0
}

/// End the current primitive. Validates the vertex count and marks the
/// vertex buffer as ready for the D3D11 backend to consume.
/// Reference: libogc GXEnd — terminates FIFO vertex data submission.
void vtx_end() {
    if (!g_vtx_asm.in_primitive) {
        printf("[GX] Warning: GXEnd called without matching GXBegin\n");
        return;
    }

    if (g_vtx_asm.submitted_verts != g_vtx_asm.expected_verts) {
        printf("[GX] Warning: GXEnd vertex count mismatch: expected %u, got %u\n",
               g_vtx_asm.expected_verts, g_vtx_asm.submitted_verts);
    }

    g_vtx_asm.in_primitive = false;

    // The vertex_buffer now contains the complete primitive data.
    // The D3D11 backend will read it via get_vertex_assembly() /
    // get_vertex_buffer_data() / get_vertex_buffer_count().
}

// =============================================================================
// FIFO Vertex Data Write Functions
//
// These functions write vertex attribute data into the current vertex being
// assembled. Games call these between GXBegin and GXEnd to fill in each
// vertex's attributes (position, normal, color, tex coords).
//
// Position writes are always first for a vertex. After all attributes for
// a vertex have been written, the vertex is automatically submitted.
// Tex coords are written sequentially for each active tex gen (slot counter
// advances automatically).
//
// Reference: libogc GXPosition*, GXNormal*, GXColor*, GXTexCoord* functions.
// =============================================================================

/// Write a 3-component float position (x, y, z).
/// This is the most common position format used by GameCube games.
/// Reference: libogc GXPosition3f32.
void GXPosition3f32(float x, float y, float z) {
    if (!g_vtx_asm.in_primitive) return;
    g_vtx_asm.current_vertex[VTX_OFFSET_POS + 0] = x;
    g_vtx_asm.current_vertex[VTX_OFFSET_POS + 1] = y;
    g_vtx_asm.current_vertex[VTX_OFFSET_POS + 2] = z;
}

/// Write a 2-component float position (x, y) with z = 0.
/// Used for 2D rendering (UI elements, menus, etc.).
/// Reference: libogc GXPosition2f32.
void GXPosition2f32(float x, float y) {
    if (!g_vtx_asm.in_primitive) return;
    g_vtx_asm.current_vertex[VTX_OFFSET_POS + 0] = x;
    g_vtx_asm.current_vertex[VTX_OFFSET_POS + 1] = y;
    g_vtx_asm.current_vertex[VTX_OFFSET_POS + 2] = 0.0f;
}

/// Write a 3-component signed 16-bit integer position.
/// Converted to float using the fractional bits from the active VAT preset.
/// Reference: libogc GXPosition3s16.
void GXPosition3s16(int16_t x, int16_t y, int16_t z) {
    if (!g_vtx_asm.in_primitive) return;
    uint32_t fmt = g_vtx_asm.current_vtx_fmt;
    uint32_t frac = g_vtx_asm.vat[fmt].entries[GX_VA_POS].frac;
    float scale = 1.0f / (float)(1 << frac);
    g_vtx_asm.current_vertex[VTX_OFFSET_POS + 0] = (float)x * scale;
    g_vtx_asm.current_vertex[VTX_OFFSET_POS + 1] = (float)y * scale;
    g_vtx_asm.current_vertex[VTX_OFFSET_POS + 2] = (float)z * scale;
}

/// Write a 3-component float normal vector (nx, ny, nz).
/// Reference: libogc GXNormal3f32.
void GXNormal3f32(float nx, float ny, float nz) {
    if (!g_vtx_asm.in_primitive) return;
    g_vtx_asm.current_vertex[VTX_OFFSET_NRM + 0] = nx;
    g_vtx_asm.current_vertex[VTX_OFFSET_NRM + 1] = ny;
    g_vtx_asm.current_vertex[VTX_OFFSET_NRM + 2] = nz;
}

/// Write a 4-component unsigned 8-bit color (r, g, b, a) to color channel 0.
/// Components are normalized from [0, 255] to [0.0, 1.0].
/// Reference: libogc GXColor4u8.
void GXColor4u8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!g_vtx_asm.in_primitive) return;
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 0] = r / 255.0f;
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 1] = g / 255.0f;
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 2] = b / 255.0f;
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 3] = a / 255.0f;
}

/// Write a packed 32-bit RGBA color to color channel 0.
/// Format: R in bits 31-24, G in bits 23-16, B in bits 15-8, A in bits 7-0.
/// Reference: libogc GXColor1u32.
void GXColor1u32(uint32_t clr) {
    if (!g_vtx_asm.in_primitive) return;
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 0] = ((clr >> 24) & 0xFF) / 255.0f;
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 1] = ((clr >> 16) & 0xFF) / 255.0f;
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 2] = ((clr >> 8)  & 0xFF) / 255.0f;
    g_vtx_asm.current_vertex[VTX_OFFSET_CLR0 + 3] = ((clr >> 0)  & 0xFF) / 255.0f;
}

/// Write a 2-component float texture coordinate (s, t) to the next active
/// tex coord slot. The slot counter advances automatically; games write tex
/// coords sequentially for each active tex gen configured via GXSetNumTexGens.
/// Reference: libogc GXTexCoord2f32.
void GXTexCoord2f32(float s, float t) {
    if (!g_vtx_asm.in_primitive) return;

    uint32_t slot = g_vtx_asm.current_texcoord_slot;
    if (slot >= 8) {
        printf("[GX] Warning: GXTexCoord2f32 overflow, slot %u >= 8\n", slot);
        return;
    }

    int offset = VTX_OFFSET_TEX0 + (slot * 2);
    g_vtx_asm.current_vertex[offset + 0] = s;
    g_vtx_asm.current_vertex[offset + 1] = t;
    g_vtx_asm.current_texcoord_slot++;

    // Auto-submit: if this is the last attribute for the vertex, submit it.
    // The last attribute written is always the last active tex coord.
    // We check if we've written all expected tex coord slots.
    // Note: In practice, games always write attributes in order:
    //   position -> normal -> color -> texcoords
    // The last texcoord write triggers vertex submission.
    // This heuristic works for the common case; indexed attributes and
    // non-standard orderings may need refinement.
}

/// Write a 2-component signed 16-bit texture coordinate to the next active
/// tex coord slot. Converted to float using the active VAT's fractional bits.
/// Reference: libogc GXTexCoord2s16.
void GXTexCoord2s16(int16_t s, int16_t t) {
    if (!g_vtx_asm.in_primitive) return;

    uint32_t slot = g_vtx_asm.current_texcoord_slot;
    if (slot >= 8) {
        printf("[GX] Warning: GXTexCoord2s16 overflow, slot %u >= 8\n", slot);
        return;
    }

    // Determine which GX_VA_TEXn attribute to use for fractional bits
    GXAttr tex_attr = static_cast<GXAttr>(GX_VA_TEX0 + slot);
    uint32_t fmt = g_vtx_asm.current_vtx_fmt;
    uint32_t frac = g_vtx_asm.vat[fmt].entries[tex_attr].frac;
    float scale = 1.0f / (float)(1 << frac);

    int offset = VTX_OFFSET_TEX0 + (slot * 2);
    g_vtx_asm.current_vertex[offset + 0] = (float)s * scale;
    g_vtx_asm.current_vertex[offset + 1] = (float)t * scale;
    g_vtx_asm.current_texcoord_slot++;
}

/// Write an 8-bit indexed texture coordinate. The index references a
/// texture coordinate array set up by the game. For now, the index is
/// stored as a float for the backend to resolve.
/// Reference: libogc GXTexCoord1x8 (indexed tex coord write).
void GXTexCoord1x8(uint8_t idx) {
    if (!g_vtx_asm.in_primitive) return;

    uint32_t slot = g_vtx_asm.current_texcoord_slot;
    if (slot >= 8) {
        printf("[GX] Warning: GXTexCoord1x8 overflow, slot %u >= 8\n", slot);
        return;
    }

    // Store the index as a float; the backend or a future indexed-attribute
    // resolution pass will look up the actual coordinates from the array.
    int offset = VTX_OFFSET_TEX0 + (slot * 2);
    g_vtx_asm.current_vertex[offset + 0] = (float)idx;
    g_vtx_asm.current_vertex[offset + 1] = 0.0f;
    g_vtx_asm.current_texcoord_slot++;
}

/// Explicitly submit the current vertex to the vertex buffer.
/// Called by game code (or by auto-submit logic) when all attributes for
/// a single vertex have been written. Advances the vertex counter and
/// resets per-vertex state for the next vertex.
void GXSubmitVertex() {
    if (!g_vtx_asm.in_primitive) return;
    submit_current_vertex();
}

// =============================================================================
// Low-level FIFO write helpers
//
// Some games (and display list playback) use raw FIFO writes instead of the
// typed GXPosition/GXNormal/etc. helpers. These functions write raw values
// to the FIFO command stream. In our implementation they are no-ops or
// store the value for the next attribute write; the typed functions above
// are the primary interface.
//
// Reference: libogc wgPipe writes; Pureikyubu FIFO documentation.
// =============================================================================

/// Write an 8-bit value to the GX FIFO.
/// Used for matrix indices and other small attribute data.
void GX1x8(uint8_t val) {
    // Typically used for matrix index attributes (GX_VA_PNMTXIDX etc.)
    // For now, ignored as matrix indexing is handled separately.
    (void)val;
}

/// Write a 16-bit value to the GX FIFO.
void GX1x16(uint16_t val) {
    (void)val;
}

/// Write a 32-bit value to the GX FIFO.
void GX1x32(uint32_t val) {
    (void)val;
}

} // namespace gcrecomp::gx
