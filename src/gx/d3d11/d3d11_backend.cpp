// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// Direct3D 11 Backend
//
// Creates the window, swap chain, and renders the recompiled game's output.
// Target: 640x480 internal (matching GameCube EFB), upscaled to window size.
//
// The GameCube's Embedded Frame Buffer (EFB) is 640x528 at maximum, but
// most games use 640x480 for NTSC output. This backend creates a D3D11
// render target at the requested resolution and handles presentation.
//
// References:
//   - Pureikyubu — EFB dimensions and pixel format documentation
//   - Microsoft D3D11 SDK — Device creation and swap chain setup
// =============================================================================

#ifdef _WIN32

#include "gcrecomp/gx/gx.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace gcrecomp::gx {

// D3D11 state
static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static IDXGISwapChain*         g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11DepthStencilView* g_dsv = nullptr;
static ID3D11Texture2D*        g_depth_buffer = nullptr;
static HWND                    g_hwnd = nullptr;

// Internal framebuffer (GameCube EFB is 640x528, we use 640x480 for simplicity)
static const uint32_t EFB_WIDTH = 640;
static const uint32_t EFB_HEIGHT = 480;

// Window dimensions for viewport/scissor defaults
static uint32_t g_window_width = 640;
static uint32_t g_window_height = 480;

// Pipeline state caches (keyed by config hash)
static std::unordered_map<uint64_t, ID3D11BlendState*>        g_blend_cache;
static std::unordered_map<uint64_t, ID3D11DepthStencilState*> g_depth_cache;
static std::unordered_map<uint64_t, ID3D11RasterizerState*>   g_raster_cache;

// Reusable dynamic vertex buffer
static ID3D11Buffer* g_dyn_vb = nullptr;
static uint32_t      g_dyn_vb_capacity = 0;

// Constant buffers (created once, updated each draw)
static ID3D11Buffer* g_vs_cb = nullptr;
static ID3D11Buffer* g_ps_cb = nullptr;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        // TODO: Handle resize
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            PostQuitMessage(0);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool GXInitBackend(void* hwnd_or_null, uint32_t width, uint32_t height) {
    printf("[D3D11] Initializing backend (%ux%u)\n", width, height);

    g_window_width = width;
    g_window_height = height;

    HWND hwnd = (HWND)hwnd_or_null;

    if (!hwnd) {
        // Create window
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"gcrecomp";
        RegisterClassExW(&wc);

        RECT rc = { 0, 0, (LONG)width, (LONG)height };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        hwnd = CreateWindowExW(
            0, L"gcrecomp",
            L"gcrecomp - GameCube Static Recompilation",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, wc.hInstance, nullptr
        );

        ShowWindow(hwnd, SW_SHOW);
    }

    g_hwnd = hwnd;

    // Create device and swap chain
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        &feature_level, 1, D3D11_SDK_VERSION,
        &scd, &g_swap_chain, &g_device, nullptr, &g_context
    );

    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] Failed to create device: 0x%08lX\n", hr);
        return false;
    }

    // Create render target view
    ID3D11Texture2D* back_buffer;
    g_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back_buffer);
    g_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
    back_buffer->Release();

    // Create depth buffer
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = width;
    dd.Height = height;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    g_device->CreateTexture2D(&dd, nullptr, &g_depth_buffer);
    g_device->CreateDepthStencilView(g_depth_buffer, nullptr, &g_dsv);

    // Set render targets
    g_context->OMSetRenderTargets(1, &g_rtv, g_dsv);

    // Set viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MaxDepth = 1.0f;
    g_context->RSSetViewports(1, &vp);

    // Initialize shader subsystem (compile vertex shader, create input layout)
    if (!GXInitShaders(g_device)) {
        fprintf(stderr, "[D3D11] Failed to initialize shaders\n");
        GXShutdownBackend();
        return false;
    }

    printf("[D3D11] Backend ready.\n");
    return true;
}

