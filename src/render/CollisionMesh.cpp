#include "render/CollisionMesh.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct CollisionVertex {
    float    x, y, z;
    D3DCOLOR colour;
};

// Collision lies almost exactly on the road surface. Lifting it a hair keeps
// it from z-fighting with the render mesh without visibly floating.
constexpr float kLift = 0.05f;

constexpr D3DCOLOR kBarrierColour   = 0x90FF3030;
constexpr D3DCOLOR kRaceColour      = 0x80FF9A20;   // a race barrier group: absent in free roam
constexpr D3DCOLOR kGroundOutline   = 0xFF101010;
constexpr D3DCOLOR kBarrierOutline  = 0xFFFFD040;
constexpr D3DCOLOR kVerticalOutline = 0xFFFF8040;
constexpr D3DCOLOR kRaceOutline     = 0xFFFFE080;

D3DCOLOR Scale(u32 rgb, float shade, u32 alpha) {
    auto channel = [&](int shift) {
        const float v = (float)((rgb >> shift) & 0xFF) * shade;
        return (u32)(v > 255.0f ? 255.0f : v);
    };
    return D3DCOLOR_ARGB(alpha, channel(16), channel(8), channel(0));
}

} // namespace

u32 CollisionMesh::SurfaceRgb(u8 surface) {
    switch (surface) {
        case 1:  return 0x8C8C96;   // ROAD
        case 2:  return 0x6E7A96;   // ROAD_WET
        case 3: case 4: case 5: case 6: return 0x9C8CB4;   // drift / smoke
        case 7:  return 0x7890B4;   // ROAD_BRIDGE
        case 8:  return 0xA0784A;   // DIRT
        case 9:  return 0xB4A07A;   // GRAVEL
        case 10: return 0x8C7860;   // ROUGH_ROAD
        case 11: return 0xA08C78;   // COBBLESTONE
        case 12: return 0xC8B48C;   // STAIRS
        case 13: return 0x50B4DC;   // PUDDLE
        case 14: return 0x2864DC;   // DEEP_WATER
        case 15: return 0x50B446;   // GRASS
        case 16: return 0xE6DCB4;   // SIDEWALK
        case 17: return 0xC88232;   // WOOD
        case 18: return 0xB45AC8;   // PLASTIC
        case 19: return 0x96E6F0;   // GLASS
        case 20: return 0xC84646;   // SOLID_WALL
        case 21: return 0xDC7878;   // SEE_THROUGH_WALL
        case 22: return 0x3C963C;   // PLANT
        case 23: case 24: return 0xB4B4B4;   // POST, PILLAR
        case 25: case 26: return 0x8CAAC8;   // METAL_GRATE, METAL
        case 27: return 0xA0A0C8;   // CHAINLINK
        default: return 0xF0F040;
    }
}

namespace {

void Push(std::vector<CollisionVertex>& out, const Vec3& p, D3DCOLOR colour) {
    out.push_back({ p.x, p.y, p.z + kLift, colour });
}

} // namespace

CollisionMesh::~CollisionMesh() {
    Release();
}

void CollisionMesh::Release() {
    if (buffer_) { buffer_->Release(); buffer_ = nullptr; }
    surfaceTriangles_ = lineStart_ = lineCount_ = 0;
    polygonCount_ = barrierCount_ = 0;
    bounds_ = {};
}

bool CollisionMesh::Upload(IDirect3DDevice9* device, const nfs::LoadedSection& section,
                           bool showRaceGroups) {
    Release();
    if (section.collision.empty()) return false;

    std::vector<CollisionVertex> surfaces, lines;

    for (const nfs::CollisionPack& pack : section.collision) {
        sectionNumber_ = pack.sectionNumber;
        pack.ResolveBarriers();   // barrier heights, chain by chain

        for (const nfs::CollisionPolygon& polygon : pack.polygons) {
            ++polygonCount_;
            const Vec3* p = polygon.corners;
            const bool race = polygon.raceGroup != 0;
            if (race && !showRaceGroups) continue;

            if (polygon.barrier) {
                ++barrierCount_;
                Vec3 wall[4];
                pack.BarrierQuad(polygon, wall);
                const Vec3 &a0 = wall[0], &a1 = wall[1], &b1 = wall[2], &b0 = wall[3];

                const D3DCOLOR fill = race ? kRaceColour : kBarrierColour;
                const D3DCOLOR edge = race ? kRaceOutline : kBarrierOutline;
                for (const Vec3& v : { a0, a1, b1, a0, b1, b0 }) Push(surfaces, v, fill);
                for (const Vec3& v : { a1, b1, a0, b0 })         Push(lines, v, edge);

                bounds_.Add(a0); bounds_.Add(a1); bounds_.Add(b0); bounds_.Add(b1);
                continue;
            }

            // Shaded by slope against a light from above and to one side, so a
            // ramp or a camber stands out from the flat road around it.
            const Vec3 normal = Normalize(Cross(p[1] - p[0], p[2] - p[0]));
            const Vec3 light  = Normalize(Vec3{ 0.4f, 0.3f, 1.0f });
            const float shade = 0.45f + 0.55f * std::fabs(Dot(normal, light));
            const D3DCOLOR colour = race ? kRaceColour
                                  : Scale(SurfaceRgb(polygon.surface), shade, polygon.Vertical() ? 0xC0 : 0xA0);
            const D3DCOLOR outline = race ? kRaceOutline
                                   : polygon.Vertical() ? kVerticalOutline : kGroundOutline;

            for (int c : { 0, 1, 2 }) Push(surfaces, p[c], colour);
            if (polygon.quad)
                for (int c : { 0, 2, 3 }) Push(surfaces, p[c], colour);

            const int corners = polygon.quad ? 4 : 3;
            for (int c = 0; c < corners; ++c) {
                Push(lines, p[c], outline);
                Push(lines, p[(c + 1) % corners], outline);
                bounds_.Add(p[c]);
            }
        }
    }

    if (surfaces.empty()) return false;

    surfaceTriangles_ = (u32)(surfaces.size() / 3);
    lineStart_        = (u32)surfaces.size();
    lineCount_        = (u32)(lines.size() / 2);
    surfaces.insert(surfaces.end(), lines.begin(), lines.end());

    const UINT bytes = (UINT)(surfaces.size() * sizeof(CollisionVertex));
    if (FAILED(device->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED,
                                          &buffer_, nullptr)))
        return false;

    void* target = nullptr;
    if (FAILED(buffer_->Lock(0, bytes, &target, 0))) {
        Release();
        return false;
    }
    std::memcpy(target, surfaces.data(), bytes);
    buffer_->Unlock();
    return true;
}

void CollisionMesh::DrawSurfaces(IDirect3DDevice9* device) const {
    if (!buffer_ || !surfaceTriangles_) return;
    device->SetStreamSource(0, buffer_, 0, sizeof(CollisionVertex));
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, surfaceTriangles_);
}

void CollisionMesh::DrawOutlines(IDirect3DDevice9* device) const {
    if (!buffer_ || !lineCount_) return;
    device->SetStreamSource(0, buffer_, 0, sizeof(CollisionVertex));
    device->DrawPrimitive(D3DPT_LINELIST, lineStart_, lineCount_);
}
