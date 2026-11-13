#include "minimap/MinimapBuildings.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace minimap {

namespace {

// --- 2D polygons -------------------------------------------------------------

float Cross2(const Vec2& o, const Vec2& a, const Vec2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

float SignedArea(const std::vector<Vec2>& poly) {
    float area = 0.0f;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vec2& a = poly[i];
        const Vec2& b = poly[(i + 1) % poly.size()];
        area += a.x * b.y - b.x * a.y;
    }
    return area * 0.5f;
}

Vec2 Centroid(const std::vector<Vec2>& poly) {
    Vec2 sum;
    for (const Vec2& p : poly) { sum.x += p.x; sum.y += p.y; }
    if (!poly.empty()) { sum.x /= (float)poly.size(); sum.y /= (float)poly.size(); }
    return sum;
}

bool Inside(const std::vector<Vec2>& poly, const Vec2& p) {
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2& a = poly[i];
        const Vec2& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) &&
            p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

bool IsConvex(const std::vector<Vec2>& poly) {
    for (size_t i = 0; i < poly.size(); ++i)
        if (Cross2(poly[i], poly[(i + 1) % poly.size()], poly[(i + 2) % poly.size()]) < -1e-3f)
            return false;
    return true;
}

// Counter-clockwise convex hull, Andrew's monotone chain.
std::vector<Vec2> Hull(std::vector<Vec2> points) {
    std::sort(points.begin(), points.end(), [](const Vec2& a, const Vec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    if (points.size() < 3) return points;
    std::vector<Vec2> hull(points.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        while (k >= 2 && Cross2(hull[k - 2], hull[k - 1], points[i]) <= 0.0f) k--;
        hull[k++] = points[i];
    }
    for (size_t i = points.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && Cross2(hull[k - 2], hull[k - 1], points[i - 1]) <= 0.0f) k--;
        hull[k++] = points[i - 1];
    }
    hull.resize(k - 1);
    return hull;
}

// Drops corners that bend the outline by less than `tolerance` units, so a
// curved wall or a ledge detail does not cost the file a point each.
std::vector<Vec2> Simplify(std::vector<Vec2> poly, float tolerance) {
    bool changed = true;
    while (changed && poly.size() > 3) {
        changed = false;
        size_t best = 0;
        float  bestDistance = tolerance;
        for (size_t i = 0; i < poly.size(); ++i) {
            const Vec2& a = poly[(i + poly.size() - 1) % poly.size()];
            const Vec2& p = poly[i];
            const Vec2& b = poly[(i + 1) % poly.size()];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            const float distance = length > 1e-4f
                ? std::fabs(Cross2(a, b, p)) / length
                : std::sqrt((p.x - a.x) * (p.x - a.x) + (p.y - a.y) * (p.y - a.y));
            if (distance < bestDistance) { bestDistance = distance; best = i; changed = true; }
        }
        if (changed) poly.erase(poly.begin() + best);
    }
    return poly;
}

// Triangles of a simple counter-clockwise polygon, as index triples.
std::vector<u32> Triangulate(const std::vector<Vec2>& poly) {
    std::vector<u32> out;
    std::vector<u32> left(poly.size());
    for (u32 i = 0; i < (u32)poly.size(); ++i) left[i] = i;
    size_t guard = 0;
    while (left.size() > 3 && guard++ < poly.size() * poly.size()) {
        bool clipped = false;
        for (size_t i = 0; i < left.size(); ++i) {
            const u32 ia = left[(i + left.size() - 1) % left.size()];
            const u32 ib = left[i];
            const u32 ic = left[(i + 1) % left.size()];
            const Vec2 &a = poly[ia], &b = poly[ib], &c = poly[ic];
            if (Cross2(a, b, c) <= 1e-6f) continue;
            bool empty = true;
            for (u32 other : left) {
                if (other == ia || other == ib || other == ic) continue;
                const Vec2& p = poly[other];
                if (Cross2(a, b, p) >= 0.0f && Cross2(b, c, p) >= 0.0f && Cross2(c, a, p) >= 0.0f) {
                    empty = false;
                    break;
                }
            }
            if (!empty) continue;
            out.insert(out.end(), { ia, ib, ic });
            left.erase(left.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (left.size() == 3) out.insert(out.end(), { left[0], left[1], left[2] });
    else for (size_t i = 1; i + 1 < left.size(); ++i)   // a knot: fan what is left
        out.insert(out.end(), { left[0], left[i], left[i + 1] });
    return out;
}

// The part of a convex polygon on the side of the line where `side` >= 0.
std::vector<Vec2> ClipHalf(const std::vector<Vec2>& poly, const Vec2& origin, const Vec2& normal) {
    std::vector<Vec2> out;
    const auto side = [&](const Vec2& p) {
        return (p.x - origin.x) * normal.x + (p.y - origin.y) * normal.y;
    };
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vec2& a = poly[i];
        const Vec2& b = poly[(i + 1) % poly.size()];
        const float sa = side(a), sb = side(b);
        if (sa >= 0.0f) out.push_back(a);
        if ((sa >= 0.0f) != (sb >= 0.0f)) {
            const float t = sa / (sa - sb);
            out.push_back({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t });
        }
    }
    return out;
}

// --- the model in world space ------------------------------------------------

struct Triangle {
    Vec3  v[3];
    u32   id[3];      // welded vertex ids
    Vec3  normal;     // unit, pointing out of the building
    float area = 0.0f;
};

struct WorldMesh {
    std::vector<Vec3>     points;      // welded
    std::vector<Triangle> triangles;
    float minZ = 0.0f, maxZ = 0.0f;
};

WorldMesh ToWorld(const PlacedModel& placed) {
    WorldMesh mesh;
    const nfs::SolidObject& model = *placed.model;
    std::map<std::tuple<int, int, int>, u32> weld;
    std::vector<u32> ids(model.vertices.size());
    for (size_t i = 0; i < model.vertices.size(); ++i) {
        const Vec3 p = placed.transform.TransformPoint(model.vertices[i].position);
        const auto key = std::make_tuple((int)std::lround(p.x * 20.0f), (int)std::lround(p.y * 20.0f),
                                         (int)std::lround(p.z * 20.0f));
        const auto found = weld.emplace(key, (u32)mesh.points.size());
        if (found.second) mesh.points.push_back(p);
        ids[i] = found.first->second;
    }

    float up = 0.0f;
    for (size_t i = 0; i + 2 < model.indices.size(); i += 3) {
        Triangle t;
        bool valid = true;
        for (int k = 0; k < 3; ++k) {
            const u16 index = model.indices[i + k];
            if (index >= ids.size()) { valid = false; break; }
            t.id[k] = ids[index];
            t.v[k] = mesh.points[t.id[k]];
        }
        if (!valid || t.id[0] == t.id[1] || t.id[1] == t.id[2] || t.id[0] == t.id[2]) continue;
        const Vec3 n = Cross(t.v[1] - t.v[0], t.v[2] - t.v[0]);
        t.area = Length(n) * 0.5f;
        if (t.area < 1e-4f) continue;
        t.normal = n * (0.5f / t.area);
        up += t.normal.z * t.area;
        mesh.triangles.push_back(t);
    }

    // The winding decides which way a normal points, and a mirrored placement
    // reverses it. Buildings have roofs and no floor, so the side that faces
    // up on balance is the outside.
    if (up < 0.0f)
        for (Triangle& t : mesh.triangles) {
            std::swap(t.v[1], t.v[2]);
            std::swap(t.id[1], t.id[2]);
            t.normal = -t.normal;
        }

    mesh.minZ = 1e30f; mesh.maxZ = -1e30f;
    for (const Vec3& p : mesh.points) { mesh.minZ = std::min(mesh.minZ, p.z); mesh.maxZ = std::max(mesh.maxZ, p.z); }
    return mesh;
}

// A facade is walls that face one way: the street side of a block with
// nothing behind it. Whether the walls face both ways along both of the
// model's own axes tells the two apart.
bool IsFacade(const WorldMesh& mesh, const Mat4& transform) {
    const Vec3 origin = transform.TransformPoint({ 0, 0, 0 });
    const Vec3 ax = Normalize(transform.TransformPoint({ 1, 0, 0 }) - origin);
    const Vec3 ay = Normalize(transform.TransformPoint({ 0, 1, 0 }) - origin);
    float facing[4] = {};
    for (const Triangle& t : mesh.triangles) {
        if (std::fabs(t.normal.z) > 0.5f) continue;
        const float dx = Dot(t.normal, ax), dy = Dot(t.normal, ay);
        if (std::fabs(dx) >= std::fabs(dy)) facing[dx >= 0.0f ? 0 : 1] += t.area;
        else                                 facing[dy >= 0.0f ? 2 : 3] += t.area;
    }
    const auto balanced = [](float a, float b) {
        return std::max(a, b) > 0.0f && std::min(a, b) >= 0.15f * std::max(a, b);
    };
    return !(balanced(facing[0], facing[1]) && balanced(facing[2], facing[3]));
}

Building BoxOf(const PlacedModel& placed, const WorldMesh& mesh) {
    Aabb local = placed.model->bounds;
    if (!local.Valid())
        for (const nfs::SolidVertex& v : placed.model->vertices) local.Add(v.position);

    Part part;
    float minZ = 1e30f, maxZ = -1e30f;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 p = placed.transform.TransformPoint({
            (corner & 1) ? local.max.x : local.min.x,
            (corner & 2) ? local.max.y : local.min.y,
            (corner & 4) ? local.max.z : local.min.z });
        minZ = std::min(minZ, p.z);
        maxZ = std::max(maxZ, p.z);
    }
    for (int corner : { 0, 1, 3, 2 }) {
        const Vec3 p = placed.transform.TransformPoint({
            (corner & 1) ? local.max.x : local.min.x,
            (corner & 2) ? local.max.y : local.min.y, local.min.z });
        part.footprint.push_back({ p.x, p.y });
    }
    if (SignedArea(part.footprint) < 0.0f) std::reverse(part.footprint.begin(), part.footprint.end());
    part.base = mesh.triangles.empty() ? minZ : std::max(minZ, mesh.minZ);
    part.height = (mesh.triangles.empty() ? maxZ : std::min(maxZ, mesh.maxZ)) - part.base;

    Building building;
    building.kind = BuildingKind::Box;
    building.parts.push_back(part);
    return building;
}

Building MeshOf(const WorldMesh& mesh) {
    // Vertex clustering: every vertex snaps to a cell of a coarse grid, and
    // what survives are the triangles whose corners land in three cells.
    Aabb box;
    for (const Vec3& p : mesh.points) box.Add(p);
    const Vec3 size = box.max - box.min;
    const float cell = std::max(1.0f, std::max(size.x, std::max(size.y, size.z)) / 12.0f);

    Building building;
    building.kind = BuildingKind::Mesh;
    std::map<std::tuple<int, int, int>, u32> cells;
    std::vector<Vec3> sums;
    std::vector<u32> counts;
    std::vector<u32> remap(mesh.points.size());
    for (size_t i = 0; i < mesh.points.size(); ++i) {
        const Vec3 p = mesh.points[i] - box.min;
        const auto key = std::make_tuple((int)(p.x / cell), (int)(p.y / cell), (int)(p.z / cell));
        const auto found = cells.emplace(key, (u32)sums.size());
        if (found.second) { sums.push_back({}); counts.push_back(0); }
        remap[i] = found.first->second;
        sums[remap[i]] += mesh.points[i];
        counts[remap[i]]++;
    }
    for (size_t i = 0; i < sums.size(); ++i)
        building.meshVertices.push_back(sums[i] * (1.0f / (float)counts[i]));

    std::set<std::tuple<u32, u32, u32>> seen;
    for (const Triangle& t : mesh.triangles) {
        const u32 a = remap[t.id[0]], b = remap[t.id[1]], c = remap[t.id[2]];
        if (a == b || b == c || a == c) continue;
        u32 sorted[3] = { a, b, c };
        std::sort(sorted, sorted + 3);
        if (!seen.insert(std::make_tuple(sorted[0], sorted[1], sorted[2])).second) continue;
        building.meshTriangles.insert(building.meshTriangles.end(), { a, b, c });
    }
    return building;
}

// The outlines of a set of roof triangles: every closed loop of edges that
// only one triangle uses, walked in the triangles' own direction. Outer
// boundaries come out counter-clockwise and courtyards clockwise.
std::vector<std::vector<Vec2>> Outlines(const WorldMesh& mesh, const std::vector<const Triangle*>& roof) {
    std::map<std::pair<u32, u32>, int> uses;
    for (const Triangle* t : roof)
        for (int k = 0; k < 3; ++k) {
            const u32 a = t->id[k], b = t->id[(k + 1) % 3];
            uses[{ std::min(a, b), std::max(a, b) }]++;
        }
    std::multimap<u32, u32> next;
    for (const Triangle* t : roof)
        for (int k = 0; k < 3; ++k) {
            const u32 a = t->id[k], b = t->id[(k + 1) % 3];
            if (uses[{ std::min(a, b), std::max(a, b) }] == 1) next.emplace(a, b);
        }

    std::vector<std::vector<Vec2>> loops;
    while (!next.empty()) {
        const u32 start = next.begin()->first;
        std::vector<Vec2> loop;
        u32 at = start;
        bool closed = false;
        for (size_t guard = 0; guard < 100000; ++guard) {
            const auto it = next.find(at);
            if (it == next.end()) break;
            loop.push_back({ mesh.points[at].x, mesh.points[at].y });
            at = it->second;
            next.erase(it);
            if (at == start) { closed = true; break; }
        }
        if (closed && loop.size() >= 3) loops.push_back(std::move(loop));
    }
    return loops;
}

struct Level {
    float z = 0.0f;
    float area = 0.0f;
    std::vector<const Triangle*> triangles;
};

// The pitched roof over one part, from the sloped faces standing on it.
Roof FitRoof(const Part& part, const std::vector<const Triangle*>& sloped) {
    Roof roof;
    float projected = 0.0f, peak = part.base + part.height;
    for (const Triangle* t : sloped) {
        projected += t->area * t->normal.z;
        for (const Vec3& v : t->v) peak = std::max(peak, v.z);
    }
    const float top = part.base + part.height;
    const float rise = peak - top;
    if (projected < 0.3f * std::fabs(SignedArea(part.footprint)) || rise < 0.5f) return roof;
    roof.height = rise;

    std::vector<Vec2> ridge;
    std::set<int> slopes;
    for (const Triangle* t : sloped) {
        slopes.insert((int)std::lround(t->normal.z * 10.0f));
        for (const Vec3& v : t->v)
            if (v.z >= top + 0.9f * rise) ridge.push_back({ v.x, v.y });
    }
    const auto farthest = [](const std::vector<Vec2>& points, Vec2& a, Vec2& b) {
        float best = 0.0f;
        const size_t n = std::min<size_t>(points.size(), 256);
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j) {
                const float dx = points[i].x - points[j].x, dy = points[i].y - points[j].y;
                const float d = dx * dx + dy * dy;
                if (d > best) { best = d; a = points[i]; b = points[j]; }
            }
        return std::sqrt(best);
    };
    Vec2 a, b, c, d;
    const float spread   = farthest(ridge, a, b);
    const float diameter = farthest(part.footprint, c, d);

    if (slopes.size() >= 4)              roof.type = RoofType::Dome;
    else if (spread < 0.15f * diameter)  roof.type = RoofType::Pyramid;
    else {
        roof.type = RoofType::Gable;
        float angle = std::atan2(b.y - a.y, b.x - a.x) * 180.0f / kPi;
        if (angle < 0.0f) angle += 180.0f;
        if (angle >= 180.0f) angle -= 180.0f;
        roof.direction = angle;
    }
    return roof;
}

// Stacked prisms from the flat roofs: every height a roof stands at is a
// part, unless a larger roof above already covers it.
bool FitParts(const WorldMesh& mesh, Building& out) {
    const float height = mesh.maxZ - mesh.minZ;

    std::vector<const Triangle*> flat, sloped;
    float upArea = 0.0f;
    for (const Triangle& t : mesh.triangles) {
        if (t.normal.z > 0.95f)      flat.push_back(&t);
        else if (t.normal.z > 0.3f)  sloped.push_back(&t);
        if (t.normal.z > 0.3f) upArea += t.area * t.normal.z;
    }

    // Flat faces grouped by height.
    const auto centreZ = [](const Triangle* t) { return (t->v[0].z + t->v[1].z + t->v[2].z) / 3.0f; };
    std::sort(flat.begin(), flat.end(), [&](const Triangle* a, const Triangle* b) {
        return centreZ(a) > centreZ(b);
    });
    std::vector<Level> levels;
    for (const Triangle* t : flat) {
        const float z = centreZ(t);
        if (z < mesh.minZ + 0.5f) continue;   // ground plates and kerbs
        if (levels.empty() || levels.back().z - z > 0.3f) levels.push_back({ z, 0.0f, {} });
        levels.back().triangles.push_back(t);
        levels.back().area += t->area;
    }
    float largest = 0.0f;
    for (const Level& level : levels) largest = std::max(largest, level.area);

    std::vector<Part> parts;
    for (const Level& level : levels) {
        if (level.area < std::max(4.0f, 0.03f * largest)) continue;

        std::vector<std::vector<Vec2>> outlines;
        float outlined = 0.0f;
        for (std::vector<Vec2>& loop : Outlines(mesh, level.triangles)) {
            const float area = SignedArea(loop);
            if (area < std::max(2.0f, 0.02f * level.area)) continue;   // courtyards, slivers
            outlined += area;
            outlines.push_back(Simplify(std::move(loop), 0.3f));
        }
        // Loops that do not close around the roof mean the faces do not share
        // their edges; the hull of the level is the next best thing.
        if (outlined < 0.9f * level.area) {
            std::vector<Vec2> points;
            for (const Triangle* t : level.triangles)
                for (const Vec3& v : t->v) points.push_back({ v.x, v.y });
            outlines = { Simplify(Hull(points), 0.3f) };
        }

        for (std::vector<Vec2>& outline : outlines) {
            if (outline.size() < 3) continue;
            const Vec2 centre = Centroid(outline);
            const float area = SignedArea(outline);
            bool covered = false;
            for (const Part& above : parts)
                if (Inside(above.footprint, centre) && area <= 1.1f * SignedArea(above.footprint)) {
                    covered = true;
                    break;
                }
            if (covered) continue;
            Part part;
            part.footprint = std::move(outline);
            part.height = level.z;   // the top for now; the base comes later
            parts.push_back(std::move(part));
        }
    }

    // A pitched roof with no flat roof under it: a house. Its eaves are
    // where the slopes start.
    if (parts.empty() && !sloped.empty()) {
        std::vector<Vec2> points;
        float eaves = 1e30f;
        for (const Triangle& t : mesh.triangles)
            for (const Vec3& v : t.v) points.push_back({ v.x, v.y });
        for (const Triangle* t : sloped)
            for (const Vec3& v : t->v) eaves = std::min(eaves, v.z);
        Part part;
        part.footprint = Simplify(Hull(points), 0.3f);
        part.height = eaves;
        parts.push_back(std::move(part));
    }
    if (parts.empty() || parts.size() > 16) return false;

    // Each part stands on the highest lower part under its centre, else on
    // the ground.
    std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.height < b.height; });
    for (size_t i = 0; i < parts.size(); ++i) {
        const float top = parts[i].height;
        const Vec2 centre = Centroid(parts[i].footprint);
        float base = mesh.minZ;
        for (size_t j = 0; j < i; ++j)
            if (parts[j].height < top - 0.3f && Inside(parts[j].footprint, centre))
                base = std::max(base, parts[j].height);
        parts[i].base = base;
        parts[i].height = top - base;
    }

