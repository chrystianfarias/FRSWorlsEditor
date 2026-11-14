#include "drive/geometry/SolidList.h"

#include "drive/cars/CarBin.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace bb {

namespace {

constexpr uint32_t Pack = 0x80134000;
constexpr uint32_t ListHeaders = 0x80134001;
constexpr uint32_t ListHeader = 0x00134002;
constexpr uint32_t SolidChunk = 0x80134010;
constexpr uint32_t SolidHeader = 0x00134011;
constexpr uint32_t TextureTable = 0x00134012;
constexpr uint32_t LightMaterialTable = 0x00134013;
constexpr uint32_t PositionMarkers = 0x0013401A;
constexpr uint32_t Mesh = 0x80134100;
constexpr uint32_t MeshHeader = 0x00134900;
constexpr uint32_t VertexBuffer = 0x00134B01;
constexpr uint32_t MaterialsChunk = 0x00134B02;
constexpr uint32_t IndicesChunk = 0x00134B03;
constexpr uint32_t CollisionPack = 0x80034020;
constexpr uint32_t CollisionHeader = 0x00039200;
constexpr uint32_t CollisionPoints = 0x00039201;
constexpr uint32_t CollisionIndices = 0x00039202;

Vec3 ReadVec3(carbin::Reader& r) {
    Vec3 v;
    v.x = r.F32();
    v.y = r.F32();
    v.z = r.F32();
    return v;
}

std::vector<uint32_t> ReadHashTable(carbin::Reader r) {
    std::vector<uint32_t> t((size_t)(r.Length() / 8));
    for (uint32_t& h : t) {
        h = r.U32();
        r.Skip(4);
    }
    return t;
}

void ReadCollisionPack(const std::vector<uint8_t>& buf, const carbin::Chunk& pack, std::vector<CollisionHull>& out) {
    CollisionHull* cur = nullptr;
    for (const carbin::Chunk& c : pack.Children) {
        switch (c.Id) {
            case CollisionHeader: {
                out.emplace_back();
                cur = &out.back();
                carbin::Reader r = carbin::ChunkReader(buf, c, 0x10);
                r.Seek(0x14);
                const int numPoints = r.S32();
                cur->NameHash = r.U32();
                cur->Name = r.FixedString(32);
                const int numIdx = r.U16();
                cur->Points.resize((size_t)std::max(0, numPoints));
                cur->Indices.resize((size_t)numIdx);
                break;
            }
            case CollisionPoints:
                if (cur) {
                    carbin::Reader r = carbin::ChunkReader(buf, c);
                    for (size_t i = 0; i < cur->Points.size(); i++) {
                        r.Seek((int)i * 48 + 16);
                        cur->Points[i] = ReadVec3(r);
                    }
                }
                break;
            case CollisionIndices:
                if (cur) {
                    carbin::Reader r = carbin::ChunkReader(buf, c, 0x10);
                    for (uint16_t& i : cur->Indices) i = r.U16();
                }
                break;
            default:
                break;
        }
    }
}

} // namespace

std::unique_ptr<SolidList> SolidList::Read(const std::vector<uint8_t>& buf) {
    auto list = std::make_unique<SolidList>();
    const std::vector<carbin::Chunk> roots = carbin::Parse(buf);
    carbin::Walk(roots, [&](const carbin::Chunk& c) {
        if (c.Id == Pack) list->ReadPack(buf, c);
        else if (c.Id == CollisionPack) {
            try {
                ReadCollisionPack(buf, c, list->Collision);
            } catch (const std::exception&) {
            }
        }
    });
    return list;
}

void SolidList::ReadPack(const std::vector<uint8_t>& buf, const carbin::Chunk& pack) {
    if (const carbin::Chunk* hdrs = pack.Find(ListHeaders))
        if (const carbin::Chunk* h = hdrs->Find(ListHeader)) {
            carbin::Reader r = carbin::ChunkReader(buf, *h);
            r.Seek(0x08);
            const int version = r.S32();
            if (version != 0x1D) throw std::runtime_error("eSolidListHeader version is not 0x1D");
            r.Seek(0x10);
            Filename = r.FixedString(56);
            GroupName = r.FixedString(32);
        }
    for (const carbin::Chunk& sc : pack.Children) {
        if (sc.Id != SolidChunk) continue;
        try {
            auto s = std::make_unique<Solid>(Solid::Read(buf, sc));
            ByHash[s->NameHash] = s.get();
            Solids.push_back(std::move(s));
        } catch (const std::exception&) {
            // A malformed solid is left out; the rest of the pack still loads.
        }
    }
}

