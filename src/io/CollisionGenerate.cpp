#include "io/CollisionGenerate.h"

#include "io/CollisionEdit.h"
#include "nfs/FreeRoam.h"
#include "nfs/SolidList.h"
#include "nfs/World.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace collision {

namespace {

bool NameHas(const std::string& upper, const char* word) { return upper.find(word) != std::string::npos; }

u8 WallSurfaceFor(const std::string& name) {
    std::string n = name;
    for (char& c : n) c = (char)std::toupper((unsigned char)c);
    // Words of the name (XO_STREETLIGHTC_1A_00 -> XO, STREETLIGHTC, 1A, 00):
    // a plant has to start one, or every STREET would be a tree.
    const auto startsWord = [&n](const char* word) {
        for (size_t at = n.find(word); at != std::string::npos; at = n.find(word, at + 1))
            if (at == 0 || n[at - 1] == '_') return true;
        return false;
    };
    if (NameHas(n, "POLE") || NameHas(n, "POST") || NameHas(n, "LIGHT") || NameHas(n, "LAMP") ||
        NameHas(n, "SIGN") || NameHas(n, "HYDRANT") || NameHas(n, "METER"))
        return 23;   // POST
    if (startsWord("TREE") || startsWord("BUSH") || startsWord("PLANT") || startsWord("HEDGE") ||
        startsWord("PALM"))
        return 22;   // PLANT
    if (NameHas(n, "FENCE") || NameHas(n, "CHAIN")) return 27;   // CHAINLINK
    if (NameHas(n, "GLASS") || NameHas(n, "WINDOW")) return 19;  // GLASS
    if (NameHas(n, "WOOD") || NameHas(n, "CRATE") || NameHas(n, "PALLET")) return 17;   // WOOD
    if (NameHas(n, "PILLAR") || NameHas(n, "COLUMN")) return 24;   // PILLAR
    return nfs::kSurfaceSolidWall;
}

double Cross2(const Vec2& o, const Vec2& a, const Vec2& b) {
    return (double)(a.x - o.x) * (b.y - o.y) - (double)(a.y - o.y) * (b.x - o.x);
}

// Andrew's monotone chain; counter-clockwise, no repeat of the first point.
std::vector<Vec2> ConvexHull(std::vector<Vec2> points) {
    std::sort(points.begin(), points.end(), [](const Vec2& a, const Vec2& b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    points.erase(std::unique(points.begin(), points.end(), [](const Vec2& a, const Vec2& b) {
                     return std::fabs(a.x - b.x) < 0.05f && std::fabs(a.y - b.y) < 0.05f;
                 }), points.end());
    if (points.size() < 3) return points;
    std::vector<Vec2> hull(points.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        while (k >= 2 && Cross2(hull[k - 2], hull[k - 1], points[i]) <= 1e-6) --k;
        hull[k++] = points[i];
    }
    for (size_t i = points.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && Cross2(hull[k - 2], hull[k - 1], points[i]) <= 1e-6) --k;
        hull[k++] = points[i];
    }
    hull.resize(k - 1);
    return hull;
}

} // namespace

GenerateResult GenerateObjectCollision(const nfs::SolidObject& model, const Mat4& transform,
                                       const nfs::World* world,
                                       std::vector<nfs::CollisionPolygon>& out) {
    GenerateResult result;
    const auto& vertices = model.vertices;
    const auto& indices = model.indices;
    if (vertices.empty() || indices.size() < 3) {
        result.note = L"the model has no mesh";
        return result;
    }

    // --- the mesh in the world ------------------------------------------------
    std::vector<Vec3> points(vertices.size());
    Aabb box;
    for (size_t i = 0; i < vertices.size(); ++i) {
        points[i] = transform.TransformPoint(vertices[i].position);
        box.Add(points[i]);
    }
    const float minZ = box.min.z, maxZ = box.max.z;
    const Vec3 centre = (box.min + box.max) * 0.5f;

    // The foot: the ground under the middle when the object stands on it or
    // is sunk into it, else its own lowest point.
    float foot = minZ;
    float ground = 0.0f;
    if (world && nfs::GroundBelow(*world, centre.x, centre.y, minZ + 1.0f, ground) &&
        ground > minZ - 3.0f && ground < maxZ)
        foot = std::max(minZ, ground);
    const float height = maxZ - foot;

    // --- which way is up for this mesh's triangles ------------------------------
    // The winding is the renderer's business, so the sign of a face normal is
    // settled by a vote: against the stored vertex normals when the model has
    // them, else by the highest faces, which have to face up.
    struct Face { Vec3 a, b, c; float nz; };
    std::vector<Face> faces;
    faces.reserve(indices.size() / 3);
    double vote = 0.0;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const u16 i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (i0 >= points.size() || i1 >= points.size() || i2 >= points.size()) continue;
        const Vec3 &a = points[i0], &b = points[i1], &c = points[i2];
        const Vec3 n = Cross(b - a, c - a);
        const float length = Length(n);
        if (length < 1e-5f) continue;
        Face face{ a, b, c, n.z / length };
        if (model.hasNormals) {
            const Vec3 stored = transform.TransformDirection(vertices[i0].normal + vertices[i1].normal +
                                                             vertices[i2].normal);
            vote += Dot(n, stored) >= 0.0f ? length : -length;
        }
        faces.push_back(face);
    }
    if (!model.hasNormals) {
        const float top = maxZ - std::max(0.1f, (maxZ - minZ) * 0.1f);
        for (const Face& f : faces)
            if (std::min({ f.a.z, f.b.z, f.c.z }) >= top && std::fabs(f.nz) > 0.7f) vote += f.nz;
    }
    const float up = vote < 0.0 ? -1.0f : 1.0f;

