#pragma once

// Port of NFSU2Unity Core/Geometry/SolidList.cs (the lenient reader used for cars):
// the solids of a GEOMETRY.BIN indexed by name hash, with position markers, light material
// indices and the car collision hulls. Everything stays in game space (X forward, Y left, Z up).

#include "core/Math.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bb {

namespace carbin { struct Chunk; }

struct PositionMarker {
    uint32_t NameHash = 0;
    int32_t IParam0 = 0;
    float FParam0 = 0, FParam1 = 0;
    float Matrix[16] = {};     // row-major; position in [12..14]

    Mat4 ToMat4() const {
        Mat4 m;
        for (int i = 0; i < 16; i++) m.m[i / 4][i % 4] = Matrix[i];
        return m;
    }
};

// U2 material ("shading group"): an index range with a texture and a light material.
struct SolidMaterial {
    Vec3 BoundsMin, BoundsMax;
    int NumIndices = 0;
    int TextureIndex = 0;
    int LightMaterialIndex = 0;
    int IndexOffset = 0;
    uint32_t Flags = 0;

    static constexpr uint32_t FlagLit = 0x0080;
    static constexpr uint32_t FlagEnvMap = 0x0100;
    static constexpr uint32_t FlagHasNormals = 0x4000;
    static constexpr uint32_t FlagAlpha = 0x8000;
};

class Solid {
public:
    std::string Name;
    uint32_t NameHash = 0;
    uint16_t Flags = 0;
    int16_t NumPolys = 0;
    Vec3 AabbMin, AabbMax;
    float Pivot[16] = {};
    float Volume = 0, Density = 0;
    std::vector<uint32_t> TextureHashes;
    std::vector<uint32_t> LightMaterialHashes;
    std::vector<PositionMarker> Markers;

    uint32_t MeshFlags = 0;
    int VertexCount = 0;
    int VertexStride = 0;
    std::vector<float> Positions;    // 3 per vertex
    std::vector<float> Normals;      // 3 per vertex (empty if the stride has none)
    std::vector<uint32_t> Colors;    // D3DCOLOR ARGB
    std::vector<float> UVs;          // 2 per vertex
    std::vector<uint16_t> Indices;
    std::vector<SolidMaterial> Materials;

    bool HasMesh() const { return VertexCount > 0; }

    // Throws std::runtime_error on a malformed solid.
    static Solid Read(const std::vector<uint8_t>& buf, const carbin::Chunk& sc);

private:
    void ReadHeader(const std::vector<uint8_t>& buf, const carbin::Chunk& c);
    void ReadMarkers(const std::vector<uint8_t>& buf, const carbin::Chunk& c);
    void ReadMesh(const std::vector<uint8_t>& buf, const carbin::Chunk& mesh);
};

// Convex car collision hull ("<CAR>_CV", chunk 0x80034020).
struct CollisionHull {
    std::string Name;
    uint32_t NameHash = 0;
    std::vector<Vec3> Points;
    std::vector<uint16_t> Indices;
};

class SolidList {
public:
    std::string Filename;
    std::string GroupName;
    std::vector<std::unique_ptr<Solid>> Solids;
    std::unordered_map<uint32_t, Solid*> ByHash;
    std::vector<CollisionHull> Collision;

    const Solid* Find(uint32_t hash) const {
        const auto it = ByHash.find(hash);
        return it == ByHash.end() ? nullptr : it->second;
    }

    // Solids that fail to parse are skipped. Throws std::runtime_error only on a bad list header.
    static std::unique_ptr<SolidList> Read(const std::vector<uint8_t>& buf);

private:
    void ReadPack(const std::vector<uint8_t>& buf, const carbin::Chunk& pack);
};

} // namespace bb
