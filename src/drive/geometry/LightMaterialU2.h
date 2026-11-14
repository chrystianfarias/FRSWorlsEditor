#pragma once

// Port of NFSU2Unity Core/Geometry/LightMaterialU2.cs: eLightMaterial (chunk 0x135200, 0xA0 bytes,
// version 3) and SetLightMaterial (0x5BD4A0), the six vectors + two scalars the CAR effect reads.
// See docs/re/light-materials.md. Colours are R,G,B ("Min" at grazing angles, "Max" face-on).
// Vector fields use the editor's ::Vec3 (x, y, z = R, G, B), like the other car ports.

#include "core/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bb {

class LightMaterialU2 {
public:
    static constexpr uint32_t ChunkId = 0x00135200;
    static constexpr uint32_t DefaultHash = 0x2C420D64; // "DEFAULT"

    uint32_t NameHash = 0;          // +0x0C
    int Version = 0;                // +0x10
    std::string Name;               // +0x14, char[28]
    float DiffuseMinScale = 0;      // +0x30
    ::Vec3 DiffuseMin;              // +0x34
    float DiffuseMaxScale = 0;      // +0x40
    ::Vec3 DiffuseMax;              // +0x44
    float DiffuseMinAlpha = 0;      // +0x50 (overridden for some hashes at load)
    float DiffuseMaxAlpha = 0;      // +0x54
    float SpecularPower = 0;        // +0x58
    float SpecularMinScale = 0;     // +0x5C
    ::Vec3 SpecularMin;             // +0x60
    float SpecularMaxScale = 0;     // +0x6C
    ::Vec3 SpecularMax;             // +0x70
    float EnvmapPower = 0;          // +0x7C
    float EnvmapMinScale = 0;       // +0x80
    ::Vec3 EnvmapMin;               // +0x84
    float EnvmapMaxScale = 0;       // +0x90
    ::Vec3 EnvmapMax;               // +0x94

    // The six vectors SetLightMaterial (0x5BD4A0) uploads, in its calculation order.
    struct ShaderParams {
        float DiffuseMin[4] = {}, DiffuseRange[4] = {}, SpecularMin[4] = {}, SpecularRange[4] = {},
              EnvmapMin[4] = {}, EnvmapRange[4] = {};
        float SpecularPower = 0, EnvmapPower = 0;
    };

    // SetLightMaterial (0x5BD4A0) for the CAR effect: stripFlags = solid material flags
    // (0x100 envmap, 0x80 keep diffuse/specular with envmap); envmapEnabled = video option.
    ShaderParams ToShaderParams(uint32_t stripFlags, bool envmapEnabled = true) const;

    // All light materials in a file (GLOBALB holds all 186), in file order, with the load-time
    // alpha override applied. Lookup = Library.
    static std::vector<LightMaterialU2> ReadAll(const std::vector<uint8_t>& buf);

    // elGetLightMaterial (0x48F6A0): first match in load order, else DEFAULT.
    class Library;
};

class LightMaterialU2::Library {
public:
    Library() = default;
    explicit Library(std::vector<LightMaterialU2> materials);
    const LightMaterialU2& Get(uint32_t hash) const;
    const LightMaterialU2& Default() const { return default_; }
    size_t Count() const { return list_.size(); }

private:
    std::vector<LightMaterialU2> list_;
    LightMaterialU2 default_;   // "(static zero default)" when GLOBALB has no DEFAULT
};

} // namespace bb
