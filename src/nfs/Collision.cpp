#include "nfs/Collision.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace nfs {

namespace {

// --- Header, chunk 0x00034131 ------------------------------------------------
constexpr size_t kHeaderSize          = 48;
constexpr size_t kHeaderSection       = 0x00;
constexpr size_t kHeaderGridWidth     = 0x04;
constexpr size_t kHeaderGridHeight    = 0x06;
constexpr size_t kHeaderGridOrigin    = 0x08;   // float2
constexpr size_t kHeaderBoundsMin     = 0x10;   // float3 + pad
constexpr size_t kHeaderBoundsMax     = 0x20;   // float3 + pad

// --- Polygon, chunk 0x00034134 -----------------------------------------------
constexpr size_t kPolygonSize         = 32;
constexpr size_t kPolygonSurface      = 0x01;
constexpr size_t kPolygonFlags        = 0x03;
constexpr size_t kPolygonHeightBase   = 0x04;   // i16, whole units
constexpr size_t kPolygonX            = 0x08;   // i16[4]
constexpr size_t kPolygonY            = 0x10;   // i16[4]
constexpr size_t kPolygonZ            = 0x18;   // i16[4]

// Plan coordinates are eighths of a unit. Height is split into a whole-unit
// base per polygon plus a 1/256 fraction per corner, which is finer than the
// plan precision: the car has to sit on the road without jitter.
constexpr float kPlanScale   = 1.0f / 8.0f;
constexpr float kHeightScale = 1.0f / 256.0f;

constexpr u8 kFlagQuad = 0x10;

// Barriers store the extreme heights -32512, +32512, +32512, -32512 with a
// zero base: a wall between two points that no car can drive over.
constexpr i16 kBarrierHeight = 32512;

bool IsBarrier(ByteSpan entry) {
    return entry.read<i16>(kPolygonZ + 0) == -kBarrierHeight &&
           entry.read<i16>(kPolygonZ + 2) ==  kBarrierHeight &&
           entry.read<i16>(kPolygonZ + 4) ==  kBarrierHeight &&
           entry.read<i16>(kPolygonZ + 6) == -kBarrierHeight;
}

} // namespace

u32 CollisionFingerprint(const CollisionPack& pack) {
    u32 hash = 2166136261u;                                   // FNV-1a
    auto mix = [&hash](const void* data, size_t size) {
        const u8* bytes = (const u8*)data;
        for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 16777619u; }
    };
    for (const CollisionPolygon& polygon : pack.polygons) {
        mix(polygon.attributes, 4);
        mix(polygon.corners, sizeof(polygon.corners));
    }
    return hash;
}

size_t CollisionPack::BarrierCount() const {
    size_t count = 0;
    for (const CollisionPolygon& polygon : polygons) count += polygon.barrier;
    return count;
}

float CollisionPack::GroundHeightNear(const Vec3& point, float fallback) const {
    float best = 1e30f, height = fallback;
    for (const CollisionPolygon& polygon : polygons) {
        if (polygon.barrier) continue;
        for (int c = 0; c < (polygon.quad ? 4 : 3); ++c) {
            const float dx = polygon.corners[c].x - point.x;
            const float dy = polygon.corners[c].y - point.y;
            const float distance = dx * dx + dy * dy;
            if (distance < best) { best = distance; height = polygon.corners[c].z; }
        }
    }
    return height;
}

void CollisionPack::BarrierQuad(const CollisionPolygon& barrier, Vec3 out[4]) const {
    const Vec3& a = barrier.corners[0];
    const Vec3& b = barrier.corners[2];
    const float za = GroundHeightNear(a, 0.0f);
    const float zb = GroundHeightNear(b, za);
    out[0] = { a.x, a.y, za - kBarrierBelow };
    out[1] = { a.x, a.y, za + kBarrierAbove };
    out[2] = { b.x, b.y, zb + kBarrierAbove };
    out[3] = { b.x, b.y, zb - kBarrierBelow };
}

