// Port of NFSU2Unity Core/World/TrackCollisionU2.cs.
#include "drive/world/TrackCollisionU2.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace bb {

namespace {

template <class T> T Rd(const uint8_t* b, size_t o) { T v; std::memcpy(&v, b + o, sizeof(T)); return v; }

struct SubChunk {
    uint32_t id = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

} // namespace

int TrackCollisionU2::BarrierCount() const
{
    int n = 0;
    for (const auto& p : Polygons) if (p.IsBarrier) n++;
    return n;
}

TrackCollisionU2 TrackCollisionU2::Read(const uint8_t* chunk, size_t size)
{
    if (chunk == nullptr || size < 8 || Rd<uint32_t>(chunk, 0) != WorldChunks::Collision)
        throw std::runtime_error("not a collision pack (0x80034130)");
    size_t end = 8 + static_cast<size_t>(Rd<uint32_t>(chunk, 4));
    if (end > size) throw std::runtime_error("collision pack overruns its buffer");
    SubChunk header, polys, refs, cells;
    bool hasHeader = false, hasPolys = false, hasRefs = false, hasCells = false;
    for (size_t p = 8; p + 8 <= end;) {
        uint32_t id = Rd<uint32_t>(chunk, p);
        size_t sz = Rd<uint32_t>(chunk, p + 4);
        if (p + 8 + sz > end) throw std::runtime_error("collision chunk overruns its pack");
        SubChunk s{ id, chunk + p + 8, sz };
        // Chunk.Find: the first child with the id
        if (id == WorldChunks::CollisionHeader && !hasHeader) { header = s; hasHeader = true; }
        else if (id == WorldChunks::CollisionPolygons && !hasPolys) { polys = s; hasPolys = true; }
        else if (id == WorldChunks::CollisionCellRefs && !hasRefs) { refs = s; hasRefs = true; }
        else if (id == WorldChunks::CollisionCells && !hasCells) { cells = s; hasCells = true; }
        p += 8 + sz;
    }
    if (!hasHeader) throw std::runtime_error("collision pack without header 0x00034131");
    if (header.size < 0x2C) throw std::runtime_error("collision header too short");

    TrackCollisionU2 c;
    c.SectionNumber = Rd<int32_t>(header.data, 0);
    c.GridWidth = Rd<uint16_t>(header.data, 4);
    c.GridHeight = Rd<uint16_t>(header.data, 6);
    c.OriginX = Rd<float>(header.data, 8);
    c.OriginY = Rd<float>(header.data, 12);
    c.BoundsMin = Vec3(Rd<float>(header.data, 0x10), Rd<float>(header.data, 0x14), Rd<float>(header.data, 0x18));
    c.BoundsMax = Vec3(Rd<float>(header.data, 0x20), Rd<float>(header.data, 0x24), Rd<float>(header.data, 0x28));

    if (hasPolys) {
        c.RawPolygons.assign(polys.data, polys.data + polys.size);
        size_t n = polys.size / CollisionPolygonU2::Size;
        c.Polygons.resize(n);
        for (size_t i = 0; i < n; i++) {
            const uint8_t* r = polys.data + i * CollisionPolygonU2::Size;
            CollisionPolygonU2 p;
            p.Attr0 = r[0]; p.CornerCode = r[1]; p.Attr2 = r[2]; p.Flags = r[3];
            p.HeightBase = Rd<int16_t>(r, 4);
            int16_t xs[4], ys[4], zs[4];
            for (int k = 0; k < 4; k++) xs[k] = Rd<int16_t>(r, 8 + 2 * k);
            for (int k = 0; k < 4; k++) ys[k] = Rd<int16_t>(r, 0x10 + 2 * k);
            for (int k = 0; k < 4; k++) zs[k] = Rd<int16_t>(r, 0x18 + 2 * k);
            p.IsBarrier = p.HeightBase == 0 && zs[0] == -CollisionPolygonU2::BarrierHeight && zs[1] == CollisionPolygonU2::BarrierHeight
                          && zs[2] == CollisionPolygonU2::BarrierHeight && zs[3] == -CollisionPolygonU2::BarrierHeight;
            auto V = [&](int k) {
                return Vec3(xs[k] / 8.0f, ys[k] / 8.0f, p.IsBarrier ? 0.0f : p.HeightBase + zs[k] / 256.0f);
            };
            p.C0 = V(0);
            p.C1 = V(1);
            p.C2 = V(2);
            p.C3 = V(3);
            c.Polygons[i] = p;
        }
    }

    if (hasRefs) {
        size_t n = refs.size / 4;
        c.RefPolygon.resize(n);
        c.RefMask.resize(n);
        for (size_t i = 0; i < n; i++) {
            c.RefPolygon[i] = Rd<uint16_t>(refs.data, 4 * i);
            c.RefMask[i] = Rd<uint16_t>(refs.data, 4 * i + 2);
        }
    }

    if (hasCells) {
        size_t n = cells.size / 4;
        size_t wh = static_cast<size_t>(c.GridWidth) * static_cast<size_t>(c.GridHeight);
        if (wh < n) n = wh;
        c.CellFirst.resize(n);
        c.CellCount.resize(n);
        for (size_t i = 0; i < n; i++) {
            c.CellFirst[i] = Rd<uint16_t>(cells.data, 4 * i);
            c.CellCount[i] = Rd<uint16_t>(cells.data, 4 * i + 2);
        }
    }
    return c;
}

bool TrackCollisionU2::TryGetGroundHeight(float x, float y, float maxZ, float& z) const
{
    z = 0;
    int cx = static_cast<int>(std::floor((x - OriginX) / CellSize)), cy = static_cast<int>(std::floor((y - OriginY) / CellSize));
    if (cx < 0 || cy < 0 || cx >= GridWidth || cy >= GridHeight) return false;
    size_t cell = static_cast<size_t>(cy) * GridWidth + cx;
    if (cell >= CellFirst.size()) return false;
    bool found = false;
    float best = -std::numeric_limits<float>::infinity();
    for (int k = 0; k < CellCount[cell]; k++) {
        size_t ri = static_cast<size_t>(CellFirst[cell]) + k;
        if (ri >= RefPolygon.size()) break;
        size_t pi = RefPolygon[ri];
        if (pi >= Polygons.size()) continue;
        const CollisionPolygonU2& p = Polygons[pi];
        if (!p.IsGround()) continue;
        float h;
        if (HeightInTriangle(p.C0, p.C1, p.C2, x, y, h) || (p.IsQuad() && HeightInTriangle(p.C0, p.C2, p.C3, x, y, h))) {
            if (h <= maxZ && h > best) { best = h; found = true; }
        }
    }
    if (found) z = best;
    return found;
}

bool TrackCollisionU2::TryNearestGround(float x, float y, Vec3& point) const
{
    point = Vec3();
    float best = FLT_MAX;
    for (const auto& p : Polygons) {
        if (!p.IsGround()) continue;
        int n = p.IsQuad() ? 4 : 3;
        float cx = 0, cy = 0, cz = 0;
        for (int k = 0; k < n; k++) {
            const Vec3& c = p.Corner(k);
            cx += c.X;
            cy += c.Y;
            cz += c.Z;
        }
        cx /= n;
        cy /= n;
        cz /= n;
        float d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (d < best) {
            best = d;
            point = Vec3(cx, cy, cz);
        }
    }
    return best < FLT_MAX;
}

bool TrackCollisionU2::HeightInTriangle(const Vec3& a, const Vec3& b, const Vec3& c, float x, float y, float& z)
{
    z = 0;
    float d = (b.Y - c.Y) * (a.X - c.X) + (c.X - b.X) * (a.Y - c.Y);
    if (std::fabs(d) < 1e-6f) return false;
    float l0 = ((b.Y - c.Y) * (x - c.X) + (c.X - b.X) * (y - c.Y)) / d;
    float l1 = ((c.Y - a.Y) * (x - c.X) + (a.X - c.X) * (y - c.Y)) / d;
    float l2 = 1 - l0 - l1;
    const float eps = -1e-4f;
    if (l0 < eps || l1 < eps || l2 < eps) return false;
    z = l0 * a.Z + l1 * b.Z + l2 * c.Z;
    return true;
}

} // namespace bb
