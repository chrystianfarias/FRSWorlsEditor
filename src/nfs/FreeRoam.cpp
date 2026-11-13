#include "nfs/FreeRoam.h"

#include "core/Log.h"
#include "nfs/World.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace nfs {

namespace {

constexpr u32 kFreeRoamRoot  = 0x80034147;
constexpr u32 kFreeRoamZones = 0x0003414A;
constexpr size_t kZoneHeader = 0x44;

std::vector<u8> ReadWholeFile(const std::wstring& path) {
    std::vector<u8> bytes;
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return bytes;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
        bytes.resize((size_t)size);
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
    }
    std::fclose(file);
    return bytes;
}

template <typename T> T Read(const std::vector<u8>& bytes, size_t at) {
    T value{};
    if (at + sizeof(T) <= bytes.size()) std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

// Payload offset and size of each chunk in [begin, end). These files hold no
// alignment filler, so the chunks follow one another directly.
struct Span { u32 id; size_t data; size_t size; };
std::vector<Span> Chunks(const std::vector<u8>& bytes, size_t begin, size_t end) {
    std::vector<Span> out;
    size_t at = begin;
    while (at + 8 <= end) {
        const u32 id = Read<u32>(bytes, at);
        const u32 size = Read<u32>(bytes, at + 4);
        if (at + 8 + size > end) break;
        out.push_back({ id, at + 8, size });
        at += 8 + size;
    }
    return out;
}

// Height of the triangle abc at (x, y) in plan, when the point is inside it.
bool HeightInTriangle(const Vec3& a, const Vec3& b, const Vec3& c, float x, float y, float& z) {
    const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (std::fabs(d) < 1e-6f) return false;
    const float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
    const float v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
    const float w = 1.0f - u - v;
    constexpr float kEdge = -1e-4f;
    if (u < kEdge || v < kEdge || w < kEdge) return false;
    z = u * a.z + v * b.z + w * c.z;
    return true;
}

// Every walkable collision height at (x, y), handed to `take`.
template <typename Take> void HeightsAt(const World& world, float x, float y, Take take) {
    for (const auto& section : world.Loaded()) {
        for (const CollisionPack& pack : section->collision) {
            if (pack.bounds.Valid() &&
                (x < pack.bounds.min.x || x > pack.bounds.max.x ||
                 y < pack.bounds.min.y || y > pack.bounds.max.y))
                continue;
            for (const CollisionPolygon& polygon : pack.polygons) {
                if (polygon.barrier) continue;
                const Vec3* c = polygon.corners;
                float z;
                bool inside = HeightInTriangle(c[0], c[1], c[2], x, y, z);
                if (!inside && polygon.quad) inside = HeightInTriangle(c[0], c[2], c[3], x, y, z);
                if (inside) take(z);
            }
        }
    }
}

} // namespace

bool ReadFreeRoamZones(const std::wstring& gameFolder, const std::wstring& region,
                       std::vector<FreeRoamZone>& out) {
    out.clear();
    const std::vector<u8> bytes =
        ReadWholeFile(gameFolder + L"\\TRACKS\\ROUTES" + region + L"\\PathsFreeRoam.bin");
    if (bytes.empty()) return false;

    for (const Span& root : Chunks(bytes, 0, bytes.size())) {
        if (root.id != kFreeRoamRoot) continue;
        for (const Span& child : Chunks(bytes, root.data, root.data + root.size)) {
            if (child.id != kFreeRoamZones) continue;

            // Records are variable length: a 0x44-byte header, then the
            // polygon. The record's own size sits at +0x42, and the records
            // fill the chunk exactly.
            size_t at = child.data;
            const size_t end = child.data + child.size;
            while (at + kZoneHeader <= end) {
                FreeRoamZone zone;
                zone.offset    = (u32)at;
                zone.type      = Read<u32>(bytes, at + 0x00);
                zone.centre    = { Read<float>(bytes, at + 0x04), Read<float>(bytes, at + 0x08) };
                zone.facing    = { Read<float>(bytes, at + 0x0C), Read<float>(bytes, at + 0x10) };
                zone.radius    = Read<float>(bytes, at + 0x14);
                zone.flag      = Read<u32>(bytes, at + 0x18);
                zone.boundsMin = { Read<float>(bytes, at + 0x20), Read<float>(bytes, at + 0x24) };
                zone.boundsMax = { Read<float>(bytes, at + 0x28), Read<float>(bytes, at + 0x2C) };
                zone.hash      = Read<u32>(bytes, at + 0x30);
                zone.cellX     = Read<i32>(bytes, at + 0x34);
                zone.cellY     = Read<i32>(bytes, at + 0x38);
                zone.extra     = Read<u32>(bytes, at + 0x3C);
                const u16 count = Read<u16>(bytes, at + 0x40);
                const u16 size  = Read<u16>(bytes, at + 0x42);
                if (size < kZoneHeader + 8u * count || at + size > end) {
                    LOG_WARN("free roam: zone record at %zu is malformed, stopping", at);
                    break;
                }
                for (u16 i = 0; i < count; ++i)
                    zone.polygon.push_back({ Read<float>(bytes, at + kZoneHeader + 8 * i),
                                             Read<float>(bytes, at + kZoneHeader + 8 * i + 4) });
                out.push_back(std::move(zone));
                at += size;
            }
            return !out.empty();
        }
    }
    return false;
}

bool GroundHeight(const World& world, float x, float y, float& height) {
    bool found = false;
    HeightsAt(world, x, y, [&](float z) {
        if (!found || z < height) height = z;
        found = true;
    });
    return found;
}

bool GroundBelow(const World& world, float x, float y, float ceiling, float& height) {
    bool found = false;
    HeightsAt(world, x, y, [&](float z) {
        if (z > ceiling) return;
        if (!found || z > height) height = z;
        found = true;
    });
    return found;
}

} // namespace nfs