// Recovers a global listing order from the cells: consecutive entries of a
// cell say which polygon comes first, and a topological sort of all those
// pairs (ties broken by file index) gives a rank that reproduces every cell.
static std::vector<u32> RecoverGridRank(size_t count, ByteSpan refs, ByteSpan cells) {
    std::vector<std::vector<u32>> next(count);
    std::vector<u32> incoming(count, 0);

    for (size_t c = 0; c + 4 <= cells.size; c += 4) {
        const u16 first = cells.read<u16>(c), n = cells.read<u16>(c + 2);
        for (u32 k = 1; k < n; ++k) {
            const u16 a = refs.read<u16>((size_t)(first + k - 1) * 4);
            const u16 b = refs.read<u16>((size_t)(first + k) * 4);
            if (a >= count || b >= count || a == b) continue;
            next[a].push_back(b);
            ++incoming[b];
        }
    }

    std::priority_queue<u32, std::vector<u32>, std::greater<u32>> ready;
    for (u32 i = 0; i < count; ++i)
        if (incoming[i] == 0) ready.push(i);

    std::vector<u32> rank(count, 0);
    u32 position = 0;
    while (!ready.empty()) {
        const u32 i = ready.top();
        ready.pop();
        rank[i] = position++;
        for (u32 j : next[i])
            if (--incoming[j] == 0) ready.push(j);
    }

    // A cycle would mean the cells disagree with each other. No shipped pack
    // does that; if one ever did, file order is the honest fallback.
    if (position != count) {
        for (u32 i = 0; i < count; ++i) rank[i] = i;
    }
    return rank;
}

bool ParseCollision(const Chunk& collisionChunk, CollisionPack& out) {
    ByteSpan header, polygons, cellRefs, cells;

    ChunkReader reader = ChunkReader::Children(collisionChunk);
    Chunk child;
    while (reader.Next(child)) {
        if (child.id == kChunkCollisionHeader)        header   = child.data;
        else if (child.id == kChunkCollisionPolygons) polygons = child.data;
        else if (child.id == kChunkCollisionCellRefs) cellRefs = child.data;
        else if (child.id == kChunkCollisionCells)    cells    = child.data;
    }

    if (header.size < kHeaderSize || polygons.empty()) return false;

    out.sectionNumber = header.read<u32>(kHeaderSection);
    out.gridWidth     = header.read<u16>(kHeaderGridWidth);
    out.gridHeight    = header.read<u16>(kHeaderGridHeight);
    out.gridOrigin    = { header.read<float>(kHeaderGridOrigin),
                          header.read<float>(kHeaderGridOrigin + 4), 0.0f };
    out.bounds.min    = { header.read<float>(kHeaderBoundsMin),
                          header.read<float>(kHeaderBoundsMin + 4),
                          header.read<float>(kHeaderBoundsMin + 8) };
    out.bounds.max    = { header.read<float>(kHeaderBoundsMax),
                          header.read<float>(kHeaderBoundsMax + 4),
                          header.read<float>(kHeaderBoundsMax + 8) };

    const size_t count = polygons.size / kPolygonSize;
    out.polygons.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const ByteSpan entry = polygons.sub(i * kPolygonSize, kPolygonSize);

        CollisionPolygon polygon;
        polygon.surface = entry.read<u8>(kPolygonSurface);
        polygon.flags   = entry.read<u8>(kPolygonFlags);
        polygon.quad    = (polygon.flags & kFlagQuad) != 0;
        polygon.barrier = IsBarrier(entry);
        for (int b = 0; b < 4; ++b) polygon.attributes[b] = entry.read<u8>((size_t)b);

        const float base = (float)entry.read<i16>(kPolygonHeightBase);
        for (int corner = 0; corner < 4; ++corner) {
            const size_t at = (size_t)corner * 2;
            polygon.corners[corner] = {
                entry.read<i16>(kPolygonX + at) * kPlanScale,
                entry.read<i16>(kPolygonY + at) * kPlanScale,
                polygon.barrier ? 0.0f : base + entry.read<i16>(kPolygonZ + at) * kHeightScale };
        }
        out.polygons.push_back(polygon);
    }

    out.gridRank = RecoverGridRank(out.polygons.size(), cellRefs, cells);
    return !out.polygons.empty();
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

