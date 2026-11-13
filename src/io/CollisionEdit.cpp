#include "io/CollisionEdit.h"

#include "core/Log.h"
#include "nfs/ChunkReader.h"
#include "nfs/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace collision {

namespace {

constexpr u8 kFlagVertical = 0x02;
constexpr u8 kFlagQuad     = 0x10;
constexpr u8 kFlagsBarrier = 0x16;   // every shipped barrier

double Cross2(const Vec3& o, const Vec3& a, const Vec3& b) {
    return (double)(a.x - o.x) * (b.y - o.y) - (double)(a.y - o.y) * (b.x - o.x);
}

double PlanArea(const std::vector<Vec3>& points) {
    double twice = 0.0;
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec3& a = points[i];
        const Vec3& b = points[(i + 1) % points.size()];
        twice += (double)a.x * b.y - (double)b.x * a.y;
    }
    return twice * 0.5;
}

bool ConvexInPlan(const Vec3* p, int count) {
    int positive = 0, negative = 0;
    for (int i = 0; i < count; ++i) {
        const double turn = Cross2(p[i], p[(i + 1) % count], p[(i + 2) % count]);
        if (turn > 1e-6) ++positive;
        else if (turn < -1e-6) ++negative;
    }
    return !(positive && negative) && (positive + negative) >= 3;
}

bool SamePlan(const Vec3& a, const Vec3& b) {
    return std::fabs(a.x - b.x) < 0.0625f && std::fabs(a.y - b.y) < 0.0625f;
}

// Point in triangle, in plan, edges included.
bool InTriangle(float x, float y, const Vec3& a, const Vec3& b, const Vec3& c) {
    const double d1 = (double)(x - b.x) * (a.y - b.y) - (double)(a.x - b.x) * (y - b.y);
    const double d2 = (double)(x - c.x) * (b.y - c.y) - (double)(b.x - c.x) * (y - c.y);
    const double d3 = (double)(x - a.x) * (c.y - a.y) - (double)(c.x - a.x) * (y - a.y);
    const bool negative = d1 < -1e-6 || d2 < -1e-6 || d3 < -1e-6;
    const bool positive = d1 > 1e-6 || d2 > 1e-6 || d3 > 1e-6;
    return !(negative && positive);
}

bool OverGround(const nfs::CollisionPolygon& polygon, float x, float y) {
    if (polygon.barrier || polygon.Vertical()) return false;
    const Vec3* c = polygon.corners;
    if (InTriangle(x, y, c[0], c[1], c[2])) return true;
    return polygon.quad && InTriangle(x, y, c[0], c[2], c[3]);
}

// The stored form of a polygon's shape, for finding the copies of it other
// packs hold: corners in the file's units, plus whether it is a barrier.
struct ShapeKey {
    i32 v[12] = {};
    bool barrier = false;
    bool operator<(const ShapeKey& o) const {
        if (barrier != o.barrier) return barrier < o.barrier;
        return std::memcmp(v, o.v, sizeof(v)) < 0;
    }
    bool operator==(const ShapeKey& o) const {
        return barrier == o.barrier && std::memcmp(v, o.v, sizeof(v)) == 0;
    }
};

ShapeKey KeyOf(const nfs::CollisionPolygon& polygon) {
    ShapeKey key;
    key.barrier = polygon.barrier;
    for (int c = 0; c < 4; ++c) {
        const int from = polygon.barrier ? (c < 2 ? 0 : 2) : c;
        key.v[c * 3 + 0] = (i32)std::lround(polygon.corners[from].x * 8.0f);
        key.v[c * 3 + 1] = (i32)std::lround(polygon.corners[from].y * 8.0f);
        key.v[c * 3 + 2] = polygon.barrier ? 0 : (i32)std::lround(polygon.corners[from].z * 256.0f);
    }
    return key;
}

struct ShapeKeyHash {
    size_t operator()(const ShapeKey& key) const {
        size_t h = key.barrier ? 0x9E3779B9u : 0;
        for (i32 value : key.v) h = h * 1000003u ^ (size_t)(u32)value;
        return h;
    }
};

void Finish(nfs::CollisionPolygon& polygon) {
    polygon.SyncFromAttributes();
}

} // namespace