    // Sloped faces go to the highest part they stand on.
    std::vector<std::vector<const Triangle*>> roofs(parts.size());
    for (const Triangle* t : sloped) {
        const Vec2 centre{ (t->v[0].x + t->v[1].x + t->v[2].x) / 3.0f,
                           (t->v[0].y + t->v[1].y + t->v[2].y) / 3.0f };
        const float low = std::min(t->v[0].z, std::min(t->v[1].z, t->v[2].z));
        int best = -1;
        for (size_t i = 0; i < parts.size(); ++i) {
            const float top = parts[i].base + parts[i].height;
            if (top <= low + 0.3f && Inside(parts[i].footprint, centre) &&
                (best < 0 || top > parts[best].base + parts[best].height))
                best = (int)i;
        }
        if (best >= 0) roofs[best].push_back(t);
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        parts[i].roof = FitRoof(parts[i], roofs[i]);
        // Pitched roofs are drawn over convex footprints.
        if (parts[i].roof.type != RoofType::Flat && !IsConvex(parts[i].footprint))
            parts[i].footprint = Simplify(Hull(parts[i].footprint), 0.3f);
    }

    // What the parts leave out decides whether they are the building at all:
    // an unexplained spire, or a footprint far smaller than the roofs.
    float reach = mesh.minZ, groundArea = 0.0f;
    for (const Part& part : parts) {
        reach = std::max(reach, part.base + part.height + part.roof.height);
        if (part.base <= mesh.minZ + 0.01f) groundArea += SignedArea(part.footprint);
    }
    if (reach < mesh.maxZ - std::max(1.5f, 0.15f * height)) return false;
    if (groundArea < 0.6f * upArea) return false;

