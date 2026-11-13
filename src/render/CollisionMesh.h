#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/World.h"

#include <d3d9.h>

// The collision of one streaming section, turned into something drawable.
//
// Collision has no texture or lighting of its own, so it is drawn as flat
// colour: ground tinted by surface id and shaded by slope so relief reads,
// barriers as red walls, and every polygon outlined so the tessellation --
// which is much coarser than the render mesh -- can be seen.
class CollisionMesh {
public:
    ~CollisionMesh();

    bool Upload(IDirect3DDevice9* device, const nfs::LoadedSection& section);
    void Release();

    // Filled ground and barrier walls, then the outlines. The caller sets the
    // render states for each pass.
    void DrawSurfaces(IDirect3DDevice9* device) const;
    void DrawOutlines(IDirect3DDevice9* device) const;

    const Aabb& Bounds() const { return bounds_; }
    u32 SectionNumber() const  { return sectionNumber_; }
    u32 PolygonCount() const   { return polygonCount_; }
    u32 BarrierCount() const   { return barrierCount_; }

    static constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;

private:
    IDirect3DVertexBuffer9* buffer_ = nullptr;
    u32  surfaceTriangles_ = 0;
    u32  lineStart_        = 0;   // first outline vertex
    u32  lineCount_        = 0;
    u32  sectionNumber_    = 0;
    u32  polygonCount_     = 0;
    u32  barrierCount_     = 0;
    Aabb bounds_;
};
