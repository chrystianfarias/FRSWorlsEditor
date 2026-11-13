#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/World.h"

#include <d3d9.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

// The vertex layout the editor draws with. It is identical to nfs::SolidVertex,
// so parsed geometry is uploaded without any repacking.
constexpr DWORD kWorldVertexFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// One material range of one object: the smallest unit the renderer draws.
struct GpuDraw {
    u32 startIndex     = 0;
    u32 primitiveCount = 0;
    u32 minVertex      = 0;
    u32 vertexCount    = 0;
    u32 textureHash    = 0;
};

// One solid object inside a section's shared buffers.
struct GpuObject {
    const nfs::SolidObject* source = nullptr;
    Mat4                 transform;
    Aabb                 worldBounds;
    bool                 isBackdrop = false;
    // Shown by the game only during the races that use it: the track
    // barriers (XO_TRACKBARRIER*) and the start / finish lines (ZPM_<race>_*).
    bool                 raceProp = false;
    u32                  baseVertex = 0;
    std::vector<GpuDraw> draws;
};

// The geometry of one streaming section, merged into a single vertex and index
// buffer. Merging keeps the number of buffer binds down: a section holds a few
// hundred objects that would otherwise each cost two SetStream calls per frame.
class GpuSection {
public:
    ~GpuSection();

    bool Upload(IDirect3DDevice9* device, const nfs::LoadedSection& section);
    void Release();

    IDirect3DVertexBuffer9* VertexBuffer() const { return vertexBuffer_; }
    IDirect3DIndexBuffer9*  IndexBuffer()  const { return indexBuffer_; }

    const std::vector<GpuObject>& Objects() const { return objects_; }
    const Aabb& Bounds() const { return bounds_; }

    u32 VertexCount()   const { return vertexCount_; }
    u32 TriangleCount() const { return triangleCount_; }

private:
    IDirect3DVertexBuffer9* vertexBuffer_ = nullptr;
    IDirect3DIndexBuffer9*  indexBuffer_  = nullptr;
    std::vector<GpuObject>  objects_;
    Aabb                    bounds_;
    u32                     vertexCount_   = 0;
    u32                     triangleCount_ = 0;
};

// How a material is drawn, decided by its texture's header.
enum class SurfaceBlend : u8 {
    Opaque,     // alpha ignored
    CutOut,     // alpha tested, written to depth with the opaque surfaces
    Blend,      // alpha blended, after everything opaque, back to front
    Additive,   // added to what is behind it, so black is invisible
};

struct CachedTexture {
    IDirect3DTexture9* texture = nullptr;   // nullptr marks a known miss
    SurfaceBlend       blend   = SurfaceBlend::Opaque;
    // Scrolling (nfs::Texture::scrollType): 0 none, 1 smooth, 2 in steps.
    u8    scrollType = 0;
    float scrollStep = 1.0f, scrollS = 0.0f, scrollT = 0.0f;

    bool Translucent() const { return blend == SurfaceBlend::Blend || blend == SurfaceBlend::Additive; }
};

// Creates Direct3D textures on demand from the packs a world has loaded, keyed
// by the hash the materials reference. Misses are remembered so a missing
// texture is not looked up again every frame.
class TextureCache {
public:
    ~TextureCache();

    void SetWorld(const nfs::World* world) { world_ = world; Clear(); }
    void Clear();

    // Where to look for a hash the world does not know: an extension's own
    // packs, which are not part of any region. Kept across SetWorld.
    using ExtraLookup = std::function<const nfs::Texture*(u32)>;
    void SetExtraLookup(ExtraLookup lookup) { extra_ = std::move(lookup); Clear(); }

    // `texture` is nullptr when the hash is unknown or the format is unsupported.
    const CachedTexture& Get(IDirect3DDevice9* device, u32 hash);

    size_t ResidentCount() const { return textures_.size(); }

private:
    IDirect3DTexture9* Create(IDirect3DDevice9* device, const nfs::Texture& texture);

    const nfs::World* world_ = nullptr;
    ExtraLookup extra_;
    std::unordered_map<u32, CachedTexture> textures_;
};