namespace {

constexpr float kCellSize    = 32.0f;
constexpr float kSubcellSize = 8.0f;

// How far apart a polygon and a square may be and still count as touching.
// The game's tool treats contact as overlap, and float noise in the separating
// axis test would otherwise drop the polygons that only meet a grid line.
constexpr double kTouchTolerance = 1e-4;

struct Point2 { double x, y; };

// Separation of a convex polygon from an axis-aligned square along the best
// separating axis: positive when they are apart, zero or less when they touch
// or overlap. Degenerate polygons -- a barrier or a vertical face is a segment
// in plan -- work too, because the square's own axes are always tested.
double Separation(const std::vector<Point2>& polygon, double x0, double y0, double size) {
    const Point2 square[4] = { { x0, y0 }, { x0 + size, y0 },
                               { x0 + size, y0 + size }, { x0, y0 + size } };
    double worst = -1e30;

    auto test = [&](double nx, double ny) {
        double pMin = 1e30, pMax = -1e30, sMin = 1e30, sMax = -1e30;
        for (const Point2& p : polygon) {
            const double d = nx * p.x + ny * p.y;
            pMin = std::min(pMin, d); pMax = std::max(pMax, d);
        }
        for (const Point2& p : square) {
            const double d = nx * p.x + ny * p.y;
            sMin = std::min(sMin, d); sMax = std::max(sMax, d);
        }
        worst = std::max(worst, std::max(sMin - pMax, pMin - sMax));
    };

    test(1.0, 0.0);
    test(0.0, 1.0);
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Point2& a = polygon[i];
        const Point2& b = polygon[(i + 1) % polygon.size()];
        const double nx = -(b.y - a.y), ny = b.x - a.x;
        const double length = std::sqrt(nx * nx + ny * ny);
        if (length > 0.0) test(nx / length, ny / length);
    }
    return worst;
}

void Put16(std::vector<u8>& out, size_t at, u16 value) { std::memcpy(out.data() + at, &value, 2); }
void Put32(std::vector<u8>& out, size_t at, u32 value) { std::memcpy(out.data() + at, &value, 4); }
void PutF(std::vector<u8>& out, size_t at, float value) { std::memcpy(out.data() + at, &value, 4); }