// --- making polygons ---------------------------------------------------------

nfs::CollisionPolygon MakeGround(const Vec3* corners, int count, u8 surface) {
    nfs::CollisionPolygon polygon;
    count = std::clamp(count, 3, 4);
    polygon.quad = count == 4;
    for (int c = 0; c < 4; ++c) polygon.corners[c] = corners[std::min(c, count - 1)];
    std::vector<Vec3> outline(corners, corners + count);
    const bool vertical = std::fabs(PlanArea(outline)) < 1e-3;
    polygon.attributes[0] = 0;
    polygon.attributes[2] = surface;
    polygon.attributes[3] = (u8)((polygon.quad ? kFlagQuad : 0) | (vertical ? kFlagVertical : 0));
    Finish(polygon);
    return polygon;
}

nfs::CollisionPolygon MakeBarrier(const Vec3& from, const Vec3& to, u8 surface) {
    nfs::CollisionPolygon polygon;
    polygon.barrier = true;
    polygon.quad = true;
    polygon.corners[0] = polygon.corners[1] = Vec3{ from.x, from.y, 0.0f };
    polygon.corners[2] = polygon.corners[3] = Vec3{ to.x, to.y, 0.0f };
    polygon.attributes[0] = 0;
    polygon.attributes[2] = surface;
    polygon.attributes[3] = kFlagsBarrier;
    Finish(polygon);
    return polygon;
}

nfs::CollisionPolygon MakeVerticalWall(const Vec3& from, const Vec3& to, float height, u8 surface) {
    nfs::CollisionPolygon polygon;
    polygon.quad = true;
    polygon.corners[0] = from;
    polygon.corners[1] = Vec3{ from.x, from.y, from.z + height };
    polygon.corners[2] = Vec3{ to.x, to.y, to.z + height };
    polygon.corners[3] = to;
    polygon.attributes[0] = 0;
    polygon.attributes[2] = surface;
    polygon.attributes[3] = kFlagQuad | kFlagVertical;
    Finish(polygon);
    return polygon;
}

void AppendWall(const std::vector<Vec3>& points, const WallOptions& options,
                std::vector<nfs::CollisionPolygon>& out) {
    // A file height range of +/-127 around the polygon's middle.
    const float height = std::clamp(options.height, 0.25f, 100.0f);
    const float sink = std::clamp(options.sink, 0.0f, 20.0f);
    const float piece = std::max(1.0f, options.maxPiece);

    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const Vec3 a = points[i], b = points[i + 1];
        const float length = std::hypot(b.x - a.x, b.y - a.y);
        if (length < 0.25f) continue;
        const int pieces = std::max(1, (int)std::ceil(length / piece));
        for (int k = 0; k < pieces; ++k) {
            const float t0 = (float)k / pieces, t1 = (float)(k + 1) / pieces;
            const Vec3 p = a + (b - a) * t0, q = a + (b - a) * t1;
            if (options.kind == WallKind::Barrier) {
                out.push_back(MakeBarrier(p, q, options.surface));
                if (options.bothSides) out.push_back(MakeBarrier(q, p, options.surface));
            } else {
                const Vec3 footP{ p.x, p.y, p.z - sink }, footQ{ q.x, q.y, q.z - sink };
                out.push_back(MakeVerticalWall(footP, footQ, height + sink, options.surface));
                if (options.bothSides)
                    out.push_back(MakeVerticalWall(footQ, footP, height + sink, options.surface));
            }
        }
    }
}

