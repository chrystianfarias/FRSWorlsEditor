#include "nfs/SolidList.h"

#include "core/Log.h"

#include <cstring>

namespace nfs {

namespace {

// --- Solid object header, chunk 0x00134011 ---------------------------------
constexpr size_t kHdrVersion   = 0x0C;
constexpr size_t kHdrHash      = 0x10;
constexpr size_t kHdrNumTris   = 0x14;
constexpr size_t kHdrBoundsMin = 0x20;
constexpr size_t kHdrBoundsMax = 0x30;
constexpr size_t kHdrTransform = 0x40;  // 4x4 floats, row major, translation in row 3
constexpr size_t kHdrName      = 0xA4;  // zero-terminated ASCII

// --- Mesh descriptor, chunk 0x00134900 -------------------------------------
constexpr size_t kDescNumMaterials = 0x10;
constexpr size_t kDescNumTriangles = 0x24;
constexpr size_t kDescNumVertices  = 0x34;

// --- Material entry, chunk 0x00134B02 --------------------------------------
constexpr size_t kMaterialEntrySize = 60;
constexpr size_t kMatBoundsMin      = 0x00;
constexpr size_t kMatIndexCount     = 0x0C;
constexpr size_t kMatBoundsMax      = 0x10;
constexpr size_t kMatTextureIndex   = 0x1C;
constexpr size_t kMatFlags          = 0x20;
constexpr size_t kMatFirstIndex     = 0x34;
constexpr size_t kMatStripFlags     = 0x38;

// --- Vertex layouts --------------------------------------------------------
// Only two strides occur across every shipped track stream.
constexpr u32 kStridePositionColorUv       = 24;  // pos, color, uv
constexpr u32 kStridePositionNormalColorUv = 36;  // pos, normal, color, uv

Vec3 ReadVec3(ByteSpan span, size_t offset) {
    return { span.read<float>(offset),
             span.read<float>(offset + 4),
             span.read<float>(offset + 8) };
}

Mat4 ReadMat4(ByteSpan span, size_t offset) {
    Mat4 m;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            m.m[row][col] = span.read<float>(offset + (size_t)(row * 4 + col) * 4);
    return m;
}

std::string ReadName(ByteSpan span, size_t offset) {
    if (offset >= span.size) return {};
    const char* text = (const char*)span.data + offset;
    const size_t available = span.size - offset;
    const size_t length = ::strnlen(text, available);
    return std::string(text, length);
}

void ParseHeader(ByteSpan data, SolidObject& out) {
    out.version    = data.read<u32>(kHdrVersion);
    out.hash       = data.read<u32>(kHdrHash);
    out.bounds.min = ReadVec3(data, kHdrBoundsMin);
    out.bounds.max = ReadVec3(data, kHdrBoundsMax);
    out.transform  = ReadMat4(data, kHdrTransform);
    out.name       = ReadName(data, kHdrName);
    out.kind       = ClassifyObject(out.name);
}

void ParseTextureRefs(ByteSpan data, SolidObject& out) {
    // Eight bytes per entry: a texture hash followed by four reserved bytes.
    const size_t count = data.size / 8;
    out.textureHashes.reserve(count);
    for (size_t i = 0; i < count; ++i)
        out.textureHashes.push_back(data.read<u32>(i * 8));
}

void ParseMaterials(ByteSpan data, SolidObject& out) {
    const size_t count = data.size / kMaterialEntrySize;
    out.materials.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const ByteSpan entry = data.sub(i * kMaterialEntrySize, kMaterialEntrySize);

        SolidMaterial material;
        material.indexCount   = entry.read<u32>(kMatIndexCount);
        material.firstIndex   = entry.read<u32>(kMatFirstIndex);
        material.textureIndex = entry.read<u32>(kMatTextureIndex);
        material.flags        = entry.read<u32>(kMatFlags);
        material.stripFlags   = entry.read<u32>(kMatStripFlags);
        material.bounds.min   = ReadVec3(entry, kMatBoundsMin);
        material.bounds.max   = ReadVec3(entry, kMatBoundsMax);

        out.materials.push_back(material);
    }
}

// Widens the packed source vertices into SolidVertex.
bool ParseVertices(ByteSpan data, u32 vertexCount, SolidObject& out) {
    if (vertexCount == 0) return false;
    if (data.size % vertexCount != 0) {
        LOG_WARN("object '%s': vertex buffer %zu bytes is not a multiple of %u vertices",
                 out.name.c_str(), data.size, vertexCount);
        return false;
    }

    const u32 stride = (u32)(data.size / vertexCount);
    size_t colorOffset, uvOffset;
    bool hasNormal;

    switch (stride) {
        case kStridePositionColorUv:
            hasNormal = false; colorOffset = 12; uvOffset = 16;
            break;
        case kStridePositionNormalColorUv:
            hasNormal = true;  colorOffset = 24; uvOffset = 28;
            break;
        default:
            LOG_WARN("object '%s': unsupported vertex stride %u", out.name.c_str(), stride);
            return false;
    }

    out.hasNormals   = hasNormal;
    out.vertexStride = stride;
    out.vertices.resize(vertexCount);
    for (u32 i = 0; i < vertexCount; ++i) {
        const ByteSpan source = data.sub((size_t)i * stride, stride);
        SolidVertex& vertex = out.vertices[i];

        vertex.position = ReadVec3(source, 0);
        vertex.normal   = hasNormal ? ReadVec3(source, 12) : Vec3{ 0.0f, 0.0f, 1.0f };
        vertex.color    = source.read<u32>(colorOffset);
        vertex.uv       = { source.read<float>(uvOffset), source.read<float>(uvOffset + 4) };
    }
    return true;
}

void ParseIndices(ByteSpan data, u32 triangleCount, SolidObject& out) {
    // The buffer is padded out to an alignment boundary, so trust the triangle
    // count from the descriptor over the raw byte size.
    size_t count = (size_t)triangleCount * 3;
    if (count * 2 > data.size) count = data.size / 2;

    out.indices.resize(count);
    if (count) std::memcpy(out.indices.data(), data.data, count * 2);
}

} // namespace