    for (Part& part : parts)
        if (part.height < 0.1f) part.height = 0.1f;

    out.parts = std::move(parts);
    out.kind = (out.parts.size() == 1 && out.parts[0].roof.type == RoofType::Flat)
                   ? BuildingKind::Prism : BuildingKind::Parts;
    return true;
}

// Stacked prisms from a height map, for the buildings whose roofs do not read
// as clean flat faces: antennas, tanks, terraced tops, curved roofs. The top
// surface is sampled on a grid, the heights are grouped into a few levels, and
// every level becomes the parts that reach it, one per connected patch.
bool FitHeightfield(const WorldMesh& mesh, Building& out) {
    Aabb box;
    for (const Vec3& p : mesh.points) box.Add(p);
    const float spanX = box.max.x - box.min.x, spanY = box.max.y - box.min.y;
    const float cell = std::min(2.0f, std::max(0.5f, std::max(spanX, spanY) / 64.0f));
    const int nx = (int)(spanX / cell) + 3, ny = (int)(spanY / cell) + 3;
    const float x0 = box.min.x - cell, y0 = box.min.y - cell;
    const float none = -1e30f;
    std::vector<float> top((size_t)nx * ny, none);

    for (const Triangle& t : mesh.triangles) {
        if (std::fabs(t.normal.z) < 0.2f) continue;   // walls carry no top
        const Vec2 a{ t.v[0].x, t.v[0].y }, b{ t.v[1].x, t.v[1].y }, c{ t.v[2].x, t.v[2].y };
        const float area = Cross2(a, b, c);
        if (std::fabs(area) < 1e-6f) continue;
        const int i0 = std::max(0, (int)((std::min(a.x, std::min(b.x, c.x)) - x0) / cell));
        const int i1 = std::min(nx - 1, (int)((std::max(a.x, std::max(b.x, c.x)) - x0) / cell));
        const int j0 = std::max(0, (int)((std::min(a.y, std::min(b.y, c.y)) - y0) / cell));
        const int j1 = std::min(ny - 1, (int)((std::max(a.y, std::max(b.y, c.y)) - y0) / cell));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const Vec2 p{ x0 + (i + 0.5f) * cell, y0 + (j + 0.5f) * cell };
                const float wa = Cross2(b, c, p) / area, wb = Cross2(c, a, p) / area;
                const float wc = 1.0f - wa - wb;
                if (wa < -1e-4f || wb < -1e-4f || wc < -1e-4f) continue;
                const float z = wa * t.v[0].z + wb * t.v[1].z + wc * t.v[2].z;
                float& slot = top[(size_t)j * nx + i];
                slot = std::max(slot, z);
            }
    }

    // Heights grouped into at most eight levels, widening the tolerance until
    // they fit. A sloped roof becomes a few terraces, which a minimap cannot
    // tell from the slope.
    const float height = mesh.maxZ - mesh.minZ;
    std::vector<float> heights;
    for (float& z : top) {
        if (z < mesh.minZ + 0.5f) z = none;   // ground, kerbs, awnings at the door
        else heights.push_back(z);
    }
    if (heights.size() < 4) return false;
    std::sort(heights.begin(), heights.end());

    std::vector<float> levels;   // ascending
    for (float tolerance = std::max(1.0f, 0.06f * height);; tolerance *= 1.5f) {
        levels.clear();
        float start = heights[0], sum = 0.0f;
        size_t count = 0;
        for (float z : heights) {
            if (z - start > tolerance) {
                levels.push_back(sum / count);
                start = z; sum = 0.0f; count = 0;
            }
            sum += z; count++;
        }
        levels.push_back(sum / count);
        if (levels.size() <= 8) break;
    }
    std::vector<int> level(top.size(), -1);
    for (size_t k = 0; k < top.size(); ++k) {
        if (top[k] == none) continue;
        int best = 0;
        for (int l = 1; l < (int)levels.size(); ++l)
            if (std::fabs(top[k] - levels[l]) < std::fabs(top[k] - levels[best])) best = l;
        level[k] = best;
    }

    const size_t minCells = std::max<size_t>(3, (size_t)(4.0f / (cell * cell)));
    std::vector<Part> parts;
    std::vector<size_t> partCells;               // how many cells each part covers
    std::vector<int> owner(top.size(), -1);      // the part each cell went to, last level
    for (int l = 0; l < (int)levels.size(); ++l) {
        std::vector<int> owned(top.size(), -1);
        // Connected patches of the cells that reach this level.
        std::vector<int> patch(top.size(), -1);
        int patches = 0;
        for (size_t seed = 0; seed < top.size(); ++seed) {
            if (level[seed] < l || patch[seed] >= 0) continue;
            std::vector<size_t> stack{ seed }, cells;
            patch[seed] = patches;
            while (!stack.empty()) {
                const size_t k = stack.back(); stack.pop_back();
                cells.push_back(k);
                const int i = (int)(k % nx), j = (int)(k / nx);
                const int di[4] = { 1, -1, 0, 0 }, dj[4] = { 0, 0, 1, -1 };
                for (int d = 0; d < 4; ++d) {
                    const int ii = i + di[d], jj = j + dj[d];
                    if (ii < 0 || jj < 0 || ii >= nx || jj >= ny) continue;
                    const size_t n = (size_t)jj * nx + ii;
                    if (level[n] >= l && patch[n] < 0) { patch[n] = patches; stack.push_back(n); }
                }
            }
            const int id = patches++;
            if (cells.size() < minCells) continue;   // a vent, a sign, a lamp

            // A patch that is the same as the one under it is that part
            // going higher, not a part of its own.
            const int below = owner[seed];
            if (below >= 0 && cells.size() * 100 >= partCells[below] * 97) {
                parts[below].height = std::max(0.1f, levels[l] - parts[below].base);
                for (size_t k : cells) owned[k] = below;
                continue;
            }

            // The outline: cell edges with the patch on their left, walked
            // into loops; the largest is the outside.
            std::multimap<std::pair<int, int>, std::pair<int, int>> next;
            const auto in = [&](int i, int j) {
                return i >= 0 && j >= 0 && i < nx && j < ny && patch[(size_t)j * nx + i] == id;
            };
            for (size_t k : cells) {
                const int i = (int)(k % nx), j = (int)(k / nx);
                if (!in(i, j - 1)) next.emplace(std::make_pair(i, j), std::make_pair(i + 1, j));
                if (!in(i + 1, j)) next.emplace(std::make_pair(i + 1, j), std::make_pair(i + 1, j + 1));
                if (!in(i, j + 1)) next.emplace(std::make_pair(i + 1, j + 1), std::make_pair(i, j + 1));
                if (!in(i - 1, j)) next.emplace(std::make_pair(i, j + 1), std::make_pair(i, j));
            }
            std::vector<Vec2> outline;
            float outlineArea = 0.0f;
            while (!next.empty()) {
                const auto start = next.begin()->first;
                std::vector<Vec2> loop;
                auto at = start;
                for (size_t guard = 0; guard < 1000000; ++guard) {
                    const auto it = next.find(at);
                    if (it == next.end()) break;
                    loop.push_back({ x0 + at.first * cell, y0 + at.second * cell });
                    at = it->second;
                    next.erase(it);
                    if (at == start) break;
                }
                const float area = loop.size() >= 3 ? SignedArea(loop) : 0.0f;
                if (area > outlineArea) { outlineArea = area; outline = std::move(loop); }
            }
            // Coarse enough that a round tower costs a couple of dozen
            // points, not one per step of the grid.
            std::vector<Vec2> simple;
            for (float tolerance = std::max(cell, 0.8f);; tolerance *= 1.5f) {
                simple = Simplify(outline, tolerance);
                if (simple.size() <= 32) break;
            }
            if (simple.size() < 3 || SignedArea(simple) < 1.0f) continue;

            Part part;
            part.footprint = std::move(simple);
            part.base = l == 0 ? mesh.minZ : levels[l - 1];
            part.height = std::max(0.1f, levels[l] - part.base);
            for (size_t k : cells) owned[k] = (int)parts.size();
            partCells.push_back(cells.size());
            parts.push_back(std::move(part));
        }
        owner.swap(owned);
    }
    if (parts.empty() || parts.size() > 24) return false;

    out.parts = std::move(parts);
    out.kind = out.parts.size() == 1 ? BuildingKind::Prism : BuildingKind::Parts;
    return true;
}

