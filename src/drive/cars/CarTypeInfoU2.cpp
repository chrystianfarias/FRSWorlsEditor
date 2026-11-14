#include "drive/cars/CarTypeInfoU2.h"

#include "drive/cars/CarBin.h"

#include <cstdio>
#include <stdexcept>

namespace bb {

std::string CarTypeInfoU2::ToString() const {
    char text[160];
    std::snprintf(text, sizeof text, "#%d %s (%s) usage=%d", Index, CollectionName.c_str(),
                  ManufacturerName.c_str(), UsageType);
    return text;
}

CarTypeInfoU2 CarTypeInfoU2::Read(const uint8_t* buf, size_t bufSize, size_t offset) {
    if (offset + Size > bufSize) throw std::runtime_error("CarTypeInfo record past the end of the buffer");
    carbin::Reader r(buf, (int)offset, (int)(offset + Size));
    CarTypeInfoU2 c;
    c.CollectionName = r.FixedString(0x20);
    c.BaseModelName = r.FixedString(0x20);
    c.GeometryFilename = r.FixedString(0x20);
    r.Seek(0xC0);
    c.ManufacturerName = r.FixedString(0x10);
    c.NameHash = r.U32();
    c.HeadlightFov = r.F32();
    auto vec3 = [&r]() {
        Vec3 v;
        v.x = r.F32();
        v.y = r.F32();
        v.z = r.F32();
        return v;
    };
    r.Seek(0xE0);
    c.HeadlightPosition = vec3();
    r.Seek(0xF0);
    c.DriverRenderingOffset = vec3();
    r.Seek(0x100);
    c.SteeringWheelRendering = vec3();
    for (int i = 0; i < 4; i++) {
        r.Seek(0x120 + i * 0x30);
        CarWheelInfo& w = c.Wheels[i];
        w.X = r.F32();
        w.PhysicsY = r.F32();
        w.RideHeight = r.F32();
        w.Unknown0C = r.F32();
        w.Radius = r.F32();
        w.Width = r.F32();
        w.Id = r.S32();
        w.Y = r.F32();
        w.WideBodyY = r.F32();
    }
    r.Seek(0x840);
    c.Index = r.S32();
    c.UsageType = r.S32();
    r.Seek(0x84C);
    c.DefaultBasePaint = r.U32();
    c.DefaultBasePaint2 = r.U32();
    r.Seek(0x88C);
    c.IsSkinnable = r.S32() != 0;
    c.Raw.assign(buf + offset, buf + offset + Size);
    return c;
}

std::vector<CarTypeInfoU2> CarTypeInfoU2::ReadAll(const std::vector<uint8_t>& globalB) {
    std::vector<CarTypeInfoU2> list;
    carbin::Walk(carbin::Parse(globalB), [&](const carbin::Chunk& ch) {
        if (ch.Id != ChunkId) return;
        // The array starts 16-byte aligned after the chunk header (8 pad bytes in GlobalB).
        const int start = (ch.DataOffset() + 0xF) & ~0xF;
        const int n = (ch.End() - start) / Size;
        for (int i = 0; i < n; i++) list.push_back(Read(globalB.data(), globalB.size(), (size_t)(start + i * Size)));
    });
    return list;
}

std::vector<CarTypeInfoU2> CarTypeInfoU2::ReadAll(const std::wstring& gameFolder) {
    std::vector<uint8_t> buf;
    if (!carbin::ReadFile(carbin::GamePath(gameFolder, "GLOBAL\\GLOBALB.BUN"), buf))
        throw std::runtime_error("cannot read GLOBAL\\GLOBALB.BUN");
    return ReadAll(buf);
}

} // namespace bb