void AppendChunk(std::vector<u8>& out, u32 id, const std::vector<u8>& payload) {
    const size_t at = out.size();
    out.resize(at + 8);
    Put32(out, at, id);
    Put32(out, at + 4, (u32)payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

bool Quantise(float value, float scale, i16& out) {
    const long q = std::lround(value * scale);
    if (q < -32767 || q > 32767) return false;
    out = (i16)q;
    return true;
}

} // namespace

bool EncodeCollision(const CollisionPack& pack, std::vector<u8>& chunk, std::wstring& error) {
    const size_t count = pack.polygons.size();

    // An empty pack is a real thing the game ships: a one-cell grid at the
    // origin with the empty-box sentinel for bounds and nothing in the cell.
    if (count == 0) {
        std::vector<u8> header(kHeaderSize, 0);
        Put32(header, kHeaderSection, pack.sectionNumber);
        Put16(header, kHeaderGridWidth, 1);
        Put16(header, kHeaderGridHeight, 1);
        for (int axis = 0; axis < 3; ++axis) {
            PutF(header, kHeaderBoundsMin + (size_t)axis * 4, FLT_MAX);
            PutF(header, kHeaderBoundsMax + (size_t)axis * 4, -FLT_MAX);
        }
        std::vector<u8> children;
        AppendChunk(children, kChunkCollisionHeader, header);
        AppendChunk(children, kChunkCollisionPolygons, {});
        AppendChunk(children, kChunkCollisionCellRefs, {});
        AppendChunk(children, kChunkCollisionCells, std::vector<u8>(4, 0));
        chunk.clear();
        AppendChunk(chunk, kChunkCollisionPack, children);
        return true;
    }
    if (count > 65535)  { error = L"More than 65 535 collision polygons in one section."; return false; }

    // --- polygons, and the plan outlines the grid is built from -----------
    std::vector<u8> records(count * kPolygonSize, 0);
    std::vector<std::vector<Point2>> outlines(count);
    Aabb bounds;

    for (size_t i = 0; i < count; ++i) {
        const CollisionPolygon& polygon = pack.polygons[i];
        const size_t at = i * kPolygonSize;
        std::memcpy(records.data() + at, polygon.attributes, 4);
        Put16(records, at + 6, 0xFFFF);

        // A barrier is stored as its two ends, each written twice.
        Vec3 corners[4];
        for (int c = 0; c < 4; ++c) corners[c] = polygon.corners[c];
        if (polygon.barrier) {
            corners[1] = corners[0];
            corners[3] = corners[2];
        }

        i16 base = 0;
        if (!polygon.barrier) {
            // The whole-unit base is the floor of the middle of the polygon's
            // height range, which keeps the per-corner offsets centred on it.
            const float lowest  = std::min({ corners[0].z, corners[1].z, corners[2].z, corners[3].z });
            const float highest = std::max({ corners[0].z, corners[1].z, corners[2].z, corners[3].z });
            const long floorBase = (long)std::floor((lowest + highest) * 0.5f);
            if (floorBase < -32767 || floorBase > 32767) {
                error = L"A collision polygon lies outside the height the file can store.";
                return false;
            }
            base = (i16)floorBase;
        }
        Put16(records, at + kPolygonHeightBase, (u16)base);

        i16 planX[4] = {}, planY[4] = {};
        for (int c = 0; c < 4; ++c) {
            i16 x = 0, y = 0, z = 0;
            if (!Quantise(corners[c].x, 8.0f, x) || !Quantise(corners[c].y, 8.0f, y)) {
                error = L"A collision polygon lies outside the plan range of the file (+/-4096).";
                return false;
            }
            if (polygon.barrier) {
                z = (c == 0 || c == 3) ? -kBarrierHeight : kBarrierHeight;
            } else if (!Quantise(corners[c].z - (float)base, 256.0f, z)) {
                error = L"A collision polygon is more than 127 units tall; split it.";
                return false;
            }
            Put16(records, at + kPolygonX + (size_t)c * 2, (u16)x);
            Put16(records, at + kPolygonY + (size_t)c * 2, (u16)y);
            Put16(records, at + kPolygonZ + (size_t)c * 2, (u16)z);
            planX[c] = x;
            planY[c] = y;

            // Measured from the stored values, so the header and the grid
            // describe exactly what the game will read.
            const float px = x * kPlanScale, py = y * kPlanScale;
            const float pz = polygon.barrier ? z * kHeightScale : base + z * kHeightScale;
            bounds.Add(Vec3{ px, py, pz });

            const Point2 point{ px, py };
            auto& outline = outlines[i];
            if (std::none_of(outline.begin(), outline.end(),
                             [&](const Point2& p) { return p.x == point.x && p.y == point.y; }))
                outline.push_back(point);
        }

        // Byte 1 is not a surface: it names, two bits each, the corner lying
        // furthest toward -x, -y, +x and +y (bits 0-1, 2-3, 4-5, 6-7), a tie
        // going to the later corner. That is a precomputed box the game tests
        // a car against before the polygon itself, and it holds for every one
        // of the 432 000 shipped polygons of the eight regions, barriers
        // included. Copied from another polygon it describes the wrong box:
        // cars fall through ground and pass through walls wherever the two
        // disagree. So it is computed, never copied.
        const auto extreme = [](const i16 values[4], bool greatest) {
            int best = 0;
            for (int c = 1; c < 4; ++c)
                if (greatest ? values[c] >= values[best] : values[c] <= values[best]) best = c;
            return best;
        };
        records[at + kPolygonSurface] =
            (u8)(extreme(planX, false) | extreme(planY, false) << 2 |
                 extreme(planX, true) << 4 | extreme(planY, true) << 6);
    }

    // --- grid ----------------------------------------------------------------
    const float originX = std::floor(bounds.min.x / kCellSize) * kCellSize;
    const float originY = std::floor(bounds.min.y / kCellSize) * kCellSize;
    const long width  = (long)std::floor(bounds.max.x / kCellSize) - (long)std::floor(bounds.min.x / kCellSize) + 1;
    const long height = (long)std::floor(bounds.max.y / kCellSize) - (long)std::floor(bounds.min.y / kCellSize) + 1;
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535 || width * height > 1 << 22) {
        error = L"The collision covers an area too large for one grid.";
        return false;
    }

    // Per cell, the polygons in it (in file order) and the sub-cells they touch.
    std::vector<std::vector<std::pair<u16, u16>>> cells((size_t)(width * height));

    for (size_t i = 0; i < count; ++i) {
        const auto& outline = outlines[i];
        double minX = 1e30, maxX = -1e30, minY = 1e30, maxY = -1e30;
        for (const Point2& p : outline) {
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        }

        const long cx0 = std::max(0L, (long)std::floor((minX - originX) / kCellSize));
        const long cx1 = std::min(width - 1, (long)std::floor((maxX - originX) / kCellSize));
        const long cy0 = std::max(0L, (long)std::floor((minY - originY) / kCellSize));
        const long cy1 = std::min(height - 1, (long)std::floor((maxY - originY) / kCellSize));

        for (long cy = cy0; cy <= cy1; ++cy) {
            for (long cx = cx0; cx <= cx1; ++cx) {
                const double x0 = originX + cx * kCellSize, y0 = originY + cy * kCellSize;

                // A cell owns its low edges and not its high ones: a polygon
                // that only reaches a cell's far boundary belongs to the next.
                if (minX >= x0 + kCellSize || minY >= y0 + kCellSize) continue;
                if (Separation(outline, x0, y0, kCellSize) > kTouchTolerance) continue;

                // Inside the cell, a sub-cell counts when it is merely touched.
                u16 mask = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                        if (Separation(outline, x0 + sx * kSubcellSize, y0 + sy * kSubcellSize,
                                       kSubcellSize) <= kTouchTolerance)
                            mask |= (u16)(1u << (sy * 4 + sx));

                if (mask) cells[(size_t)(cy * width + cx)].push_back({ (u16)i, mask });
            }
        }
    }

    const bool ranked = pack.gridRank.size() == count;
    for (auto& cell : cells)
        if (ranked)
            std::stable_sort(cell.begin(), cell.end(), [&](const auto& a, const auto& b) {
                return pack.gridRank[a.first] < pack.gridRank[b.first];
            });

    std::vector<u8> refs, table((size_t)(width * height) * 4, 0);
    size_t first = 0;
    for (size_t c = 0; c < cells.size(); ++c) {
        if (first > 65535 || cells[c].size() > 65535) {
            error = L"Too many collision references for the grid's 16-bit fields.";
            return false;
        }
        Put16(table, c * 4, (u16)first);
        Put16(table, c * 4 + 2, (u16)cells[c].size());
        for (const auto& [polygon, mask] : cells[c]) {
            const size_t at = refs.size();
            refs.resize(at + 4);
            Put16(refs, at, polygon);
            Put16(refs, at + 2, mask);
        }
        first += cells[c].size();
    }

    // --- header --------------------------------------------------------------
    std::vector<u8> header(kHeaderSize, 0);
    Put32(header, kHeaderSection, pack.sectionNumber);
    Put16(header, kHeaderGridWidth, (u16)width);
    Put16(header, kHeaderGridHeight, (u16)height);
    PutF(header, kHeaderGridOrigin, originX);
    PutF(header, kHeaderGridOrigin + 4, originY);
    PutF(header, kHeaderBoundsMin, bounds.min.x);
    PutF(header, kHeaderBoundsMin + 4, bounds.min.y);
    PutF(header, kHeaderBoundsMin + 8, bounds.min.z);
    PutF(header, kHeaderBoundsMax, bounds.max.x);
    PutF(header, kHeaderBoundsMax + 4, bounds.max.y);
    PutF(header, kHeaderBoundsMax + 8, bounds.max.z);

    // The children carry no alignment filler in any shipped pack.
    std::vector<u8> children;
    AppendChunk(children, kChunkCollisionHeader, header);
    AppendChunk(children, kChunkCollisionPolygons, records);
    AppendChunk(children, kChunkCollisionCellRefs, refs);
    AppendChunk(children, kChunkCollisionCells, table);

    chunk.clear();
    AppendChunk(chunk, kChunkCollisionPack, children);
    return true;
}

} // namespace nfs
