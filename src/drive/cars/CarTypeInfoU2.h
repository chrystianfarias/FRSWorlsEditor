#pragma once

// Port of NFSU2Unity Core/Cars/CarTypeInfoU2.cs.

#include "core/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bb {

// CarInfoWheel (0x30), CarTypeInfo+0x120, order FL, FR, RR, RL.
// Field uses from docs/re/car-render-models.md section 5.1.
struct CarWheelInfo {
    float X = 0;           // +0x00 axle X (m, +X forward)
    float PhysicsY = 0;    // +0x04 wheel-centre Y used by physics/FE (render ignores it)
    float RideHeight = 0;  // +0x08 wheel-centre Z in body space
    float Unknown0C = 0;   // +0x0C no use found
    float Radius = 0;      // +0x10
    float Width = 0;       // +0x14 render width for aftermarket rims (level > 0)
    int32_t Id = 0;        // +0x18
    float Y = 0;           // +0x1C render Y = outer face of the tyre
    float WideBodyY = 0;   // +0x20 render Y with a wide body
};

// NFSU2 CarTypeInfo (0x890 bytes), GLOBAL\GLOBALB.BUN chunk 0x34600 (16-byte aligned array).
// Only identity and wheel fields are decoded; the performance blocks (0x1E0..0x83F) are
// kept raw in Raw for the physics.
class CarTypeInfoU2 {
public:
    static constexpr uint32_t ChunkId = 0x00034600;
    static constexpr int Size = 0x890;

    std::string CollectionName;   // +0x000
    std::string BaseModelName;    // +0x020, prefix for textures and "<base>_CV"
    std::string GeometryFilename; // +0x040
    std::string ManufacturerName; // +0x0C0
    uint32_t NameHash = 0;        // +0x0D0
    float HeadlightFov = 0;       // +0x0D4
    Vec3 HeadlightPosition;       // +0x0E0
    Vec3 DriverRenderingOffset;   // +0x0F0
    Vec3 SteeringWheelRendering;  // +0x100
    CarWheelInfo Wheels[4] = {};
    int32_t Index = 0;            // +0x840
    int32_t UsageType = 0;        // +0x844: 0 racer, 1 cop, 2 traffic
    uint32_t DefaultBasePaint = 0;  // +0x84C, PartNameHash of a BASE_PAINT part
    uint32_t DefaultBasePaint2 = 0; // +0x850
    bool IsSkinnable = false;     // +0x88C (Game::MODEL_NEEDS_SKIN)
    std::vector<uint8_t> Raw;     // the whole 0x890-byte record

    std::string ToString() const;

    // Decodes one record at buf[offset .. offset+Size). Throws std::runtime_error when out of range.
    static CarTypeInfoU2 Read(const uint8_t* buf, size_t bufSize, size_t offset);

    // Reads every CarTypeInfo from a GLOBALB.BUN image.
    static std::vector<CarTypeInfoU2> ReadAll(const std::vector<uint8_t>& globalB);

    // Reads <gameFolder>\GLOBAL\GLOBALB.BUN. Throws std::runtime_error when the file cannot be read.
    static std::vector<CarTypeInfoU2> ReadAll(const std::wstring& gameFolder);
};

} // namespace bb