bool AppendGround(const std::vector<Vec3>& outline, u8 surface,
                  std::vector<nfs::CollisionPolygon>& out) {
    std::vector<Vec3> points;
    for (const Vec3& p : outline)
        if (points.empty() || !SamePlan(points.back(), p)) points.push_back(p);
    while (points.size() > 1 && SamePlan(points.front(), points.back())) points.pop_back();
    if (points.size() < 3 || std::fabs(PlanArea(points)) < 0.05) return false;

    if (points.size() == 3) {
        out.push_back(MakeGround(points.data(), 3, surface));
        return true;
    }
    if (points.size() == 4 && ConvexInPlan(points.data(), 4)) {
        out.push_back(MakeGround(points.data(), 4, surface));
        return true;
    }

    // Ear clipping, in plan, on a counter-clockwise copy.
    if (PlanArea(points) < 0.0) std::reverse(points.begin(), points.end());
    std::vector<size_t> left(points.size());
    for (size_t i = 0; i < left.size(); ++i) left[i] = i;
    size_t guard = 0;
    while (left.size() > 3 && guard++ < 10000) {
        bool clipped = false;
        for (size_t i = 0; i < left.size(); ++i) {
            const size_t ia = left[(i + left.size() - 1) % left.size()];
            const size_t ib = left[i];
            const size_t ic = left[(i + 1) % left.size()];
            const Vec3 &a = points[ia], &b = points[ib], &c = points[ic];
            if (Cross2(a, b, c) <= 1e-6) continue;   // reflex or flat
            bool inside = false;
            for (size_t j : left) {
                if (j == ia || j == ib || j == ic) continue;
                if (InTriangle(points[j].x, points[j].y, a, b, c)) { inside = true; break; }
            }
            if (inside) continue;
            const Vec3 tri[3] = { a, b, c };
            out.push_back(MakeGround(tri, 3, surface));
            left.erase(left.begin() + (ptrdiff_t)i);
            clipped = true;
            break;
        }
        if (!clipped) break;   // not a simple outline: fan what is left
    }
    for (size_t i = 1; i + 1 < left.size(); ++i) {
        const Vec3 tri[3] = { points[left[0]], points[left[i]], points[left[i + 1]] };
        if (std::fabs(Cross2(tri[0], tri[1], tri[2])) > 1e-6) out.push_back(MakeGround(tri, 3, surface));
    }
    return true;
}

void AppendStrip(const std::vector<Vec3>& left, const std::vector<Vec3>& right, u8 surface,
                 std::vector<nfs::CollisionPolygon>& out) {
    const size_t n = std::min(left.size(), right.size());
    for (size_t i = 0; i + 1 < n; ++i) {
        const Vec3 quad[4] = { left[i], left[i + 1], right[i + 1], right[i] };
        if (ConvexInPlan(quad, 4)) {
            out.push_back(MakeGround(quad, 4, surface));
        } else {
            const Vec3 a[3] = { quad[0], quad[1], quad[2] };
            const Vec3 b[3] = { quad[0], quad[2], quad[3] };
            if (std::fabs(Cross2(a[0], a[1], a[2])) > 1e-6) out.push_back(MakeGround(a, 3, surface));
            if (std::fabs(Cross2(b[0], b[1], b[2])) > 1e-6) out.push_back(MakeGround(b, 3, surface));
        }
    }
}

// --- changing the world -------------------------------------------------------

void Editor::SetWorld(nfs::World* world) {
    world_ = world;
    undo_.clear();
    touched_.clear();
    moving_ = false;
    moveRefs_.clear();
    moveStart_.clear();
}

nfs::CollisionPack* Editor::Pack(u32 section, u32 pack) const {
    if (!world_) return nullptr;
    nfs::LoadedSection* loaded = world_->MutableLoaded(section);
    if (!loaded || pack >= loaded->collision.size()) return nullptr;
    return &loaded->collision[pack];
}

void Editor::Remember(const std::wstring& label, std::vector<std::pair<u32, u32>> packs) {
    std::sort(packs.begin(), packs.end());
    packs.erase(std::unique(packs.begin(), packs.end()), packs.end());
    Step step;
    step.label = label;
    for (const auto& [section, pack] : packs) {
        const nfs::CollisionPack* data = Pack(section, pack);
        if (!data) continue;
        const nfs::LoadedSection& loaded = *world_->Loaded()[section];
        step.packs.push_back({ section, pack, *data, loaded.collisionChanged, loaded.changedCollisionPacks });
    }
    undo_.push_back(std::move(step));
    ++recorded_;
    // Packs are a few hundred kilobytes at most; fifty steps is plenty.
    if (undo_.size() > 50) undo_.erase(undo_.begin());
}

