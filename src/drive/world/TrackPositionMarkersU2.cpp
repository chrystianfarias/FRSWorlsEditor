// Port of NFSU2Unity Core/World/TrackPositionMarkersU2.cs.
#include "drive/world/TrackPositionMarkersU2.h"

#include <algorithm>
#include <cstring>

namespace bb {

namespace {
template <class T> T Rd(const uint8_t* b, size_t o) { T v; std::memcpy(&v, b + o, sizeof(T)); return v; }
}

std::vector<TrackPositionMarkerU2> TrackPositionMarkersU2::Read(const std::vector<uint8_t>& file)
{
    std::vector<TrackPositionMarkerU2> list;
    const uint8_t* b = file.data();
    size_t pos = 0;
    while (pos + 8 <= file.size()) {
        uint32_t id = Rd<uint32_t>(b, pos);
        int32_t size = Rd<int32_t>(b, pos + 4);
        size_t start = pos + 8, end = std::min(file.size(), start + static_cast<size_t>(std::max(0, size)));
        if (id == ChunkId) {
            size_t p = start;
            while (p < end && b[p] == 0x11) p++;   // alignment filler before the records
            for (; p + TrackPositionMarkerU2::Size <= end; p += TrackPositionMarkerU2::Size) {
                TrackPositionMarkerU2 m;
                m.NameHash = Rd<uint32_t>(b, p + 0x08);
                m.Slot = Rd<int32_t>(b, p + 0x0C);
                m.Position = Vec3(Rd<float>(b, p + 0x10), Rd<float>(b, p + 0x14), Rd<float>(b, p + 0x18));
                m.Heading = Rd<uint16_t>(b, p + 0x20);
                m.TrackId = Rd<int32_t>(b, p + 0x24);
                list.push_back(m);
            }
        }
        pos = end;
    }
    return list;
}

bool TrackPositionMarkersU2::TryFind(const std::vector<TrackPositionMarkerU2>& markers, uint32_t nameHash, int trackId,
                                     int slot, TrackPositionMarkerU2& marker)
{
    for (const auto& m : markers)
        if (m.NameHash == nameHash && m.TrackId == trackId && m.Slot == slot) {
            marker = m;
            return true;
        }
    marker = TrackPositionMarkerU2();
    return false;
}

} // namespace bb