Solid Solid::Read(const std::vector<uint8_t>& buf, const carbin::Chunk& sc) {
    Solid s;
    for (const carbin::Chunk& c : sc.Children) {
        switch (c.Id) {
            case SolidHeader: s.ReadHeader(buf, c); break;
            case TextureTable: s.TextureHashes = ReadHashTable(carbin::ChunkReader(buf, c)); break;
            case LightMaterialTable: s.LightMaterialHashes = ReadHashTable(carbin::ChunkReader(buf, c)); break;
            case PositionMarkers: s.ReadMarkers(buf, c); break;
            case Mesh: s.ReadMesh(buf, c); break;
            default: break;
        }
    }
    return s;
}

void Solid::ReadHeader(const std::vector<uint8_t>& buf, const carbin::Chunk& c) {
    carbin::Reader r = carbin::ChunkReader(buf, c, 0x10);
    const int size = r.Length();
    r.Seek(0x0C);
    const uint8_t version = r.U8();
    if (version != 0x16) throw std::runtime_error("eSolid version is not 0x16");
    r.Skip(1);
    Flags = r.U16();
    NameHash = r.U32();
    NumPolys = r.S16();
    r.Seek(0x20);
    AabbMin = ReadVec3(r);
    r.Seek(0x30);
    AabbMax = ReadVec3(r);
    r.Seek(0x40);
    for (float& f : Pivot) f = r.F32();
    if (size >= 0xC0) {
        r.Seek(0x94);
        Volume = r.F32();
        Density = r.F32();
        r.Seek(0xA4);
        Name = r.FixedString(28);
    } else {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08X", NameHash);
        Name = text;
    }
}

void Solid::ReadMarkers(const std::vector<uint8_t>& buf, const carbin::Chunk& c) {
    carbin::Reader r = carbin::ChunkReader(buf, c, 0x10);
    const int n = r.Remaining() / 80;
    Markers.resize((size_t)n);
    for (PositionMarker& m : Markers) {
        m.NameHash = r.U32();
        m.IParam0 = r.S32();
        m.FParam0 = r.F32();
        m.FParam1 = r.F32();
        for (float& f : m.Matrix) f = r.F32();
    }
}

void Solid::ReadMesh(const std::vector<uint8_t>& buf, const carbin::Chunk& mesh) {
    const carbin::Chunk* hdr = mesh.Find(MeshHeader);
    const carbin::Chunk* vb = mesh.Find(VertexBuffer);
    const carbin::Chunk* mats = mesh.Find(MaterialsChunk);
    const carbin::Chunk* idx = mesh.Find(IndicesChunk);
    if (!hdr || !vb || !mats || !idx) throw std::runtime_error("incomplete mesh");

    carbin::Reader hr = carbin::ChunkReader(buf, *hdr, 0x10);
    hr.Seek(0x08);
    const int version = hr.S32();
    if (version != 0x10) throw std::runtime_error("mesh version is not 0x10");
    MeshFlags = hr.U32();
    const int numMats = hr.S32();
    hr.Seek(0x34);
    VertexCount = hr.S32();

    carbin::Reader mr = carbin::ChunkReader(buf, *mats, 0x10);
    Materials.resize((size_t)std::max(0, numMats));
    for (SolidMaterial& m : Materials) {
        const int b = mr.Relative();
        m.BoundsMin = ReadVec3(mr);
        m.NumIndices = mr.S32();
        m.BoundsMax = ReadVec3(mr);
        m.TextureIndex = mr.S32();
        m.LightMaterialIndex = mr.S32();
        mr.Seek(b + 0x34);
        m.IndexOffset = mr.S32();
        m.Flags = mr.U32();
        mr.Seek(b + 0x3C);
    }

    int totalIdx = 0;
    for (const SolidMaterial& m : Materials) totalIdx = std::max(totalIdx, m.IndexOffset + m.NumIndices);
    carbin::Reader ir = carbin::ChunkReader(buf, *idx, 0x10);
    Indices.resize((size_t)totalIdx);
    for (uint16_t& i : Indices) i = ir.U16();

    carbin::Reader vr = carbin::ChunkReader(buf, *vb, 0x80);
    if (VertexCount == 0) return;
    VertexStride = vr.Length() / VertexCount;
    const int n = VertexCount;
    const bool hasNormal = VertexStride == 36 || VertexStride == 60;
    if (!hasNormal && VertexStride != 24) throw std::runtime_error("unknown vertex stride");
    Positions.resize((size_t)n * 3);
    if (hasNormal) Normals.resize((size_t)n * 3);
    Colors.resize((size_t)n);
    UVs.resize((size_t)n * 2);
    for (int i = 0; i < n; i++) {
        vr.Seek(i * VertexStride);
        Positions[i * 3] = vr.F32();
        Positions[i * 3 + 1] = vr.F32();
        Positions[i * 3 + 2] = vr.F32();
        if (hasNormal) {
            Normals[i * 3] = vr.F32();
            Normals[i * 3 + 1] = vr.F32();
            Normals[i * 3 + 2] = vr.F32();
        }
        Colors[i] = vr.U32();
        UVs[i * 2] = vr.F32();
        UVs[i * 2 + 1] = vr.F32();
    }
}

} // namespace bb