ObjectKind ClassifyObject(const std::string& name) {
    if (name.rfind("PAN_", 0) == 0)    return ObjectKind::Backdrop;  // panorama pieces
    if (name.rfind("SKYDOME", 0) == 0) return ObjectKind::Backdrop;
    if (name.rfind("TRN_", 0) == 0)    return ObjectKind::Terrain;
    return ObjectKind::Prop;
}

bool ParseSolidObject(const Chunk& objectChunk, SolidObject& out) {
    ChunkReader reader = ChunkReader::Children(objectChunk);
    Chunk child;

    u32 vertexCount = 0;
    u32 triangleCount = 0;
    ByteSpan vertexData, indexData, materialData;

    while (reader.Next(child)) {
        switch (child.id) {
            case kChunkSolidObjectHeader:
                ParseHeader(child.data, out);
                triangleCount = child.data.read<u32>(kHdrNumTris);
                break;

            case kChunkSolidObjectTexRefs:
                ParseTextureRefs(child.data, out);
                break;

            case kChunkMeshContainer: {
                ChunkReader mesh = ChunkReader::Children(child);
                Chunk part;
                while (mesh.Next(part)) {
                    switch (part.id) {
                        case kChunkMeshDescriptor:
                            vertexCount = part.data.read<u32>(kDescNumVertices);
                            if (u32 tris = part.data.read<u32>(kDescNumTriangles))
                                triangleCount = tris;
                            break;
                        case kChunkMeshVertices:
                            vertexData = part.data;
                            out.vertexFileOffset = part.dataOffset;
                            break;
                        case kChunkMeshIndices:   indexData    = part.data; break;
                        case kChunkMeshMaterials: materialData = part.data; break;
                        default: break;
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    if (!ParseVertices(vertexData, vertexCount, out)) return false;
    ParseIndices(indexData, triangleCount, out);
    ParseMaterials(materialData, out);

    // Guard the draw calls: a corrupt range would otherwise read past the buffer.
    for (SolidMaterial& material : out.materials) {
        if (material.firstIndex > out.indices.size()) {
            material.firstIndex = 0;
            material.indexCount = 0;
        } else if (material.firstIndex + material.indexCount > out.indices.size()) {
            material.indexCount = (u32)(out.indices.size() - material.firstIndex);
        }
    }

    return !out.vertices.empty() && !out.indices.empty();
}

bool ParseSolidList(const Chunk& pack, SolidList& out) {
    ChunkReader reader = ChunkReader::Children(pack);
    Chunk child;

    while (reader.Next(child)) {
        if (child.id == kChunkSolidListHeader) {
            // The pack path sits in the padding chunk right after the header.
            const Chunk info = FindChunk(child, kChunkSolidListPadding);
            if (info.data.size > 0x10)
                out.path = ReadName(info.data, 0x10);
        } else if (child.id == kChunkSolidObject) {
            SolidObject object;
            if (ParseSolidObject(child, object))
                out.objects.push_back(std::move(object));
        }
    }

    return !out.objects.empty();
}

void ParseAllSolidLists(ByteSpan buffer, u64 fileOffset, std::vector<SolidList>& out) {
    ChunkReader reader(buffer, fileOffset);
    Chunk chunk;

    while (reader.Next(chunk)) {
        if (chunk.id != kChunkSolidListPack) continue;

        SolidList list;
        if (ParseSolidList(chunk, list))
            out.push_back(std::move(list));
    }
}

} // namespace nfs
