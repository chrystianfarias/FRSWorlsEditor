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

constexpr D3DCOLOR kBarrierColour = 0x90FF3030;
constexpr D3DCOLOR kGroundOutline = 0xFF101010;
constexpr D3DCOLOR kBarrierOutline = 0xFFFFD040;

// A stable, well spread colour for a surface id, so neighbouring materials
// can be told apart. Kept away from red, which belongs to the barriers.
D3DCOLOR SurfaceColour(u8 surface, float shade) {
    const float hue = std::fmod(0.15f + (float)surface * 0.618034f, 1.0f) * 0.8f + 0.1f;
    auto channel = [&](float offset) {
        float t = std::fabs(std::fmod(hue * 6.0f + offset, 6.0f) - 3.0f) - 1.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return (u32)((0.35f + 0.65f * t) * shade * 255.0f);
    };
    return D3DCOLOR_ARGB(0xA0, channel(0.0f), channel(4.0f), channel(2.0f));
}

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

bool CollisionMesh::Upload(IDirect3DDevice9* device, const nfs::LoadedSection& section) {
    Release();
    if (section.collision.empty()) return false;

    std::vector<CollisionVertex> surfaces, lines;

    for (const nfs::CollisionPack& pack : section.collision) {
        sectionNumber_ = pack.sectionNumber;

        for (const nfs::CollisionPolygon& polygon : pack.polygons) {
            ++polygonCount_;
            const Vec3* p = polygon.corners;

            if (polygon.barrier) {
                ++barrierCount_;
                Vec3 wall[4];
                pack.BarrierQuad(polygon, wall);
                const Vec3 &a0 = wall[0], &a1 = wall[1], &b1 = wall[2], &b0 = wall[3];

                for (const Vec3& v : { a0, a1, b1, a0, b1, b0 }) Push(surfaces, v, kBarrierColour);
                for (const Vec3& v : { a1, b1, a0, b0 })         Push(lines, v, kBarrierOutline);

                bounds_.Add(a0); bounds_.Add(a1); bounds_.Add(b0); bounds_.Add(b1);
                continue;
            }

            // Shaded by slope against a light from above and to one side, so a
            // ramp or a camber stands out from the flat road around it.
            const Vec3 normal = Normalize(Cross(p[1] - p[0], p[2] - p[0]));
            const Vec3 light  = Normalize(Vec3{ 0.4f, 0.3f, 1.0f });
            const float shade = 0.45f + 0.55f * std::fabs(Dot(normal, light));
            const D3DCOLOR colour = SurfaceColour(polygon.surface, shade);

            for (int c : { 0, 1, 2 }) Push(surfaces, p[c], colour);
            if (polygon.quad)
                for (int c : { 0, 2, 3 }) Push(surfaces, p[c], colour);

            const int corners = polygon.quad ? 4 : 3;
            for (int c = 0; c < corners; ++c) {
                Push(lines, p[c], kGroundOutline);
                Push(lines, p[(c + 1) % corners], kGroundOutline);
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
