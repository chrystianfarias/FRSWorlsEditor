#include "nfs/Preculler.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace nfs {
namespace {

constexpr u32 kChunkVisibilityContainer = 0x80034150;
constexpr u32 kChunkVisibilityHeader    = 0x00034151;
constexpr u32 kChunkZonePolygons        = 0x00034152;
constexpr u32 kChunkZoneVisibility      = 0x00034153;

// Both children hold variable-length records introduced by this marker. There
// is no count and no stride, so the record boundaries are the marker positions.
constexpr u8 kMarker[8] = { 0x0B, 0, 0, 0, 0x0B, 0, 0, 0 };

bool MarkerAt(ByteSpan span, size_t offset) {
    if (offset + sizeof(kMarker) > span.size) return false;
    return std::equal(std::begin(kMarker), std::end(kMarker), span.data + offset);
}

// Offsets of every record body, which runs from just past one marker to the
// next marker or the end.
std::vector<std::pair<size_t, size_t>> RecordBodies(ByteSpan span) {
    std::vector<size_t> starts;
    for (size_t offset = 0; offset + sizeof(kMarker) <= span.size; offset += 4)
        if (MarkerAt(span, offset)) starts.push_back(offset);

    std::vector<std::pair<size_t, size_t>> bodies;
    bodies.reserve(starts.size());
    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t begin = starts[i] + sizeof(kMarker);
        const size_t end   = (i + 1 < starts.size()) ? starts[i + 1] : span.size;
        if (end > begin) bodies.emplace_back(begin, end - begin);
    }
    return bodies;
}

} // namespace

bool Preculler::Parse(ByteSpan bunFile, bool underground1) {
    zones_.clear();
    visible_.clear();
    records_.clear();

    ChunkReader reader(bunFile, 0);
    Chunk chunk;
    Chunk container;
    while (reader.Next(chunk)) {
        if (chunk.id == kChunkVisibilityContainer) { container = chunk; break; }
    }
    if (container.id == 0) return false;

    ChunkReader children = ChunkReader::Children(container);
    Chunk child;
    while (children.Next(child)) {
        if (underground1) {
            if (child.id == kChunkVisibilityHeader) {          // outlines, one id lower
                for (const auto& [offset, length] : RecordBodies(child.data)) {
                    const ByteSpan body = child.data.sub(offset, length);
                    Zone zone;
                    zone.id = body.read<u16>(0);
                    const u32 points = body.read<u16>(2);
                    if (points < 3 || 4 + (size_t)points * 8 > body.size) continue;
                    zone.boundsMin = { 1e30f, 1e30f, 0.0f };
                    zone.boundsMax = { -1e30f, -1e30f, 0.0f };
                    for (u32 i = 0; i < points; ++i) {
                        const Vec3 p{ body.read<float>(4 + i * 8), body.read<float>(8 + i * 8), 0.0f };
                        zone.ring.push_back(p);
                        zone.boundsMin.x = std::min(zone.boundsMin.x, p.x);
                        zone.boundsMin.y = std::min(zone.boundsMin.y, p.y);
                        zone.boundsMax.x = std::max(zone.boundsMax.x, p.x);
                        zone.boundsMax.y = std::max(zone.boundsMax.y, p.y);
                    }
                    zone.recordOffset = (u64)(child.data.data - bunFile.data) + offset;
                    zone.recordLength = (u32)length;
                    zones_.push_back(std::move(zone));
                }
                continue;
            }
            if (child.id == kChunkZonePolygons) child.id = kChunkZoneVisibility;   // the lists
            else continue;
        }
        if (child.id == kChunkVisibilityHeader && child.data.size >= 4) {
            const u32 base = child.data.read<u32>(0);
            if (base > 0 && base < 100) collisionBase_ = base;
        } else if (child.id == kChunkZonePolygons) {
            for (const auto& [offset, length] : RecordBodies(child.data)) {
                const ByteSpan body = child.data.sub(offset, length);

                Zone zone;
                zone.id = body.read<u16>(0);
                const u32 points = body.read<u16>(2);

                // u16 id, u16 point count, the bounding box (min, max), the
                // box's middle, then the ring itself (as SPEED2.EXE reads it:
                // its zone objects test the point against `count` points from
                // +0x1C of the body). Taking the middle for a ring point, and
                // stopping short of the last ones, puts the edges wrong.
                if (points < 3 || 28 + (size_t)points * 8 > body.size) continue;

                zone.boundsMin = { body.read<float>(4),  body.read<float>(8),  0.0f };
                zone.boundsMax = { body.read<float>(12), body.read<float>(16), 0.0f };
                if (zone.boundsMin.x > zone.boundsMax.x) std::swap(zone.boundsMin.x, zone.boundsMax.x);
                if (zone.boundsMin.y > zone.boundsMax.y) std::swap(zone.boundsMin.y, zone.boundsMax.y);

                for (u32 i = 0; i < points; ++i)
                    zone.ring.push_back({ body.read<float>(28 + i * 8),
                                          body.read<float>(32 + i * 8), 0.0f });

                zone.recordOffset = (u64)(child.data.data - bunFile.data) + offset;
                zone.recordLength = (u32)length;
                if (zone.ring.size() >= 3) zones_.push_back(std::move(zone));
            }
        } else if (child.id == kChunkZoneVisibility) {
            for (const auto& [offset, length] : RecordBodies(child.data)) {
                const ByteSpan body = child.data.sub(offset, length);

                const u32 id    = body.read<u16>(0);
                const u32 count = body.read<u32>(8);
                if (count == 0 || 12 + (size_t)count * 2 > body.size) continue;

                std::vector<u32> sections;
                sections.reserve(count);
                for (u32 i = 0; i < count; ++i)
                    sections.push_back(body.read<u16>(12 + i * 2));

                visible_[id] = std::move(sections);
                records_[id] = { (u64)(child.data.data - bunFile.data) + offset, (u32)length };
            }
        }
    }

    LOG_INFO("preculler: %zu zones, %zu visibility lists", zones_.size(), visible_.size());
    return !zones_.empty() && !visible_.empty();
}