void GXShutdownBackend() {
    // Release pipeline state caches
    for (auto& p : g_blend_cache)  { if (p.second) p.second->Release(); }
    for (auto& p : g_depth_cache)  { if (p.second) p.second->Release(); }
    for (auto& p : g_raster_cache) { if (p.second) p.second->Release(); }
    g_blend_cache.clear();
    g_depth_cache.clear();
    g_raster_cache.clear();

    // Release constant buffers
    if (g_vs_cb) { g_vs_cb->Release(); g_vs_cb = nullptr; }
    if (g_ps_cb) { g_ps_cb->Release(); g_ps_cb = nullptr; }

    // Release dynamic vertex buffer
    if (g_dyn_vb) { g_dyn_vb->Release(); g_dyn_vb = nullptr; }
    g_dyn_vb_capacity = 0;

    // Shut down shader subsystem
    GXShutdownShaders();

    // Release core D3D11 objects
    if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
    if (g_depth_buffer) { g_depth_buffer->Release(); g_depth_buffer = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swap_chain) { g_swap_chain->Release(); g_swap_chain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    printf("[D3D11] Shutdown complete.\n");
}

void GXPresent() {
    if (g_swap_chain) {
        g_swap_chain->Present(1, 0); // VSync on

        // Clear for next frame
        float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_context->ClearRenderTargetView(g_rtv, clear_color);
        g_context->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
}

// =============================================================================
// GXDrawPrimitive — Main Draw Call
//
// Assembles the D3D11 pipeline state from the current GXState and issues
// a draw call. This is the core rendering function called by GXEnd() after
// vertex assembly is complete.
//
// Steps:
//   1. Get/compile TEV pixel shader for current state
//   2. Upload vertices to a reusable dynamic vertex buffer
//   3. Set blend, depth-stencil, rasterizer states (cached by config hash)
//   4. Fill constant buffers with matrices + TEV constants
//   5. Set viewport and scissor from GX state
//   6. Bind textures and samplers
//   7. Draw
//
// Reference: libogc GXBegin/GXEnd pipeline; Pureikyubu rendering flow.
// =============================================================================

// --- Constant buffer data layouts (must match HLSL cbuffer declarations) ---

/// VS constant buffer: model-view + projection matrices.
struct VSConstants {
    float model_view[4][4];
    float projection[4][4];
};

/// PS constant buffer: TEV konst colors, color regs, fog, alpha ref.
/// Layout matches cbuffer TevConstants in the generated pixel shader.
struct PSConstants {
    float konst[4][4];      ///< 4 konst color registers (64 bytes)
    float tev_color[4][4];  ///< 4 TEV color registers (64 bytes)
    float fog_params[4];    ///< x=start, y=end, z=1/(end-start), w=type (16 bytes)
    float fog_color[4];     ///< Fog color RGBA (16 bytes)
    float alpha_ref[4];     ///< x=ref0/255, y=ref1/255, z=0, w=0 (16 bytes)
};

// --- FNV-1a hash for pipeline state keys ---

static uint64_t fnv1a_hash(const void* data, size_t size) {
    uint64_t hash = 0x517CC1B727220A95ULL;
    const uint64_t prime = 0x00000100000001B3ULL;
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= prime;
    }
    return hash;
}

// --- Map GX enums to D3D11 ---
// Reference: libogc GX_BL_*, GX_NEVER..GX_ALWAYS; Microsoft D3D11 SDK.

static D3D11_PRIMITIVE_TOPOLOGY map_primitive(uint32_t gx_prim) {
    switch (gx_prim) {
    case GX_TRIANGLES:     return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case GX_TRIANGLESTRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case GX_LINES:         return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case GX_LINESTRIP:     return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case GX_POINTS:        return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    default:               return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static D3D11_BLEND map_blend_factor(GXBlendFactor f) {
    switch (f) {
    case GX_BL_ZERO:        return D3D11_BLEND_ZERO;
    case GX_BL_ONE:         return D3D11_BLEND_ONE;
    case GX_BL_SRCCLR:      return D3D11_BLEND_SRC_COLOR;
    case GX_BL_INVSRCCLR:   return D3D11_BLEND_INV_SRC_COLOR;
    case GX_BL_SRCALPHA:    return D3D11_BLEND_SRC_ALPHA;
    case GX_BL_INVSRCALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
    case GX_BL_DSTALPHA:    return D3D11_BLEND_DEST_ALPHA;
    case GX_BL_INVDSTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    default:                return D3D11_BLEND_ONE;
    }
}

static D3D11_COMPARISON_FUNC map_compare(GXCompare c) {
    switch (c) {
    case GX_NEVER:   return D3D11_COMPARISON_NEVER;
    case GX_LESS:    return D3D11_COMPARISON_LESS;
    case GX_EQUAL:   return D3D11_COMPARISON_EQUAL;
    case GX_LEQUAL:  return D3D11_COMPARISON_LESS_EQUAL;
    case GX_GREATER: return D3D11_COMPARISON_GREATER;
    case GX_NEQUAL:  return D3D11_COMPARISON_NOT_EQUAL;
    case GX_GEQUAL:  return D3D11_COMPARISON_GREATER_EQUAL;
    case GX_ALWAYS:  return D3D11_COMPARISON_ALWAYS;
    default:         return D3D11_COMPARISON_ALWAYS;
    }
}

// --- Primitive conversion for unsupported topologies ---
// D3D11 does not support triangle fans or quads natively.
// Reference: libogc GX_QUADS, GX_TRIANGLEFAN primitive type values.

/// Convert quads to triangle list (2 triangles per quad).
/// Quad order: v0, v1, v2, v3 -> tris (v0,v1,v2), (v0,v2,v3).
static std::vector<float> convert_quads_to_tris(const float* data, uint32_t num_verts, uint32_t fpv) {
    uint32_t num_quads = num_verts / 4;
    std::vector<float> tris;
    tris.reserve(num_quads * 6 * fpv);
    for (uint32_t q = 0; q < num_quads; q++) {
        const float* v0 = data + (q * 4 + 0) * fpv;
        const float* v1 = data + (q * 4 + 1) * fpv;
        const float* v2 = data + (q * 4 + 2) * fpv;
        const float* v3 = data + (q * 4 + 3) * fpv;
        tris.insert(tris.end(), v0, v0 + fpv);
        tris.insert(tris.end(), v1, v1 + fpv);
        tris.insert(tris.end(), v2, v2 + fpv);
        tris.insert(tris.end(), v0, v0 + fpv);
        tris.insert(tris.end(), v2, v2 + fpv);
        tris.insert(tris.end(), v3, v3 + fpv);
    }
    return tris;
}

/// Convert triangle fan to triangle list.
/// Fan order: v0, v1, v2, v3, ... -> tris (v0,v1,v2), (v0,v2,v3), ...
static std::vector<float> convert_fan_to_tris(const float* data, uint32_t num_verts, uint32_t fpv) {
    std::vector<float> tris;
    if (num_verts < 3) return tris;
    tris.reserve((num_verts - 2) * 3 * fpv);
    const float* v0 = data;
    for (uint32_t i = 1; i < num_verts - 1; i++) {
        const float* v1 = data + i * fpv;
        const float* v2 = data + (i + 1) * fpv;
        tris.insert(tris.end(), v0, v0 + fpv);
        tris.insert(tris.end(), v1, v1 + fpv);
        tris.insert(tris.end(), v2, v2 + fpv);
    }
    return tris;
}

// --- Cached pipeline state helpers ---

/// Get or create a blend state. Cached by hash of (mode, src, dst).
/// Reference: libogc GXSetBlendMode; Pureikyubu blending processor docs.
static ID3D11BlendState* get_blend_state(const GXState& state) {
    struct { uint32_t mode, src, dst; } key = {
        (uint32_t)state.blend_mode, (uint32_t)state.src_factor, (uint32_t)state.dst_factor
    };
    uint64_t hash = fnv1a_hash(&key, sizeof(key));
    auto it = g_blend_cache.find(hash);
    if (it != g_blend_cache.end()) return it->second;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (state.blend_mode == GX_BM_BLEND) {
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = map_blend_factor(state.src_factor);
        bd.RenderTarget[0].DestBlend = map_blend_factor(state.dst_factor);
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    } else if (state.blend_mode == GX_BM_SUBTRACT) {
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_REV_SUBTRACT;
    }
    ID3D11BlendState* bs = nullptr;
    g_device->CreateBlendState(&bd, &bs);
    g_blend_cache[hash] = bs;
    return bs;
}

/// Get or create a depth-stencil state. Cached by hash of (enable, func, write).
/// Reference: libogc GXSetZMode; Pureikyubu depth buffer docs.
static ID3D11DepthStencilState* get_depth_state(const GXState& state) {
    struct { uint32_t enable, func, write; } key = {
        state.z_enable ? 1u : 0u, (uint32_t)state.z_func, state.z_write ? 1u : 0u
    };
    uint64_t hash = fnv1a_hash(&key, sizeof(key));
    auto it = g_depth_cache.find(hash);
    if (it != g_depth_cache.end()) return it->second;

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = state.z_enable ? TRUE : FALSE;
    dsd.DepthWriteMask = state.z_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = map_compare(state.z_func);
    dsd.StencilEnable = FALSE;
    ID3D11DepthStencilState* ds = nullptr;
    g_device->CreateDepthStencilState(&dsd, &ds);
    g_depth_cache[hash] = ds;
    return ds;
}

/// Get or create a rasterizer state. Cached by cull_mode.
/// Note: GameCube uses CW front faces; D3D11 FrontCounterClockwise=FALSE
/// matches this convention.
/// Reference: libogc GXSetCullMode; Pureikyubu rasterizer docs.
static ID3D11RasterizerState* get_raster_state(const GXState& state) {
    uint64_t hash = (uint64_t)state.cull_mode;
    auto it = g_raster_cache.find(hash);
    if (it != g_raster_cache.end()) return it->second;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    switch (state.cull_mode) {
    case GX_CULL_NONE:  rd.CullMode = D3D11_CULL_NONE;  break;
    case GX_CULL_FRONT: rd.CullMode = D3D11_CULL_FRONT; break;
    case GX_CULL_BACK:  rd.CullMode = D3D11_CULL_BACK;  break;
    case GX_CULL_ALL:   rd.CullMode = D3D11_CULL_NONE;  break;
    default:            rd.CullMode = D3D11_CULL_NONE;  break;
    }
    ID3D11RasterizerState* rs = nullptr;
    g_device->CreateRasterizerState(&rd, &rs);
    g_raster_cache[hash] = rs;
    return rs;
}

// --- Dynamic vertex buffer management ---

/// Ensure the dynamic vertex buffer can hold at least required_bytes.
/// Recreates with headroom if too small.
static bool ensure_vertex_buffer(uint32_t required_bytes) {
    if (g_dyn_vb && g_dyn_vb_capacity >= required_bytes) return true;
    if (g_dyn_vb) { g_dyn_vb->Release(); g_dyn_vb = nullptr; }

    uint32_t alloc = required_bytes + (required_bytes / 2);
    if (alloc < 4096) alloc = 4096;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = alloc;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = g_device->CreateBuffer(&desc, nullptr, &g_dyn_vb);
    if (FAILED(hr)) return false;
    g_dyn_vb_capacity = alloc;
    return true;
}

/// Upload vertex data into the dynamic vertex buffer via Map/Unmap.
static bool upload_vertices(const float* data, uint32_t num_bytes) {
    if (!ensure_vertex_buffer(num_bytes)) return false;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = g_context->Map(g_dyn_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;
    memcpy(mapped.pData, data, num_bytes);
    g_context->Unmap(g_dyn_vb, 0);
    return true;
}

// --- Constant buffer creation ---

/// Create a D3D11 dynamic constant buffer of the given size.
static ID3D11Buffer* create_const_buffer(uint32_t size) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = (size + 15) & ~15;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* buf = nullptr;
    g_device->CreateBuffer(&desc, nullptr, &buf);
    return buf;
}

/// Update a dynamic constant buffer via Map/Unmap.
static void update_const_buffer(ID3D11Buffer* buf, const void* data, uint32_t size) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = g_context->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, data, size);
        g_context->Unmap(buf, 0);
    }
}

