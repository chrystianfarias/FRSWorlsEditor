#include "nfs/Preculler.h"

#include "core/Log.h"

#include <algorithm>

namespace nfs {
namespace {

constexpr u32 kChunkVisibilityContainer = 0x80034150;
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

bool Preculler::Parse(ByteSpan bunFile) {
    zones_.clear();
    visible_.clear();

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
        if (child.id == kChunkZonePolygons) {
            for (const auto& [offset, length] : RecordBodies(child.data)) {
                const ByteSpan body = child.data.sub(offset, length);

                Zone zone;
                zone.id = body.read<u16>(0);
                const u32 points = body.read<u16>(2);

                // The first two points are the zone's bounding box; the polygon
                // itself is what follows. Treating the box as part of the ring
                // makes the outline cross itself, and a point-in-polygon test on
                // that answers more or less at random.
                if (points < 3 || 4 + (size_t)points * 8 > body.size) continue;

                zone.boundsMin = { body.read<float>(4),  body.read<float>(8),  0.0f };
                zone.boundsMax = { body.read<float>(12), body.read<float>(16), 0.0f };
                if (zone.boundsMin.x > zone.boundsMax.x) std::swap(zone.boundsMin.x, zone.boundsMax.x);
                if (zone.boundsMin.y > zone.boundsMax.y) std::swap(zone.boundsMin.y, zone.boundsMax.y);

                for (u32 i = 2; i < points; ++i)
                    zone.ring.push_back({ body.read<float>(4 + i * 8),
                                          body.read<float>(8 + i * 8), 0.0f });

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