bool Preculler::Contains(const Zone& zone, float x, float y) const {
    if (x < zone.boundsMin.x || x > zone.boundsMax.x) return false;
    if (y < zone.boundsMin.y || y > zone.boundsMax.y) return false;

    bool inside = false;
    const size_t count = zone.ring.size();
    for (size_t i = 0, j = count - 1; i < count; j = i++) {
        const Vec3& a = zone.ring[i];
        const Vec3& b = zone.ring[j];
        if ((a.y > y) != (b.y > y) &&
            x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

bool Preculler::Listed(u32 sectionNumber) const {
    for (const auto& [zone, sections] : visible_)
        for (u32 number : sections)
            if (number == sectionNumber) return true;
    return false;
}

bool Preculler::IsCollisionZone(u32 zoneId) const {
    const u32 letter = zoneId / 100, number = zoneId % 100;
    return letter >= 1 && letter <= 20 && number >= 1 && number < collisionBase_;
}

namespace {

float DistanceToRing(const std::vector<Vec3>& ring, float x, float y) {
    float best = 1e30f;
    for (size_t i = 0; i < ring.size(); ++i) {
        const Vec3& a = ring[i];
        const Vec3& b = ring[(i + 1) % ring.size()];
        const float dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
        float t = length > 1e-9f ? ((x - a.x) * dx + (y - a.y) * dy) / length : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float ex = a.x + dx * t - x, ey = a.y + dy * t - y;
        best = std::min(best, ex * ex + ey * ey);
    }
    return std::sqrt(best);
}

} // namespace

void Preculler::CollisionZonesAt(const Vec3& point, float margin, std::vector<u32>& zoneIds) const {
    zoneIds.clear();
    const Zone* nearest = nullptr;
    float nearestDistance = 1e30f;
    for (const Zone& zone : zones_) {
        if (!IsCollisionZone(zone.id) || zone.ring.size() < 3) continue;
        const float distance = Contains(zone, point.x, point.y) ? 0.0f : DistanceToRing(zone.ring, point.x, point.y);
        if (distance <= margin) zoneIds.push_back(zone.id);
        if (distance < nearestDistance || (distance == nearestDistance && nearest && zone.id < nearest->id)) {
            nearestDistance = distance;
            nearest = &zone;
        }
    }
    if (zoneIds.empty() && nearest) zoneIds.push_back(nearest->id);
    std::sort(zoneIds.begin(), zoneIds.end());
    zoneIds.erase(std::unique(zoneIds.begin(), zoneIds.end()), zoneIds.end());
}

void Preculler::ListsNear(const Vec3& point, float radius, std::vector<std::vector<u32>>& lists) const {
    lists.clear();
    for (const Zone& zone : zones_) {
        const auto found = visible_.find(zone.id);
        if (found == visible_.end() || zone.ring.size() < 3) continue;
        if (point.x < zone.boundsMin.x - radius || point.x > zone.boundsMax.x + radius ||
            point.y < zone.boundsMin.y - radius || point.y > zone.boundsMax.y + radius)
            continue;
        bool near = Contains(zone, point.x, point.y);
        for (size_t i = 0; i < zone.ring.size() && !near; ++i) {
            const Vec3& a = zone.ring[i];
            const Vec3& b = zone.ring[(i + 1) % zone.ring.size()];
            const float dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
            float t = length > 1e-9f ? ((point.x - a.x) * dx + (point.y - a.y) * dy) / length : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const float ex = a.x + dx * t - point.x, ey = a.y + dy * t - point.y;
            near = ex * ex + ey * ey <= radius * radius;
        }
        if (!near) continue;
        std::vector<u32> list = found->second;
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
        lists.push_back(std::move(list));
    }
}

int Preculler::ZoneAt(float x, float y) const {
    int best = -1;
    float bestArea = 0.0f;
    for (size_t i = 0; i < zones_.size(); ++i) {
        const Zone& zone = zones_[i];
        if (visible_.find(zone.id) == visible_.end() || !Contains(zone, x, y)) continue;
        const float area = (zone.boundsMax.x - zone.boundsMin.x) * (zone.boundsMax.y - zone.boundsMin.y);
        if (best < 0 || area < bestArea) { best = (int)i; bestArea = area; }
    }
    return best;
}

const std::vector<u32>* Preculler::ListOf(u32 zoneId) const {
    const auto found = visible_.find(zoneId);
    return found == visible_.end() ? nullptr : &found->second;
}

bool Preculler::ListRecord(u32 zoneId, u64& fileOffset, u32& length) const {
    const auto found = records_.find(zoneId);
    if (found == records_.end()) return false;
    fileOffset = found->second.first;
    length = found->second.second;
    return true;
}

bool Preculler::VisibleAt(const Vec3& point, std::vector<u32>& sections) const {
    sections.clear();
    if (zones_.empty()) return false;

    const Zone* best = nullptr;
    float bestArea = 0.0f;
    for (const Zone& zone : zones_) {
        if (visible_.find(zone.id) == visible_.end()) continue;
        if (!Contains(zone, point.x, point.y)) continue;

        const float area = (zone.boundsMax.x - zone.boundsMin.x) *
                           (zone.boundsMax.y - zone.boundsMin.y);
        if (!best || area < bestArea) { best = &zone; bestArea = area; }
    }
    if (!best) return false;

    sections = visible_.find(best->id)->second;
    std::sort(sections.begin(), sections.end());
    sections.erase(std::unique(sections.begin(), sections.end()), sections.end());
    return !sections.empty();
}

} // namespace nfs