void Editor::Changed(u32 section, u32 pack) {
    nfs::LoadedSection* loaded = world_->MutableLoaded(section);
    if (!loaded) return;
    loaded->MarkCollisionChanged(pack);
    if (std::find(touched_.begin(), touched_.end(), section) == touched_.end())
        touched_.push_back(section);
}

std::vector<u32> Editor::TakeTouchedSections() {
    std::vector<u32> out;
    out.swap(touched_);
    return out;
}

bool Editor::Undo() {
    if (undo_.empty() || !world_) return false;
    if (moving_) EndMove(false);
    Step step = std::move(undo_.back());
    undo_.pop_back();
    ++undone_;
    for (PackState& state : step.packs) {
        nfs::CollisionPack* pack = Pack(state.section, state.pack);
        nfs::LoadedSection* loaded = world_->MutableLoaded(state.section);
        if (!pack || !loaded) continue;
        *pack = std::move(state.data);
        loaded->collisionChanged = state.sectionChanged;
        loaded->changedCollisionPacks = state.changedPacks;
        if (std::find(touched_.begin(), touched_.end(), state.section) == touched_.end())
            touched_.push_back(state.section);
    }
    LOG_INFO("collision: undid \"%S\"", step.label.c_str());
    return true;
}

std::vector<std::pair<u32, u32>> Editor::PacksFor(const nfs::CollisionPolygon& polygon) const {
    std::vector<std::pair<u32, u32>> out;
    if (!world_) return out;

    // Where to look: the corners, the middle, and for walls the middle of the
    // segment -- what decides which sections' ground it stands on.
    std::vector<Vec3> samples;
    const int corners = polygon.barrier ? 0 : (polygon.quad ? 4 : 3);
    Vec3 middle{};
    if (polygon.barrier) {
        samples = { polygon.corners[0], polygon.corners[2] };
        middle = (polygon.corners[0] + polygon.corners[2]) * 0.5f;
    } else {
        for (int c = 0; c < corners; ++c) { samples.push_back(polygon.corners[c]); middle = middle + polygon.corners[c]; }
        middle = middle * (1.0f / (float)corners);
    }
    samples.push_back(middle);
    // A sample on the very edge of the ground still counts as over it; one a
    // little inside is what the car will actually be on.
    for (size_t i = 0, n = samples.size(); i + 1 < n; ++i)
        samples.push_back(samples[i] + (middle - samples[i]) * 0.05f);

    Aabb plan;
    for (const Vec3& s : samples) plan.Add(s);

    const auto& loaded = world_->Loaded();
    float nearest = 1e30f;
    std::pair<u32, u32> nearestPack{ UINT32_MAX, UINT32_MAX };

    for (size_t s = 0; s < loaded.size(); ++s) {
        const nfs::LoadedSection& section = *loaded[s];
        for (size_t p = 0; p < section.collision.size(); ++p) {
            const nfs::CollisionPack& pack = section.collision[p];
            if (pack.polygons.empty()) continue;
            // The header bounds, padded: they are from the file, and edits
            // only ever move the ground a little.
            const Aabb& b = pack.bounds;
            if (b.Valid()) {
                const float cx = std::clamp(middle.x, b.min.x, b.max.x) - middle.x;
                const float cy = std::clamp(middle.y, b.min.y, b.max.y) - middle.y;
                const float distance = cx * cx + cy * cy;
                if (distance < nearest) { nearest = distance; nearestPack = { (u32)s, (u32)p }; }
                if (plan.max.x < b.min.x - 40.0f || plan.min.x > b.max.x + 40.0f ||
                    plan.max.y < b.min.y - 40.0f || plan.min.y > b.max.y + 40.0f)
                    continue;
            }
            bool hit = false;
            for (const nfs::CollisionPolygon& ground : pack.polygons) {
                for (const Vec3& sample : samples)
                    if (OverGround(ground, sample.x, sample.y)) { hit = true; break; }
                if (hit) break;
            }
            if (hit) out.push_back({ (u32)s, (u32)p });
        }
    }
    if (out.empty() && nearestPack.first != UINT32_MAX) out.push_back(nearestPack);
    return out;
}

