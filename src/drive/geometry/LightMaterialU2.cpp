#include "drive/geometry/LightMaterialU2.h"

#include "drive/cars/CarBin.h"

#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace bb {

namespace {

::Vec3 ReadVec3(carbin::Reader& r) {
    const float x = r.F32();
    const float y = r.F32();
    const float z = r.F32();
    return { x, y, z };
}

LightMaterialU2 Read(carbin::Reader& r) {
    LightMaterialU2 m;
    r.Seek(0x0C);
    m.NameHash = r.U32();
    m.Version = r.S32();
    m.Name = r.FixedString(28);
    m.DiffuseMinScale = r.F32();
    m.DiffuseMin = ReadVec3(r);
    m.DiffuseMaxScale = r.F32();
    m.DiffuseMax = ReadVec3(r);
    m.DiffuseMinAlpha = r.F32();
    m.DiffuseMaxAlpha = r.F32();
    m.SpecularPower = r.F32();
    m.SpecularMinScale = r.F32();
    m.SpecularMin = ReadVec3(r);
    m.SpecularMaxScale = r.F32();
    m.SpecularMax = ReadVec3(r);
    m.EnvmapPower = r.F32();
    m.EnvmapMinScale = r.F32();
    m.EnvmapMin = ReadVec3(r);
    m.EnvmapMaxScale = r.F32();
    m.EnvmapMax = ReadVec3(r);
    // Loader 0x48E070 multiplies both envmap scales by 1.0 (flt_78D780): a no-op kept for fidelity.
    m.EnvmapMinScale *= 1.0f;
    m.EnvmapMaxScale *= 1.0f;
    return m;
}

// sub_5BB5B0: per-hash alpha override (min, max) applied at load.
const std::unordered_map<uint32_t, std::pair<float, float>>& AlphaOverride() {
    static const std::unordered_map<uint32_t, std::pair<float, float>> table = {
        { 0x471A1DCA, { 0.3f, 1.0f } }, { 0x3ED70C43, { 0.3f, 1.0f } }, { 0x12C9453C, { 0.3f, 1.0f } },
        { 0x89F23A14, { 0.3f, 1.0f } }, { 0x89946400, { 0.3f, 1.0f } },
        { 0x8050CA3F, { 0.4f, 1.0f } }, { 0x8A616C4C, { 0.4f, 1.0f } }, { 0xD68EA0C1, { 0.4f, 1.0f } }, { 0xD6EC76D5, { 0.4f, 1.0f } },
        { 0x2388DD82, { 0.5f, 1.0f } }, { 0x23E6A3F4, { 0.5f, 1.0f } }, { 0x23E6B396, { 0.5f, 1.0f } }, { 0x6CF41096, { 0.5f, 1.0f } },
        { 0x8062E2C0, { 0.5f, 1.0f } }, { 0x8CBE3497, { 0.5f, 1.0f } }, { 0x8CB694ED, { 0.5f, 1.0f } }, { 0x8CB61DCD, { 0.5f, 1.0f } },
        { 0x8CB95E6B, { 0.5f, 1.0f } }, { 0x8CBC02EC, { 0.5f, 1.0f } }, { 0xC9D2B801, { 0.5f, 1.0f } }, { 0xB3645F81, { 0.5f, 1.0f } },
        { 0x33A26CB6, { 0.4f, 0.8f } }, { 0xA6348EE3, { 0.4f, 0.8f } },
    };
    return table;
}

void Set(float* min, float* rng, const ::Vec3& a, float sa, const ::Vec3& b, float sb, float k) {
    min[0] = a.x * sa * k;
    min[1] = a.y * sa * k;
    min[2] = a.z * sa * k;
    rng[0] = b.x * sb * k - min[0];
    rng[1] = b.y * sb * k - min[1];
    rng[2] = b.z * sb * k - min[2];
}

} // namespace

LightMaterialU2::ShaderParams LightMaterialU2::ToShaderParams(uint32_t stripFlags, bool envmapEnabled) const {
    ShaderParams p;
    Set(p.DiffuseMin, p.DiffuseRange, DiffuseMin, DiffuseMinScale, DiffuseMax, DiffuseMaxScale, 1.0f);
    p.DiffuseMin[3] = DiffuseMinAlpha;
    p.DiffuseRange[3] = DiffuseMaxAlpha - DiffuseMinAlpha;
    Set(p.SpecularMin, p.SpecularRange, SpecularMin, SpecularMinScale, SpecularMax, SpecularMaxScale, 1.0f);
    Set(p.EnvmapMin, p.EnvmapRange, EnvmapMin, EnvmapMinScale, EnvmapMax, EnvmapMaxScale, 0.5f); // flt_784260
    if ((stripFlags & 0x100) != 0 && envmapEnabled) {
        if ((stripFlags & 0x80) == 0) {
            std::memset(p.DiffuseMin, 0, sizeof p.DiffuseMin);
            std::memset(p.DiffuseRange, 0, sizeof p.DiffuseRange);
            std::memset(p.SpecularMin, 0, sizeof p.SpecularMin);
            std::memset(p.SpecularRange, 0, sizeof p.SpecularRange);
        }
    } else {
        std::memset(p.EnvmapMin, 0, sizeof p.EnvmapMin);
        std::memset(p.EnvmapRange, 0, sizeof p.EnvmapRange);
    }
    p.SpecularPower = 1.1f * SpecularPower; // flt_800B94
    p.EnvmapPower = EnvmapPower;
    return p;
}

std::vector<LightMaterialU2> LightMaterialU2::ReadAll(const std::vector<uint8_t>& buf) {
    std::vector<LightMaterialU2> list;
    carbin::Walk(carbin::Parse(buf), [&](const carbin::Chunk& c) {
        if (c.Id != ChunkId) return;
        try {
            carbin::Reader r = carbin::ChunkReader(buf, c);
            LightMaterialU2 m = Read(r);
            if (m.Version != 3) return; // the loader only registers version 3
            const auto o = AlphaOverride().find(m.NameHash);
            if (o != AlphaOverride().end()) {
                m.DiffuseMinAlpha = o->second.first;
                m.DiffuseMaxAlpha = o->second.second;
            }
            list.push_back(std::move(m));
        } catch (const std::exception&) {
        }
    });
    return list;
}

LightMaterialU2::Library::Library(std::vector<LightMaterialU2> materials) : list_(std::move(materials)) {
    default_.Name = "(static zero default)";
    for (const LightMaterialU2& m : list_)
        if (m.NameHash == DefaultHash) {
            default_ = m;
            break;
        }
}

const LightMaterialU2& LightMaterialU2::Library::Get(uint32_t hash) const {
    for (const LightMaterialU2& m : list_)
        if (m.NameHash == hash) return m;
    return default_;
}

} // namespace bb