// --- JSON ----------------------------------------------------------------------

// One decimal, and no ".0": a city's worth of coordinates is most of the file.
void Number(std::string& out, float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", value);
    std::string s = text;
    if (s == "-0.0") s = "0.0";
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    out += s;
}

void Footprint(std::string& out, const std::vector<Vec2>& footprint) {
    out += "\"p\": [";
    for (size_t i = 0; i < footprint.size(); ++i) {
        if (i) out += ", ";
        Number(out, footprint[i].x);
        out += ", ";
        Number(out, footprint[i].y);
    }
    out += "]";
}

void PartJson(std::string& out, const Part& part) {
    Footprint(out, part.footprint);
    out += ", \"z\": ";
    Number(out, part.base);
    out += ", \"h\": ";
    Number(out, part.height);
    if (part.roof.type == RoofType::Flat) return;
    static const char* kNames[] = { "flat", "pyramid", "gable", "dome" };
    out += ", \"roof\": { \"type\": \"";
    out += kNames[(int)part.roof.type];
    out += "\", \"h\": ";
    Number(out, part.roof.height);
    if (part.roof.type == RoofType::Gable) {
        out += ", \"dir\": ";
        Number(out, part.roof.direction);
    }
    out += " }";
}

// --- preview -------------------------------------------------------------------

