// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// GX Texture Object Management
//
// Implements GXInitTexObj, GXLoadTexObj, and GXBindTextures. Connects the
// texture decoder (gx_texture.cpp) to D3D11 shader resource views so that
// TEV stages can sample textures via the pixel shader.
//
// The GameCube supports 8 texture map slots (GX_TEXMAP0..7). Games set up
// a GXTexObj with format/dimensions/filtering, then call GXLoadTexObj to
// bind it to a slot. At draw time, GXBindTextures sets the corresponding
// D3D11 SRVs and sampler states on the pixel shader.
//
// A texture cache keyed by (data_ptr, width, height, format) avoids
// redundant decoding and D3D11 resource creation when the same texture
// is loaded multiple times.
//
// References:
//   - libogc — GXInitTexObj, GXLoadTexObj signatures and semantics
//   - Pureikyubu — Texture unit register documentation
//   - Microsoft D3D11 SDK — Texture2D, SRV, and sampler state creation
// =============================================================================

#ifdef _WIN32

#include "gcrecomp/gx/gx.h"
#include <cstdio>
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

namespace gcrecomp::gx {

// Forward declaration of the texture decoder from gx_texture.cpp
std::vector<uint8_t> decode_texture(const uint8_t* src, uint32_t width, uint32_t height, GXTexFmt fmt);

// =============================================================================
// Texture Cache
//
// Avoids re-decoding and re-uploading textures that have already been seen.
// Keyed by the combination of data pointer, dimensions, and format.
// =============================================================================

struct TextureCacheKey {
    uintptr_t data_ptr;
    uint32_t  width;
    uint32_t  height;
    uint32_t  format;