std::vector<PolygonRef> Editor::Add(const std::vector<nfs::CollisionPolygon>& polygons,
                                    const std::wstring& label) {
    std::vector<PolygonRef> added;
    if (!world_ || polygons.empty()) return added;

    std::vector<std::vector<std::pair<u32, u32>>> targets(polygons.size());
    std::vector<std::pair<u32, u32>> all;
    for (size_t i = 0; i < polygons.size(); ++i) {
        targets[i] = PacksFor(polygons[i]);
        all.insert(all.end(), targets[i].begin(), targets[i].end());
    }
    if (all.empty()) return added;
    Remember(label, all);

    // The shapes each target pack already holds.
    std::map<std::pair<u32, u32>, std::set<ShapeKey>> present;
    for (const auto& target : all) {
        if (present.count(target)) continue;
        auto& keys = present[target];
        if (const nfs::CollisionPack* pack = Pack(target.first, target.second))
            for (const nfs::CollisionPolygon& polygon : pack->polygons) keys.insert(KeyOf(polygon));
    }

    for (size_t i = 0; i < polygons.size(); ++i) {
        const ShapeKey key = KeyOf(polygons[i]);
        for (const auto& [section, packIndex] : targets[i]) {
            nfs::CollisionPack* pack = Pack(section, packIndex);
            if (!pack) continue;
            if (!present[{ section, packIndex }].insert(key).second) continue;   // already there
            if (pack->gridRank.size() != pack->polygons.size()) {
                pack->gridRank.resize(pack->polygons.size());
                for (u32 k = 0; k < pack->gridRank.size(); ++k) pack->gridRank[k] = k;
            }
            const u32 rank = pack->gridRank.empty()
                                 ? 0 : *std::max_element(pack->gridRank.begin(), pack->gridRank.end()) + 1;
            nfs::CollisionPolygon polygon = polygons[i];
            polygon.SyncFromAttributes();
            pack->polygons.push_back(polygon);
            pack->gridRank.push_back(rank);
            added.push_back({ section, packIndex, (u32)pack->polygons.size() - 1 });
            Changed(section, packIndex);
        }
    }
    LOG_INFO("collision: %S -- %zu polygons in, %zu stored (edge copies included)",
             label.c_str(), polygons.size(), added.size());
    return added;
}

std::vector<PolygonRef> Editor::CopiesOf(const PolygonRef& ref) const {
    std::vector<PolygonRef> out;
    const nfs::CollisionPack* home = Pack(ref.section, ref.pack);
    if (!home || ref.polygon >= home->polygons.size()) return out;
    const ShapeKey key = KeyOf(home->polygons[ref.polygon]);
    const Vec3 at = home->polygons[ref.polygon].corners[0];
    const auto& loaded = world_->Loaded();
    for (size_t s = 0; s < loaded.size(); ++s)
        for (size_t p = 0; p < loaded[s]->collision.size(); ++p) {
            const nfs::CollisionPack& pack = loaded[s]->collision[p];
            const Aabb& b = pack.bounds;
            if (b.Valid() && (at.x < b.min.x - 1.0f || at.x > b.max.x + 1.0f ||
                              at.y < b.min.y - 1.0f || at.y > b.max.y + 1.0f) &&
                !(s == ref.section && p == ref.pack))
                continue;
            for (size_t i = 0; i < pack.polygons.size(); ++i)
                if (KeyOf(pack.polygons[i]) == key) out.push_back({ (u32)s, (u32)p, (u32)i });
        }
    return out;
}