constexpr u32 kPrismColour = 0xC8D2DC;
constexpr u32 kRoofColour  = 0xE6EBF0;
constexpr u32 kBoxColour   = 0xE8A860;
constexpr u32 kMeshColour  = 0xB898E0;
constexpr u32 kLineColour  = 0xFF28303A;

struct PreviewSink {
    std::vector<PreviewVertex>& triangles;
    std::vector<PreviewVertex>& lines;

    void Triangle(const Vec3& a, const Vec3& b, const Vec3& c, u32 base) {
        const Vec3 n = Normalize(Cross(b - a, c - a));
        if (Length(n) < 0.5f) return;
        const Vec3 light = Normalize({ 0.35f, 0.25f, 0.9f });
        const float shade = 0.4f + 0.6f * std::max(0.0f, Dot(n, light));
        const auto channel = [&](int shift) {
            return (u32)std::min(255.0f, ((base >> shift) & 0xFF) * shade) << shift;
        };
        const u32 colour = 0xFF000000u | channel(16) | channel(8) | channel(0);
        for (const Vec3* v : { &a, &b, &c }) triangles.push_back({ v->x, v->y, v->z, colour });
    }

    void Line(const Vec3& a, const Vec3& b) {
        lines.push_back({ a.x, a.y, a.z, kLineColour });
        lines.push_back({ b.x, b.y, b.z, kLineColour });
    }
};