    // --- solid or drivable --------------------------------------------------------
    bool drivable = height <= 0.6f;
    for (const Face& f : faces) {
        const float nz = f.nz * up;
        if (nz <= 0.6f || nz >= 0.98f) continue;
        const float low = std::min({ f.a.z, f.b.z, f.c.z }), high = std::max({ f.a.z, f.b.z, f.c.z });
        if (low <= foot + 0.6f && high >= foot + 0.3f) { drivable = true; break; }
    }
    result.shape = drivable ? GeneratedShape::Drivable : GeneratedShape::Solid;

    // --- ground on the upward faces ---------------------------------------------
    const auto addGround = [&](float above) {
        for (const Face& f : faces) {
            if (f.nz * up <= 0.6f) continue;
            if ((f.a.z + f.b.z + f.c.z) / 3.0f < foot + above) continue;
            if (result.ground >= kMaxGround) { result.groundCapped = true; break; }
            const Vec3 corners[3] = { f.a, f.b, f.c };
            out.push_back(MakeGround(corners, 3, nfs::kSurfaceRoad));
            ++result.ground;
        }
    };

    if (drivable) {
        addGround(-1.0f);
        if (!result.ground) result.note = L"no face of it looks up";
        return result;
    }

    // --- walls round the part a car can hit ---------------------------------------
    const float band = foot + std::min(2.5f, std::max(height, 0.5f));
    std::vector<Vec2> plan;
    for (const Vec3& p : points)
        if (p.z <= band && p.z >= foot - 1.0f) plan.push_back({ p.x, p.y });
    if (plan.size() < 2)
        for (const Vec3& p : points) plan.push_back({ p.x, p.y });

    std::vector<Vec2> hull = ConvexHull(plan);
    double area = 0.0;
    for (size_t i = 0; i < hull.size(); ++i) {
        const Vec2& a = hull[i];
        const Vec2& b = hull[(i + 1) % hull.size()];
        area += (double)a.x * b.y - (double)b.x * a.y;
    }
    area = std::fabs(area) * 0.5;

    WallOptions options;
    options.kind = WallKind::Vertical;
    options.surface = WallSurfaceFor(model.name);
    options.height = std::clamp(maxZ - foot, 0.5f, 100.0f);
    options.sink = 0.5f;

    std::vector<Vec3> outline;
    if (hull.size() >= 3 && area > 0.02) {
        for (const Vec2& p : hull) outline.push_back({ p.x, p.y, foot });
        outline.push_back(outline.front());
    } else {
        // A thin thing -- a fence panel, a sign: one wall along its length.
        Vec2 lo = plan.front(), hi = plan.front();
        float best = -1.0f;
        for (const Vec2& a : hull)
            for (const Vec2& b : hull) {
                const float d = (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
                if (d > best) { best = d; lo = a; hi = b; }
            }
        if (best < 0.04f) {
            result.note = L"it is too small to stop a car";
            return result;
        }
        outline = { { lo.x, lo.y, foot }, { hi.x, hi.y, foot } };
    }
    const size_t before = out.size();
    AppendWall(outline, options, out);
    result.walls = out.size() - before;

    // Roofs and tops a car may land on.
    addGround(0.75f);
    return result;
}

} // namespace collision