    bool operator==(const TextureCacheKey& o) const {
        return data_ptr == o.data_ptr && width == o.width &&
               height == o.height && format == o.format;
    }
};

struct TextureCacheKeyHash {
    size_t operator()(const TextureCacheKey& k) const {
        // FNV-1a hash over the key fields
        size_t h = 14695981039346656037ULL;
        auto mix = [&](uint64_t v) {
            h ^= v;
            h *= 1099511628211ULL;
        };
        mix(k.data_ptr);
        mix(k.width);
        mix(k.height);
        mix(k.format);
        return h;
    }
};

struct CachedTexture {
    ID3D11ShaderResourceView* srv;
    ID3D11Texture2D*          texture;
};

static std::unordered_map<TextureCacheKey, CachedTexture, TextureCacheKeyHash> g_texture_cache;

// =============================================================================
// Per-slot state: 8 texture map slots, each with an SRV and sampler
// =============================================================================

static constexpr uint32_t MAX_TEX_MAPS = 8;

struct TexMapSlot {
    ID3D11ShaderResourceView* srv;
    ID3D11SamplerState*       sampler;
};

static TexMapSlot g_tex_maps[MAX_TEX_MAPS] = {};

// =============================================================================
// Helper: convert GX wrap mode to D3D11 texture address mode
// =============================================================================

static D3D11_TEXTURE_ADDRESS_MODE wrap_to_d3d(uint32_t wrap) {
    switch (wrap) {
    case 0:  return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

// =============================================================================
// Helper: convert GX min/mag filter settings to a D3D11 filter enum
//
// GX min filter values:
//   0 = near, 1 = linear,
//   4 = near_mip_near, 5 = lin_mip_near,
//   6 = near_mip_lin,  7 = lin_mip_lin
// GX mag filter values:
//   0 = near, 1 = linear
// =============================================================================

static D3D11_FILTER filters_to_d3d(uint32_t min_filter, uint32_t mag_filter) {
    bool mag_linear = (mag_filter == 1);
    bool min_linear = false;
    bool mip_linear = false;

    switch (min_filter) {
    case 0: min_linear = false; mip_linear = false; break; // near
    case 1: min_linear = true;  mip_linear = false; break; // linear
    case 4: min_linear = false; mip_linear = false; break; // near_mip_near
    case 5: min_linear = true;  mip_linear = false; break; // lin_mip_near
    case 6: min_linear = false; mip_linear = true;  break; // near_mip_lin
    case 7: min_linear = true;  mip_linear = true;  break; // lin_mip_lin
    default: min_linear = true; mip_linear = false; break;
    }

    // Encode as D3D11_FILTER using the MIN_MAG_MIP pattern:
    //   D3D11_FILTER = (min << 4) | (mag << 2) | mip
    //   where 0 = POINT, 1 = LINEAR for each component
    uint32_t f = 0;
    if (min_linear) f |= D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT; // bit 4
    if (mag_linear) f |= D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT; // bit 2
    if (mip_linear) f |= D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR; // bit 0

    return static_cast<D3D11_FILTER>(f);
}

// =============================================================================
// Helper: create a D3D11 sampler state from GXTexObj wrap/filter settings
// =============================================================================

static ID3D11SamplerState* create_sampler(ID3D11Device* device, const GXTexObj* obj) {
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = filters_to_d3d(obj->min_filter, obj->mag_filter);
    sd.AddressU = wrap_to_d3d(obj->wrap_s);
    sd.AddressV = wrap_to_d3d(obj->wrap_t);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    ID3D11SamplerState* sampler = nullptr;
    HRESULT hr = device->CreateSamplerState(&sd, &sampler);
    if (FAILED(hr)) {
        fprintf(stderr, "[GX TexObj] Failed to create sampler state: 0x%08lX\n", hr);
    }
    return sampler;
}

// =============================================================================
// GXInitTexObj — Fill a GXTexObj struct with texture parameters.
//
// Does not create any D3D11 resources; that happens lazily in GXLoadTexObj.
// Default filters: min = linear (1), mag = linear (1).
// =============================================================================

void GXInitTexObj(GXTexObj* obj, const void* data, uint16_t width, uint16_t height,
                  GXTexFmt format, uint32_t wrap_s, uint32_t wrap_t, bool mipmap) {
    if (!obj) return;
    memset(obj, 0, sizeof(*obj));
    obj->data       = static_cast<const uint8_t*>(data);
    obj->width      = width;
    obj->height     = height;
    obj->format     = format;
    obj->wrap_s     = wrap_s;
    obj->wrap_t     = wrap_t;
    obj->mipmap     = mipmap;
    obj->min_filter = 1; // linear
    obj->mag_filter = 1; // linear
}

// =============================================================================
// GXInitTexObjCI — Initialize a color-indexed (paletted) texture object.
//
// Same as GXInitTexObj but for CI formats (C4, C8, C14X2). The tlut
// parameter selects the TLUT to use for palette lookup (not yet
// implemented; the texture decoder currently does not support CI formats).
// =============================================================================

void GXInitTexObjCI(GXTexObj* obj, const void* data, uint16_t width, uint16_t height,
                    GXTexFmt format, uint32_t wrap_s, uint32_t wrap_t, bool mipmap, uint32_t tlut) {
    // Initialize the same way; TLUT handling is a future extension
    GXInitTexObj(obj, data, width, height, format, wrap_s, wrap_t, mipmap);
    // TODO: store tlut index when CI format decoding is implemented
    (void)tlut;
}

// =============================================================================
// GXLoadTexObj — Decode texture data and bind to a map slot.
//
// Checks the texture cache first. On cache miss, decodes the GameCube
// tiled texture to RGBA8, creates a D3D11 Texture2D and SRV, and stores
// the result in the cache. Also creates a sampler state matching the
// texture's wrap/filter settings and assigns both to the requested slot.
// =============================================================================

void GXLoadTexObj(const GXTexObj* obj, uint32_t map_id) {
    if (!obj || map_id >= MAX_TEX_MAPS) return;

    auto* device = static_cast<ID3D11Device*>(GXGetD3D11Device());
    if (!device) {
        fprintf(stderr, "[GX TexObj] D3D11 device not available\n");
        return;
    }

    // Build cache key
    TextureCacheKey key;
    key.data_ptr = reinterpret_cast<uintptr_t>(obj->data);
    key.width    = obj->width;
    key.height   = obj->height;
    key.format   = static_cast<uint32_t>(obj->format);

    // Look up cache
    ID3D11ShaderResourceView* srv = nullptr;
    auto it = g_texture_cache.find(key);
    if (it != g_texture_cache.end()) {
        srv = it->second.srv;
    } else {
        // Cache miss: decode and upload
        if (!obj->data) {
            fprintf(stderr, "[GX TexObj] Null texture data for map %u\n", map_id);
            return;
        }

        std::vector<uint8_t> rgba_data = decode_texture(obj->data, obj->width, obj->height, obj->format);

        // Create D3D11 Texture2D
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width            = obj->width;
        desc.Height           = obj->height;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem     = rgba_data.data();
        init.SysMemPitch  = obj->width * 4;

        ID3D11Texture2D* texture = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, &init, &texture);
        if (FAILED(hr)) {
            fprintf(stderr, "[GX TexObj] Failed to create Texture2D (%ux%u fmt=0x%X): 0x%08lX\n",
                    obj->width, obj->height, obj->format, hr);
            return;
        }

        // Create shader resource view
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels       = 1;

        hr = device->CreateShaderResourceView(texture, &srv_desc, &srv);
        if (FAILED(hr)) {
            fprintf(stderr, "[GX TexObj] Failed to create SRV: 0x%08lX\n", hr);
            texture->Release();
            return;
        }

        // Store in cache
        g_texture_cache[key] = { srv, texture };
    }

    // Release previous sampler for this slot (samplers are not cached since
    // the same texture data can be loaded with different filter settings)
    if (g_tex_maps[map_id].sampler) {
        g_tex_maps[map_id].sampler->Release();
        g_tex_maps[map_id].sampler = nullptr;
    }

    // Create sampler state for this texture's wrap/filter settings
    ID3D11SamplerState* sampler = create_sampler(device, obj);

    // Assign to slot
    g_tex_maps[map_id].srv     = srv;
    g_tex_maps[map_id].sampler = sampler;
}

// =============================================================================
// GXSetTexObjMinFilt / GXSetTexObjMagFilt — Update filter modes on a GXTexObj.
//
// These modify the texture object before it is loaded via GXLoadTexObj.
// The sampler state is created at load time, so changes here only take
// effect on the next GXLoadTexObj call.
// =============================================================================

void GXSetTexObjMinFilt(GXTexObj* obj, uint32_t filter) {
    if (obj) obj->min_filter = filter;
}

void GXSetTexObjMagFilt(GXTexObj* obj, uint32_t filter) {
    if (obj) obj->mag_filter = filter;
}

// =============================================================================
// GXInvalidateTexAll — Flush the entire texture cache.
//
// Releases all cached D3D11 textures and SRVs. Also clears all map slots.
// Called when the game invalidates texture memory (e.g., after loading
// new texture data into the same addresses).
// =============================================================================

void GXInvalidateTexAll() {
    // Release all cached textures
    for (auto& [key, cached] : g_texture_cache) {
        if (cached.srv) cached.srv->Release();
        if (cached.texture) cached.texture->Release();
    }
    g_texture_cache.clear();

    // Clear all map slots
    for (uint32_t i = 0; i < MAX_TEX_MAPS; i++) {
        g_tex_maps[i].srv = nullptr;
        if (g_tex_maps[i].sampler) {
            g_tex_maps[i].sampler->Release();
            g_tex_maps[i].sampler = nullptr;
        }
    }
}

// =============================================================================
// GXBindTextures — Bind all 8 texture map SRVs and samplers to the pixel shader.
//
// Called by GXDrawPrimitive before issuing draw calls. Sets shader resource
// views on pixel shader slots t0..t7 and sampler states on s0..s7.
// Slots without a loaded texture get nullptr (D3D11 will return 0 on sample).
// =============================================================================

void GXBindTextures() {
    auto* context = static_cast<ID3D11DeviceContext*>(GXGetD3D11Context());
    if (!context) return;

    ID3D11ShaderResourceView* srvs[MAX_TEX_MAPS];
    ID3D11SamplerState*       samplers[MAX_TEX_MAPS];

    for (uint32_t i = 0; i < MAX_TEX_MAPS; i++) {
        srvs[i]     = g_tex_maps[i].srv;
        samplers[i]  = g_tex_maps[i].sampler;
    }

    context->PSSetShaderResources(0, MAX_TEX_MAPS, srvs);
    context->PSSetSamplers(0, MAX_TEX_MAPS, samplers);
}

} // namespace gcrecomp::gx

#endif // _WIN32