Vec3 At(const Vec2& p, float z) { return { p.x, p.y, z }; }

void PreviewRoof(PreviewSink& sink, const Part& part, float top) {
    const std::vector<Vec2>& poly = part.footprint;
    const Roof& roof = part.roof;
    const Vec2 c = Centroid(poly);

    switch (roof.type) {
    case RoofType::Flat: {
        const std::vector<u32> tris = Triangulate(poly);
        for (size_t i = 0; i + 2 < tris.size(); i += 3)
            sink.Triangle(At(poly[tris[i]], top), At(poly[tris[i + 1]], top), At(poly[tris[i + 2]], top),
                          kRoofColour);
        break;
    }
    case RoofType::Pyramid: {
        const Vec3 apex = At(c, top + roof.height);
        for (size_t i = 0; i < poly.size(); ++i)
            sink.Triangle(At(poly[i], top), At(poly[(i + 1) % poly.size()], top), apex, kRoofColour);
        break;
    }
    case RoofType::Dome: {
        constexpr int kRings = 4;
        std::vector<Vec3> previous;
        for (const Vec2& p : poly) previous.push_back(At(p, top));
        for (int r = 1; r <= kRings; ++r) {
            const float t = (float)r / kRings * kPi * 0.5f;
            const float s = std::cos(t);
            const float z = top + roof.height * std::sin(t);
            if (r == kRings) {
                const Vec3 apex = At(c, z);
                for (size_t i = 0; i < previous.size(); ++i)
                    sink.Triangle(previous[i], previous[(i + 1) % previous.size()], apex, kRoofColour);
                break;
            }
            std::vector<Vec3> ring;
            for (const Vec2& p : poly) ring.push_back(At({ c.x + (p.x - c.x) * s, c.y + (p.y - c.y) * s }, z));
            for (size_t i = 0; i < ring.size(); ++i) {
                const size_t j = (i + 1) % ring.size();
                sink.Triangle(previous[i], previous[j], ring[j], kRoofColour);
                sink.Triangle(previous[i], ring[j], ring[i], kRoofColour);
            }
            previous = std::move(ring);
        }
        break;
    }
    case RoofType::Gable: {
        // Height falls off linearly from the ridge line, so each half of the
        // footprint is one plane, and the end walls fill up to it.
        const float angle = roof.direction * kPi / 180.0f;
        const Vec2 across{ -std::sin(angle), std::cos(angle) };
        float reach = 0.0f;
        for (const Vec2& p : poly)
            reach = std::max(reach, std::fabs((p.x - c.x) * across.x + (p.y - c.y) * across.y));
        if (reach < 1e-3f) break;
        const auto height = [&](const Vec2& p) {
            const float d = std::fabs((p.x - c.x) * across.x + (p.y - c.y) * across.y);
            return top + roof.height * (1.0f - d / reach);
        };
        for (const Vec2& side : { across, Vec2{ -across.x, -across.y } }) {
            const std::vector<Vec2> half = ClipHalf(poly, c, side);
            if (half.size() < 3) continue;
            for (size_t i = 1; i + 1 < half.size(); ++i)
                sink.Triangle(At(half[0], height(half[0])), At(half[i], height(half[i])),
                              At(half[i + 1], height(half[i + 1])), kRoofColour);
            for (size_t i = 0; i < half.size(); ++i) {
                const Vec2& a = half[i];
                const Vec2& b = half[(i + 1) % half.size()];
                const float ha = height(a), hb = height(b);
                if (ha - top < 0.05f && hb - top < 0.05f) continue;
                const float da = (a.x - c.x) * side.x + (a.y - c.y) * side.y;
                const float db = (b.x - c.x) * side.x + (b.y - c.y) * side.y;
                if (std::fabs(da) < 1e-3f && std::fabs(db) < 1e-3f) continue;   // the ridge itself
                sink.Triangle(At(a, top), At(b, top), At(b, hb), kPrismColour);
                sink.Triangle(At(a, top), At(b, hb), At(a, ha), kPrismColour);
            }
        }
        break;
    }
    }
}

} // namespace