std::vector<PolygonRef> Editor::WithCopies(const std::vector<PolygonRef>& refs) const {
    std::vector<PolygonRef> out;
    if (!world_ || refs.empty()) return out;
    if (refs.size() <= 8) {
        for (const PolygonRef& ref : refs)
            for (const PolygonRef& copy : CopiesOf(ref))
                if (std::find(out.begin(), out.end(), copy) == out.end()) out.push_back(copy);
        return out;
    }

    // Many at once: one pass over every pack with a table of the shapes.
    std::unordered_map<ShapeKey, int, ShapeKeyHash> wanted;
    Aabb area;
    for (const PolygonRef& ref : refs) {
        const nfs::CollisionPack* pack = Pack(ref.section, ref.pack);
        if (!pack || ref.polygon >= pack->polygons.size()) continue;
        wanted[KeyOf(pack->polygons[ref.polygon])] = 1;
        area.Add(pack->polygons[ref.polygon].corners[0]);
    }
    const auto& loaded = world_->Loaded();
    for (size_t s = 0; s < loaded.size(); ++s)
        for (size_t p = 0; p < loaded[s]->collision.size(); ++p) {
            const nfs::CollisionPack& pack = loaded[s]->collision[p];
            const Aabb& b = pack.bounds;
            if (b.Valid() && area.Valid() &&
                (area.max.x < b.min.x - 1.0f || area.min.x > b.max.x + 1.0f ||
                 area.max.y < b.min.y - 1.0f || area.min.y > b.max.y + 1.0f)) {
                bool own = false;
                for (const PolygonRef& ref : refs) own = own || (ref.section == s && ref.pack == p);
                if (!own) continue;
            }
            for (size_t i = 0; i < pack.polygons.size(); ++i)
                if (wanted.count(KeyOf(pack.polygons[i]))) out.push_back({ (u32)s, (u32)p, (u32)i });
        }
    return out;
}

size_t Editor::Remove(const std::vector<PolygonRef>& refs) {
    std::vector<PolygonRef> all = WithCopies(refs);
    if (all.empty()) return 0;
    std::vector<std::pair<u32, u32>> packs;
    for (const PolygonRef& ref : all) packs.push_back({ ref.section, ref.pack });
    Remember(L"Delete collision", packs);

    // Last first within each pack, so the indices still to go stay valid.
    std::sort(all.begin(), all.end(), [](const PolygonRef& a, const PolygonRef& b) {
        if (a.section != b.section) return a.section < b.section;
        if (a.pack != b.pack) return a.pack < b.pack;
        return a.polygon > b.polygon;
    });
    all.erase(std::unique(all.begin(), all.end()), all.end());
    for (const PolygonRef& ref : all) {
        nfs::CollisionPack* pack = Pack(ref.section, ref.pack);
        if (!pack || ref.polygon >= pack->polygons.size()) continue;
        pack->polygons.erase(pack->polygons.begin() + ref.polygon);
        if (ref.polygon < pack->gridRank.size()) pack->gridRank.erase(pack->gridRank.begin() + ref.polygon);
        Changed(ref.section, ref.pack);
    }
    LOG_INFO("collision: deleted %zu polygons (%zu picked)", all.size(), refs.size());
    return all.size();
}

size_t Editor::SetSurface(const std::vector<PolygonRef>& refs, u8 surface) {
    const std::vector<PolygonRef> all = WithCopies(refs);
    if (all.empty()) return 0;
    std::vector<std::pair<u32, u32>> packs;
    for (const PolygonRef& ref : all) packs.push_back({ ref.section, ref.pack });
    Remember(L"Set surface", packs);
    for (const PolygonRef& ref : all) {
        nfs::CollisionPack* pack = Pack(ref.section, ref.pack);
        if (!pack || ref.polygon >= pack->polygons.size()) continue;
        nfs::CollisionPolygon& polygon = pack->polygons[ref.polygon];
        polygon.attributes[2] = surface;
        polygon.SyncFromAttributes();
        Changed(ref.section, ref.pack);
    }
    return all.size();
}

size_t Editor::SetRaceGroup(const std::vector<PolygonRef>& refs, u8 group) {
    const std::vector<PolygonRef> all = WithCopies(refs);
    if (all.empty()) return 0;
    std::vector<std::pair<u32, u32>> packs;
    for (const PolygonRef& ref : all) packs.push_back({ ref.section, ref.pack });
    Remember(group ? L"Set race group" : L"Make always present", packs);
    for (const PolygonRef& ref : all) {
        nfs::CollisionPack* pack = Pack(ref.section, ref.pack);
        if (!pack || ref.polygon >= pack->polygons.size()) continue;
        nfs::CollisionPolygon& polygon = pack->polygons[ref.polygon];
        polygon.attributes[0] = group;
        polygon.SyncFromAttributes();
        Changed(ref.section, ref.pack);
    }
    return all.size();
}

