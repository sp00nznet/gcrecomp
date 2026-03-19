// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// D3D11 Shader Compilation and Caching
//
// Compiles TEV-generated HLSL shaders at runtime using D3DCompile.
// Caches compiled pixel shaders by TEV configuration hash (from
// hash_tev_state) to avoid redundant compilation. The vertex shader
// and input layout are compiled once at init time.
//
// The vertex shader accepts all 8 GameCube texture coordinates plus
// position, normal, and two vertex colors (30 floats = 120 bytes stride).
//
// References:
//   - GameCubeRecompiled — Runtime shader generation and caching approach
//   - Microsoft D3D11 SDK — D3DCompile API and shader model documentation
//   - libogc — Vertex attribute layout (position, normal, colors, tex coords)
// =============================================================================

#ifdef _WIN32

#include "gcrecomp/gx/gx.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace gcrecomp::gx {

// =============================================================================
// Vertex shader source — accepts all GameCube vertex attributes
//
// The input layout matches the assembled vertex data from GXDrawPrimitive:
//   POSITION  float3  offset  0  (12 bytes)
//   NORMAL    float3  offset 12  (12 bytes)
//   COLOR0    float4  offset 24  (16 bytes)
//   COLOR1    float4  offset 40  (16 bytes)
//   TEXCOORD0 float2  offset 56  ( 8 bytes)
//   TEXCOORD1 float2  offset 64  ( 8 bytes)
//   TEXCOORD2 float2  offset 72  ( 8 bytes)
//   TEXCOORD3 float2  offset 80  ( 8 bytes)
//   TEXCOORD4 float2  offset 88  ( 8 bytes)
//   TEXCOORD5 float2  offset 96  ( 8 bytes)
//   TEXCOORD6 float2  offset 104 ( 8 bytes)
//   TEXCOORD7 float2  offset 112 ( 8 bytes)
//   Total stride = 120 bytes (30 floats)
//
// Reference: libogc vertex attribute order; Pureikyubu CP register docs.
// =============================================================================

static const char* g_vs_source = R"(
struct VSInput {
    float3 position  : POSITION;
    float3 normal    : NORMAL;
    float4 color0    : COLOR0;
    float4 color1    : COLOR1;
    float2 texcoord0 : TEXCOORD0;
    float2 texcoord1 : TEXCOORD1;
    float2 texcoord2 : TEXCOORD2;
    float2 texcoord3 : TEXCOORD3;
    float2 texcoord4 : TEXCOORD4;
    float2 texcoord5 : TEXCOORD5;
    float2 texcoord6 : TEXCOORD6;
    float2 texcoord7 : TEXCOORD7;
};

struct VSOutput {
    float4 position  : SV_POSITION;
    float4 color0    : COLOR0;
    float4 color1    : COLOR1;
    float2 texcoord0 : TEXCOORD0;
    float2 texcoord1 : TEXCOORD1;
    float2 texcoord2 : TEXCOORD2;
    float2 texcoord3 : TEXCOORD3;
    float2 texcoord4 : TEXCOORD4;
    float2 texcoord5 : TEXCOORD5;
    float2 texcoord6 : TEXCOORD6;
    float2 texcoord7 : TEXCOORD7;
};

cbuffer Matrices : register(b0) {
    row_major float4x4 model_view;
    row_major float4x4 projection;
};

VSOutput main(VSInput input) {
    VSOutput output;
    float4 world_pos = mul(model_view, float4(input.position, 1.0));
    output.position  = mul(projection, world_pos);
    output.color0    = input.color0;
    output.color1    = input.color1;
    output.texcoord0 = input.texcoord0;
    output.texcoord1 = input.texcoord1;
    output.texcoord2 = input.texcoord2;
    output.texcoord3 = input.texcoord3;
    output.texcoord4 = input.texcoord4;
    output.texcoord5 = input.texcoord5;
    output.texcoord6 = input.texcoord6;
    output.texcoord7 = input.texcoord7;
    return output;
}
)";

// =============================================================================
// Shader cache and compiled resources
// =============================================================================

/// Cached pixel shaders keyed by TEV state hash (from hash_tev_state).
static std::unordered_map<uint64_t, ID3D11PixelShader*> g_ps_cache;

/// Compiled vertex shader (created once at init).
static ID3D11VertexShader* g_vertex_shader = nullptr;

/// Vertex input layout matching the VSInput struct above.
static ID3D11InputLayout* g_input_layout = nullptr;

/// Retained vertex shader bytecode blob (needed for input layout creation).
static ID3DBlob* g_vs_blob = nullptr;

// =============================================================================
// Internal helpers
// =============================================================================

/// Compile an HLSL shader from source using D3DCompile.
/// @param source HLSL source code string
/// @param target Shader model target (e.g., "vs_5_0", "ps_5_0")
/// @param blob_out Receives the compiled bytecode blob on success
/// @param error_out Receives the error message blob on failure
/// @return true on success
static bool compile_shader(const std::string& source, const char* target,
                           ID3DBlob** blob_out, ID3DBlob** error_out) {
    HRESULT hr = D3DCompile(
        source.c_str(), source.size(),
        nullptr, nullptr, nullptr,
        "main", target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        blob_out, error_out
    );
    if (FAILED(hr)) {
        if (error_out && *error_out) {
            fprintf(stderr, "[D3D11] Shader compile error: %s\n",
                    (const char*)(*error_out)->GetBufferPointer());
        }
        return false;
    }
    return true;
}