bool IsMinimapBuilding(const nfs::SolidObject& model) {
    return model.name.rfind("XB_", 0) == 0 || model.name.rfind("XBU_", 0) == 0;
}

bool BuildBuilding(const PlacedModel& placed, Building& out) {
    if (!placed.model || placed.model->vertices.empty()) return false;
    const WorldMesh mesh = ToWorld(placed);
    if (mesh.triangles.empty()) return false;

    if (mesh.maxZ - mesh.minZ < 1.0f || IsFacade(mesh, placed.transform)) {
        out = BoxOf(placed, mesh);
        return true;
    }
    Building parts;
    if (FitParts(mesh, parts)) { out = std::move(parts); return true; }
    if (FitHeightfield(mesh, parts)) { out = std::move(parts); return true; }
    out = MeshOf(mesh);
    return !out.meshTriangles.empty();
}

std::vector<Building> BuildAll(const std::vector<PlacedModel>& placed, GenerateSummary& summary) {
    std::vector<Building> buildings;
    buildings.reserve(placed.size());
    for (const PlacedModel& one : placed) {
        Building building;
        if (!BuildBuilding(one, building)) { summary.skipped++; continue; }
        switch (building.kind) {
            case BuildingKind::Prism: summary.prisms++; break;
            case BuildingKind::Parts: summary.parts++;  break;
            case BuildingKind::Box:   summary.boxes++;  break;
            case BuildingKind::Mesh:  summary.meshes++; break;
        }
        buildings.push_back(std::move(building));
    }
    LOG_INFO("minimap buildings: %zu prisms, %zu stacked, %zu boxes, %zu meshes, %zu skipped",
             summary.prisms, summary.parts, summary.boxes, summary.meshes, summary.skipped);
    return buildings;
}