// --- GXDrawPrimitive implementation ---

void GXDrawPrimitive(uint32_t primitive_type, const float* vertex_data,
                     uint32_t num_vertices, uint32_t floats_per_vertex) {
    if (!g_device || !g_context || num_vertices == 0 || !vertex_data) return;

    const GXState& state = GXGetState();

    // Skip draw if GX_CULL_ALL (discard all primitives)
    if (state.cull_mode == GX_CULL_ALL) return;

    // Handle primitives not natively supported by D3D11
    const float* draw_data = vertex_data;
    uint32_t draw_count = num_vertices;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    std::vector<float> converted;

    if (primitive_type == GX_QUADS) {
        converted = convert_quads_to_tris(vertex_data, num_vertices, floats_per_vertex);
        draw_data = converted.data();
        draw_count = (uint32_t)(converted.size() / floats_per_vertex);
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    } else if (primitive_type == GX_TRIANGLEFAN) {
        converted = convert_fan_to_tris(vertex_data, num_vertices, floats_per_vertex);
        draw_data = converted.data();
        draw_count = (uint32_t)(converted.size() / floats_per_vertex);
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    } else {
        topology = map_primitive(primitive_type);
    }

    if (draw_count == 0) return;

    // 1. Get/compile pixel shader for current TEV configuration
    auto* ps = static_cast<ID3D11PixelShader*>(GXGetOrCompileTevShader(state, g_device));
    auto* vs = static_cast<ID3D11VertexShader*>(GXGetVertexShader());
    auto* layout = static_cast<ID3D11InputLayout*>(GXGetInputLayout());
    if (!ps || !vs || !layout) return;

    // 2. Upload vertices to dynamic VB
    uint32_t stride = floats_per_vertex * sizeof(float);
    uint32_t vb_bytes = draw_count * stride;
    if (!upload_vertices(draw_data, vb_bytes)) return;

    // 3. Create constant buffers on first use
    if (!g_vs_cb) g_vs_cb = create_const_buffer(sizeof(VSConstants));
    if (!g_ps_cb) g_ps_cb = create_const_buffer(sizeof(PSConstants));
    if (!g_vs_cb || !g_ps_cb) return;

    // 4. Update VS constant buffer (matrices)
    {
        VSConstants vs_consts;
        GXGetMatrices(vs_consts.model_view, vs_consts.projection);
        update_const_buffer(g_vs_cb, &vs_consts, sizeof(vs_consts));
    }

    // 5. Update PS constant buffer (TEV constants)
    {
        PSConstants ps_consts = {};
        memcpy(ps_consts.konst, state.konst, sizeof(state.konst));
        memcpy(ps_consts.tev_color, state.tev_reg, sizeof(state.tev_reg));
        ps_consts.fog_params[0] = state.fog_start;
        ps_consts.fog_params[1] = state.fog_end;
        float fog_range = state.fog_end - state.fog_start;
        ps_consts.fog_params[2] = (fog_range > 0.0001f) ? (1.0f / fog_range) : 0.0f;
        ps_consts.fog_params[3] = (float)state.fog_type;
        memcpy(ps_consts.fog_color, state.fog_color, sizeof(state.fog_color));
        ps_consts.alpha_ref[0] = state.alpha_ref0 / 255.0f;
        ps_consts.alpha_ref[1] = state.alpha_ref1 / 255.0f;
        update_const_buffer(g_ps_cb, &ps_consts, sizeof(ps_consts));
    }

    // 6. Set input assembler state
    UINT ia_stride = stride;
    UINT ia_offset = 0;
    g_context->IASetInputLayout(layout);
    g_context->IASetVertexBuffers(0, 1, &g_dyn_vb, &ia_stride, &ia_offset);
    g_context->IASetPrimitiveTopology(topology);

    // 7. Set shaders
    g_context->VSSetShader(vs, nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);

    // 8. Bind constant buffers
    g_context->VSSetConstantBuffers(0, 1, &g_vs_cb);
    g_context->PSSetConstantBuffers(0, 1, &g_ps_cb);

    // 9. Set cached pipeline state objects
    ID3D11BlendState* bs = get_blend_state(state);
    float blend_factor[4] = { 1, 1, 1, 1 };
    g_context->OMSetBlendState(bs, blend_factor, 0xFFFFFFFF);

    ID3D11DepthStencilState* ds = get_depth_state(state);
    g_context->OMSetDepthStencilState(ds, 0);

    ID3D11RasterizerState* rs = get_raster_state(state);
    g_context->RSSetState(rs);

    // 10. Set viewport from GX state
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = state.viewport_x;
    vp.TopLeftY = state.viewport_y;
    vp.Width    = state.viewport_w;
    vp.Height   = state.viewport_h;
    vp.MinDepth = state.viewport_near;
    vp.MaxDepth = state.viewport_far;
    g_context->RSSetViewports(1, &vp);

    // 11. Set scissor rect from GX state (default to full window if not set)
    D3D11_RECT scissor;
    if (state.scissor_w > 0 && state.scissor_h > 0) {
        scissor.left   = (LONG)state.scissor_x;
        scissor.top    = (LONG)state.scissor_y;
        scissor.right  = (LONG)(state.scissor_x + state.scissor_w);
        scissor.bottom = (LONG)(state.scissor_y + state.scissor_h);
    } else {
        scissor.left   = 0;
        scissor.top    = 0;
        scissor.right  = (LONG)g_window_width;
        scissor.bottom = (LONG)g_window_height;
    }
    g_context->RSSetScissorRects(1, &scissor);

    // 12. Ensure render targets are bound
    g_context->OMSetRenderTargets(1, &g_rtv, g_dsv);

    // 13. Bind textures (SRVs and samplers for TEV pixel shader)
    GXBindTextures();

    // 14. Draw
    g_context->Draw(draw_count, 0);
}

// =============================================================================
// D3D11 Device Accessors
//
// Allow other modules (e.g., gx_texobj.cpp) to create D3D11 resources
// without including d3d11.h in the public header. Returns raw void*
// that callers cast back to the appropriate COM interface.
// =============================================================================

void* GXGetD3D11Device() {
    return static_cast<void*>(g_device);
}

void* GXGetD3D11Context() {
    return static_cast<void*>(g_context);
}

} // namespace gcrecomp::gx

#endif // _WIN32