// =============================================================================
// Public API
// =============================================================================

/// Initialize the D3D11 shader subsystem.
/// Compiles the vertex shader and creates the matching input layout.
/// Must be called once during GXInitBackend before any draw calls.
///
/// The input layout defines how assembled vertex data maps to the vertex
/// shader's VSInput struct. All 8 texture coordinates are included even
/// if a particular draw call does not use them all — unused slots contain
/// zeros from the vertex assembler.
///
/// Reference: Microsoft D3D11 SDK — ID3D11Device::CreateInputLayout.
bool GXInitShaders(void* device_ptr) {
    if (!device_ptr) return false;
    ID3D11Device* device = static_cast<ID3D11Device*>(device_ptr);

    // Compile vertex shader
    ID3DBlob* error_blob = nullptr;
    if (!compile_shader(g_vs_source, "vs_5_0", &g_vs_blob, &error_blob)) {
        fprintf(stderr, "[D3D11] Failed to compile vertex shader\n");
        if (error_blob) error_blob->Release();
        return false;
    }
    if (error_blob) { error_blob->Release(); error_blob = nullptr; }

    HRESULT hr = device->CreateVertexShader(
        g_vs_blob->GetBufferPointer(), g_vs_blob->GetBufferSize(),
        nullptr, &g_vertex_shader
    );
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] Failed to create vertex shader: 0x%08lX\n", hr);
        return false;
    }

    // Create input layout matching VSInput and the 120-byte vertex stride.
    // Reference: libogc vertex attribute order (pos, nrm, clr0, clr1, tex0-7).
    D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT,    0,   0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",    0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",     0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",     1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,       0,  56, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  1, DXGI_FORMAT_R32G32_FLOAT,       0,  64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  2, DXGI_FORMAT_R32G32_FLOAT,       0,  72, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  3, DXGI_FORMAT_R32G32_FLOAT,       0,  80, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  4, DXGI_FORMAT_R32G32_FLOAT,       0,  88, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  5, DXGI_FORMAT_R32G32_FLOAT,       0,  96, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  6, DXGI_FORMAT_R32G32_FLOAT,       0, 104, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  7, DXGI_FORMAT_R32G32_FLOAT,       0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = device->CreateInputLayout(
        layout_desc, _countof(layout_desc),
        g_vs_blob->GetBufferPointer(), g_vs_blob->GetBufferSize(),
        &g_input_layout
    );
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] Failed to create input layout: 0x%08lX\n", hr);
        return false;
    }

    printf("[D3D11] Shaders initialized (VS compiled, input layout created)\n");
    return true;
}

/// Shut down the shader subsystem and release all resources.
/// Releases the vertex shader, input layout, VS blob, and all cached
/// pixel shaders.
void GXShutdownShaders() {
    for (auto& pair : g_ps_cache) {
        if (pair.second) pair.second->Release();
    }
    g_ps_cache.clear();

    if (g_input_layout)  { g_input_layout->Release();  g_input_layout  = nullptr; }
    if (g_vertex_shader) { g_vertex_shader->Release(); g_vertex_shader = nullptr; }
    if (g_vs_blob)       { g_vs_blob->Release();       g_vs_blob       = nullptr; }

    printf("[D3D11] Shaders shut down (%zu cached pixel shaders released)\n",
           g_ps_cache.size());
}

/// Get or compile a pixel shader for the current TEV state.
/// The TEV state is hashed using hash_tev_state() and used as the cache
/// key. On cache miss, generate_tev_shader() produces HLSL source which
/// is compiled with D3DCompile at shader model 5.0.
///
/// Reference: GameCubeRecompiled — runtime TEV shader compilation and caching.
void* GXGetOrCompileTevShader(const GXState& state, void* device_ptr) {
    ID3D11Device* device = static_cast<ID3D11Device*>(device_ptr);
    uint64_t hash = hash_tev_state(state);

    // Cache lookup
    auto it = g_ps_cache.find(hash);
    if (it != g_ps_cache.end()) {
        return it->second;
    }

    // Cache miss — generate HLSL and compile
    std::string hlsl = generate_tev_shader(state);

    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    if (!compile_shader(hlsl, "ps_5_0", &ps_blob, &error_blob)) {
        fprintf(stderr, "[D3D11] Failed to compile TEV pixel shader (hash=0x%016llX)\n",
                (unsigned long long)hash);
        if (error_blob) error_blob->Release();
        // Cache nullptr to avoid repeated compilation attempts
        g_ps_cache[hash] = nullptr;
        return nullptr;
    }
    if (error_blob) { error_blob->Release(); error_blob = nullptr; }

    ID3D11PixelShader* ps = nullptr;
    HRESULT hr = device->CreatePixelShader(
        ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
        nullptr, &ps
    );
    ps_blob->Release();

    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] Failed to create pixel shader: 0x%08lX\n", hr);
        g_ps_cache[hash] = nullptr;
        return nullptr;
    }

    g_ps_cache[hash] = ps;
    printf("[D3D11] Compiled TEV pixel shader (hash=0x%016llX, cache size=%zu)\n",
           (unsigned long long)hash, g_ps_cache.size());
    return ps;
}

/// Get the compiled vertex shader (created during GXInitShaders).
void* GXGetVertexShader() {
    return g_vertex_shader;
}

/// Get the vertex input layout (created during GXInitShaders).
void* GXGetInputLayout() {
    return g_input_layout;
}

} // namespace gcrecomp::gx

#endif // _WIN32