bool WriteJson(const std::vector<Building>& buildings, const std::wstring& path) {
    std::string out = "{ \"buildings\": [\n";
    for (size_t b = 0; b < buildings.size(); ++b) {
        const Building& building = buildings[b];
        out += "  { ";
        if (building.kind == BuildingKind::Mesh) {
            out += "\"mesh\": { \"v\": [";
            for (size_t i = 0; i < building.meshVertices.size(); ++i) {
                if (i) out += ", ";
                Number(out, building.meshVertices[i].x); out += ", ";
                Number(out, building.meshVertices[i].y); out += ", ";
                Number(out, building.meshVertices[i].z);
            }
            out += "], \"f\": [";
            for (size_t i = 0; i + 2 < building.meshTriangles.size(); i += 3) {
                if (i) out += ", ";
                out += "[" + std::to_string(building.meshTriangles[i]) + ", " +
                       std::to_string(building.meshTriangles[i + 1]) + ", " +
                       std::to_string(building.meshTriangles[i + 2]) + "]";
            }
            out += "] }";
        } else if (building.parts.size() == 1 && building.parts[0].roof.type == RoofType::Flat) {
            PartJson(out, building.parts[0]);
        } else {
            out += "\"parts\": [";
            for (size_t i = 0; i < building.parts.size(); ++i) {
                out += i ? ", { " : " { ";
                PartJson(out, building.parts[i]);
                out += " }";
            }
            out += " ]";
        }
        out += b + 1 < buildings.size() ? " },\n" : " }\n";
    }
    out += "] }\n";

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        LOG_ERROR("could not write %ls", path.c_str());
        return false;
    }
    const bool ok = std::fwrite(out.data(), 1, out.size(), file) == out.size();
    std::fclose(file);
    LOG_INFO("minimap buildings: wrote %zu buildings, %zu bytes to %ls",
             buildings.size(), out.size(), path.c_str());
    return ok;
}

void BuildPreview(const std::vector<Building>& buildings,
                  std::vector<PreviewVertex>& triangles,
                  std::vector<PreviewVertex>& lines) {
    triangles.clear();
    lines.clear();
    PreviewSink sink{ triangles, lines };

    for (const Building& building : buildings) {
        if (building.kind == BuildingKind::Mesh) {
            const auto& v = building.meshVertices;
            const auto& f = building.meshTriangles;
            for (size_t i = 0; i + 2 < f.size(); i += 3)
                sink.Triangle(v[f[i]], v[f[i + 1]], v[f[i + 2]], kMeshColour);
            continue;
        }
        const u32 wall = building.kind == BuildingKind::Box ? kBoxColour : kPrismColour;
        for (const Part& part : building.parts) {
            const auto& poly = part.footprint;
            const float bottom = part.base, top = part.base + part.height;
            for (size_t i = 0; i < poly.size(); ++i) {
                const Vec2& a = poly[i];
                const Vec2& b = poly[(i + 1) % poly.size()];
                sink.Triangle(At(a, bottom), At(b, bottom), At(b, top), wall);
                sink.Triangle(At(a, bottom), At(b, top), At(a, top), wall);
                sink.Line(At(a, top), At(b, top));
                // Corners only where the wall turns: a curved wall drawn with
                // an edge at every point reads as a column of stripes.
                const Vec2& before = poly[(i + poly.size() - 1) % poly.size()];
                const float ex = a.x - before.x, ey = a.y - before.y;
                const float fx = b.x - a.x, fy = b.y - a.y;
                const float lengths = std::sqrt((ex * ex + ey * ey) * (fx * fx + fy * fy));
                if (lengths > 1e-6f && std::fabs(ex * fy - ey * fx) > 0.34f * lengths)
                    sink.Line(At(a, bottom), At(a, top));
            }
            if (building.kind == BuildingKind::Box) {
                const std::vector<u32> tris = Triangulate(poly);
                for (size_t i = 0; i + 2 < tris.size(); i += 3)
                    sink.Triangle(At(poly[tris[i]], top), At(poly[tris[i + 1]], top),
                                  At(poly[tris[i + 2]], top), kBoxColour);
            } else {
                PreviewRoof(sink, part, top);
            }
        }
    }
}

} // namespace minimap