bool Editor::BeginMove(const std::vector<PolygonRef>& refs) {
    if (moving_) EndMove(false);
    moveRefs_ = WithCopies(refs);
    if (moveRefs_.empty()) return false;
    moveStart_.clear();
    std::vector<std::pair<u32, u32>> packs;
    for (const PolygonRef& ref : moveRefs_) {
        packs.push_back({ ref.section, ref.pack });
        moveStart_.push_back(Pack(ref.section, ref.pack)->polygons[ref.polygon]);
    }
    Remember(L"Move collision", packs);
    moving_ = true;
    return true;
}

void Editor::UpdateMove(const Vec3& offset) {
    if (!moving_) return;
    for (size_t i = 0; i < moveRefs_.size(); ++i) {
        const PolygonRef& ref = moveRefs_[i];
        nfs::CollisionPack* pack = Pack(ref.section, ref.pack);
        if (!pack || ref.polygon >= pack->polygons.size()) continue;
        nfs::CollisionPolygon polygon = moveStart_[i];
        for (Vec3& corner : polygon.corners) {
            corner.x += offset.x;
            corner.y += offset.y;
            if (!polygon.barrier) corner.z += offset.z;
        }
        pack->polygons[ref.polygon] = polygon;
        if (std::find(touched_.begin(), touched_.end(), ref.section) == touched_.end())
            touched_.push_back(ref.section);
    }
}

bool Editor::BeginReshape(const std::vector<PolygonRef>& refs) {
    if (moving_) EndMove(false);
    moveRefs_.clear();
    moveStart_.clear();
    std::vector<std::pair<u32, u32>> packs;
    for (const PolygonRef& ref : refs) {
        nfs::CollisionPack* pack = Pack(ref.section, ref.pack);
        if (!pack || ref.polygon >= pack->polygons.size()) continue;
        moveRefs_.push_back(ref);
        moveStart_.push_back(pack->polygons[ref.polygon]);
        packs.push_back({ ref.section, ref.pack });
    }
    if (moveRefs_.empty()) return false;
    Remember(L"Reshape collision", packs);
    moving_ = true;
    return true;
}

void Editor::SetShape(const PolygonRef& ref, const nfs::CollisionPolygon& polygon) {
    if (!moving_) return;
    nfs::CollisionPack* pack = Pack(ref.section, ref.pack);
    if (!pack || ref.polygon >= pack->polygons.size()) return;
    pack->polygons[ref.polygon] = polygon;
    if (std::find(touched_.begin(), touched_.end(), ref.section) == touched_.end()) touched_.push_back(ref.section);
}

const nfs::CollisionPolygon* Editor::Polygon(const PolygonRef& ref) const {
    const nfs::CollisionPack* pack = Pack(ref.section, ref.pack);
    return pack && ref.polygon < pack->polygons.size() ? &pack->polygons[ref.polygon] : nullptr;
}

bool Editor::BarrierQuad(const PolygonRef& ref, Vec3 out[4]) const {
    const nfs::CollisionPack* pack = Pack(ref.section, ref.pack);
    if (!pack || ref.polygon >= pack->polygons.size()) return false;
    pack->BarrierQuad(pack->polygons[ref.polygon], out);
    return true;
}

void Editor::EndMove(bool keep) {
    if (!moving_) return;
    moving_ = false;
    if (keep) {
        for (const PolygonRef& ref : moveRefs_) Changed(ref.section, ref.pack);
    } else if (!undo_.empty()) {
        Undo();
    }
    moveRefs_.clear();
    moveStart_.clear();
}

// --- saving ------------------------------------------------------------------

bool CollisionChunkOrders(const nfs::World& world, const nfs::Section& section,
                          std::vector<u32>& orders) {
    orders.clear();
    const ByteSpan bytes = world.SectionBytes(section);
    nfs::ChunkReader reader(bytes, section.streamOffset);
    nfs::Chunk top;
    u32 position = 0;
    while (reader.Next(top)) {
        if (top.id != nfs::kChunkCollisionPack) continue;
        const u32 at = position++;
        // Counted the way the loader counts them: empty packs are skipped.
        nfs::CollisionPack probe;
        if (nfs::ParseCollision(top, probe)) orders.push_back(at);
    }
    return true;
}

} // namespace collision
