#include "io/SectionRebuild.h"
#include "io/TpkBuild.h"

#include "core/Log.h"
#include "nfs/ChunkIds.h"

#include <cstring>

namespace {

// --- the fields this has to rewrite ----------------------------------------
constexpr size_t kHdrNumTris   = 0x14;
constexpr size_t kHdrBoundsMin = 0x20;
constexpr size_t kHdrBoundsMax = 0x30;

constexpr size_t kDescNumMaterials = 0x10;

// How many textures the object has, as one byte in its header.
//
// The list of hashes is a chunk of its own and its length is implied by the
// chunk's size, so this looks redundant -- but it is what the game reads. It
// matches the number of entries in every one of the 155 objects in this
// section, and leaving it behind when the list grows is fatal: the game sizes
// the object's texture array from it, and a material referring to the entry
// that was added then indexes one past the end.
constexpr size_t kHdrNumTextures = 0x19;
constexpr size_t kDescNumTriangles = 0x24;
constexpr size_t kDescNumVertices  = 0x34;

constexpr size_t kMaterialEntrySize = 60;
constexpr size_t kMatBoundsMin      = 0x00;
constexpr size_t kMatIndexCount     = 0x0C;
constexpr size_t kMatBoundsMax      = 0x10;
constexpr size_t kMatTextureIndex   = 0x1C;
constexpr size_t kMatFirstIndex     = 0x34;

u32 Read32(const u8* at) { u32 v; std::memcpy(&v, at, 4); return v; }

void Write32(std::vector<u8>& out, size_t at, u32 value) {
    std::memcpy(out.data() + at, &value, 4);
}

void WriteVec3(std::vector<u8>& out, size_t at, const Vec3& value) {
    std::memcpy(out.data() + at, &value.x, 4);
    std::memcpy(out.data() + at + 4, &value.y, 4);
    std::memcpy(out.data() + at + 8, &value.z, 4);
}

u64 AlignUp(u64 value, u64 to) { return (value + to - 1) / to * to; }

// Appends one chunk, padding its payload the way the format does.
//
// The filler is 0x11 bytes and its length comes from where the payload lands in
// the file, not from where it lands in this buffer -- which is why the base
// offset travels with the writer. Getting this wrong shifts every payload in the
// section by a few bytes and the game reads garbage.
struct ChunkWriter {
    std::vector<u8>& out;
    u64 base;      // the file offset the buffer's first byte will have

    // `filler` is given rather than derived, because the format does not pad
    // uniformly. A leaf chunk's payload is aligned -- the vertex block to 128
    // bytes, the rest to 16 -- but a container's children start right after
    // its header with no padding at all. Padding a container anyway pushes
    // every chunk after it out of step, and the game reads a vertex stride of
    // 25 where the file says 24.
    size_t Begin(u32 id, size_t filler) {
        const size_t headerAt = out.size();
        out.resize(out.size() + 8);
        std::memcpy(out.data() + headerAt, &id, 4);
        out.insert(out.end(), filler, 0x11);
        return headerAt;
    }

    // The padding a leaf chunk's payload needs where it is about to land.
    size_t FillerFor(u32 alignment) const {
        const u64 payloadAt = base + out.size() + 8;
        return (size_t)(AlignUp(payloadAt, alignment) - payloadAt);
    }

    void End(size_t headerAt) {
        const u32 size = (u32)(out.size() - headerAt - 8);
        std::memcpy(out.data() + headerAt + 4, &size, 4);
    }

    void Bytes(const u8* data, size_t count) {
        out.insert(out.end(), data, data + count);
    }
};

// Walks the children of a container that is already in the file.
struct ChunkSpan {
    u32 id = 0;
    const u8* header = nullptr;   // the 8-byte header
    const u8* payload = nullptr;  // past the filler
    size_t payloadSize = 0;
    size_t totalSize = 0;         // header plus everything it covers
};

std::vector<ChunkSpan> Children(const u8* stream, u64 base, const u8* at, size_t length) {
    std::vector<ChunkSpan> children;
    size_t offset = 0;

    while (offset + 8 <= length) {
        const u32 id = Read32(at + offset);
        const u32 size = Read32(at + offset + 4);

        // An empty marker is eight bytes of the file like any other. Skipping
        // it while reading is harmless; skipping it while rewriting drops it,
        // and everything after moves eight bytes -- enough to throw every
        // 128-byte aligned vertex block out of step.
        if (id == 0 && size == 0) {
            ChunkSpan marker;
            marker.id        = 0;
            marker.header    = at + offset;
            marker.totalSize = 8;
            marker.payload   = at + offset + 8;
            children.push_back(marker);
            offset += 8;
            continue;
        }
        if (size == 0 || offset + 8 + size > length) break;

        ChunkSpan child;
        child.id        = id;
        child.header    = at + offset;
        child.totalSize = 8 + size;

        const u8* payload = at + offset + 8;
        const u32 alignment = nfs::PayloadAlignment(id);
        const u64 payloadAt = base + (payload - stream);
        u64 filler = AlignUp(payloadAt, alignment) - payloadAt;

        if (filler > size) filler = 0;
        for (u64 i = 0; i < filler; ++i)
            if (payload[i] != 0x11) { filler = 0; break; }

        child.payload     = payload + filler;
        child.payloadSize = size - (size_t)filler;

        children.push_back(child);
        offset += 8 + size;
    }
    return children;
}


// Re-emits a texture pack with new textures in it.
//
// The pack's shape is preserved exactly: the same chunks in the same order,
// each still carrying its own filler, with only the four tables that describe
// textures replaced. Anything else inside it -- the pack header naming the
// file, the data header, the zero-length markers between blocks -- is copied
// across byte for byte, because none of it counts textures.

// The payload of every texture data block in a finished section.
//
// This is what the region's table subtracts from a section's size to get its
// second figure, so it decides where the game believes the pixels end. Counted
// from the emitted bytes, which is the only copy guaranteed to be what the game
// will actually read.
u64 TextureDataBytes(const std::vector<u8>& section) {
    u64 total = 0;

    const auto walk = [&](auto&& self, size_t begin, size_t end) -> void {
        size_t at = begin;
        while (at + 8 <= end) {
            u32 id = 0, size = 0;
            std::memcpy(&id, section.data() + at, 4);
            std::memcpy(&size, section.data() + at + 4, 4);

            // A zero-length marker is eight bytes and carries nothing.
            if (size < 8) { at += 8; continue; }
            if (at + 8 + size > end) return;

            if (id == nfs::kChunkTpkDataBlock) {
                total += size;
            } else if (id == nfs::kChunkTexturePack) {
                self(self, at + 8, at + 8 + size);
            }

            at += 8 + size;
        }
    };

    walk(walk, 0, section.size());
    return total;
}

bool WriteTexturePack(ChunkWriter& writer, const u8* stream, u64 sectionOffset,
                      const ChunkSpan& pack, const std::vector<NewTexture>& textures,
                      std::wstring& error) {
    const std::vector<ChunkSpan> blocks =
        Children(stream, sectionOffset, pack.payload, pack.payloadSize);

    // Gather what is there now.
    //
    // Copied out, not pointed at. Children() returns its vector by value, so a
    // reference into one taken in the loop header dangles the moment that
    // iteration ends -- the tables would be read back from freed memory, and a
    // pack built from that takes every object after it in the section down with
    // it.
    TpkTables original;
    bool haveHeaders = false, haveFormats = false, havePixels = false;

    for (const ChunkSpan& block : blocks) {
        if (!nfs::IsContainer(block.id)) continue;

        const std::vector<ChunkSpan> parts =
            Children(stream, sectionOffset, block.payload, block.payloadSize);

        for (const ChunkSpan& part : parts) {
            const auto take = [&part] {
                return std::vector<u8>(part.payload, part.payload + part.payloadSize);
            };
            switch (part.id) {
                case nfs::kChunkTpkHashTable:      original.hashTable = take(); break;
                case nfs::kChunkTpkTextureHeaders: original.headers = take(); haveHeaders = true; break;
                case nfs::kChunkTpkFormatTable:    original.formats = take(); haveFormats = true; break;
                case nfs::kChunkTpkPixelData:      original.pixels = take();  havePixels  = true; break;
                default: break;
            }
        }
    }

    if (!haveHeaders || !haveFormats || !havePixels) {
        // A section whose pack has no texture tables has nothing to add to.
        writer.Bytes(pack.header, pack.totalSize);
        return true;
    }

    TpkTables rebuilt;
    if (!BuildTpkTables(original, textures, rebuilt, error)) return false;

    const std::vector<u8> packedPixels = rebuilt.pixels;

    // Whatever this pack grows by has to be a multiple of 128.
    //
    // The pack sits near the front of the section, so growing it pushes every
    // object behind it along. Those objects are copied byte for byte, filler
    // and all -- and their filler was sized for where they used to be. A vertex
    // chunk is aligned by its absolute offset in the file, so a shift that is
    // not a whole number of alignment units leaves every one of them starting a
    // few bytes off: the stride reads as 25 or 28, the buffer stops being a
    // whole number of vertices, and the objects simply stop loading.
    //
    // Rather than re-aligning every object behind the pack, the pack absorbs
    // the remainder itself. The pixel block already carries unused slack at the
    // end, so extending it costs nothing and disturbs nothing.
    const size_t before = writer.out.size();
    size_t padding = 0;

    for (int attempt = 0; attempt < 4; ++attempt) {
        writer.out.resize(before);

        rebuilt.pixels = packedPixels;
        rebuilt.pixels.resize(packedPixels.size() + padding, 0);

        const size_t packAt = writer.Begin(pack.id, 0);

        for (const ChunkSpan& block : blocks) {
            if (!nfs::IsContainer(block.id)) {
                writer.Bytes(block.header, block.totalSize);
                continue;
            }

            const size_t blockAt = writer.Begin(block.id, 0);

            for (const ChunkSpan& part :
                 Children(stream, sectionOffset, block.payload, block.payloadSize)) {

                const std::vector<u8>* replacement = nullptr;
                switch (part.id) {
                    case nfs::kChunkTpkHashTable:      replacement = &rebuilt.hashTable; break;
                    case nfs::kChunkTpkTextureHeaders: replacement = &rebuilt.headers;   break;
                    case nfs::kChunkTpkFormatTable:    replacement = &rebuilt.formats;   break;
                    case nfs::kChunkTpkPixelData:      replacement = &rebuilt.pixels;    break;
                    default: break;
                }

                if (!replacement) {
                    writer.Bytes(part.header, part.totalSize);
                    continue;
                }

                // The filler this chunk had, not the filler its id would
                // suggest. In the shipped packs the three tables carry none at
                // all -- their payloads sit at offsets that are not multiples
                // of sixteen, let alone of anything larger -- so the game reads
                // them from the first byte after the header. Putting the
                // default sixteen-byte filler in front of the texture headers,
                // as this did, hands it eight bytes of 0x11 to read as the
                // start of a texture. Only the pixel block genuinely aligns,
                // and it is the one chunk whose position actually moves.
                const size_t had = part.totalSize - 8 - part.payloadSize;
                const size_t filler =
                    part.id == nfs::kChunkTpkPixelData
                        ? writer.FillerFor(nfs::PayloadAlignment(part.id))
                        : had;

                const size_t at = writer.Begin(part.id, filler);
                writer.Bytes(replacement->data(), replacement->size());
                writer.End(at);
            }

            writer.End(blockAt);
        }

        writer.End(packAt);

        // Measured rather than predicted. The filler in front of each chunk
        // depends on where that chunk landed, which depends on the padding --
        // so the only trustworthy size is the one that comes out of writing it.
        const size_t written = writer.out.size() - before;
        const long long shift = (long long)written - (long long)pack.totalSize;
        const long long stray = ((shift % 128) + 128) % 128;

        if (stray == 0) {
            LOG_INFO("texture pack grew by %lld bytes, a whole number of 128-byte "
                     "units (%zu bytes of padding)", shift, padding);
            return true;
        }

        padding += (size_t)(128 - stray);
    }

    error = L"The texture pack could not be padded to keep the objects behind it "
            L"on their alignment.";
    return false;
}

} // namespace

bool RebuildSection(const u8* stream, size_t streamSize,
                    u64 sectionOffset, u32 sectionSize, u32 roomAvailable,
                    u32 targetHash, const ReplacementMesh& mesh,
                    const std::vector<NewTexture>& textures,
                    RebuiltSection& out, std::wstring& error) {
    if (sectionOffset + sectionSize > streamSize) {
        error = L"That section runs past the end of the stream.";
        return false;
    }
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        error = L"The replacement mesh is empty.";
        return false;
    }

    const u8* sectionAt = stream + sectionOffset;

    out.bytes.clear();
    out.bytes.reserve(sectionSize + 65536);
    out.fileOffset   = sectionOffset;
    out.originalSize = sectionSize;

    ChunkWriter writer{ out.bytes, sectionOffset };
    bool replaced = false;
    bool packDone = false;

    for (const ChunkSpan& top : Children(stream, sectionOffset, sectionAt, sectionSize)) {
        // Only a solid list pack can hold the object, so everything else -- the
        // texture packs, the scenery, the visibility data -- is carried over
        // byte for byte.
        // Only the first pack in the section takes them. A section can hold
        // several, and adding the same textures to each would pay for every one
        // of them again while the material only ever looks the texture up once.
        if (top.id == nfs::kChunkTexturePack && !textures.empty() && !packDone) {
            packDone = true;
            if (!WriteTexturePack(writer, stream, sectionOffset, top, textures, error))
                return false;
            continue;
        }

        if (top.id != nfs::kChunkSolidListPack) {
            writer.Bytes(top.header, top.totalSize);
            continue;
        }

        const size_t packHeaderAt = writer.Begin(top.id, 0);

        for (const ChunkSpan& inPack :
             Children(stream, sectionOffset, top.payload, top.payloadSize)) {

            if (inPack.id != nfs::kChunkSolidObject) {
                writer.Bytes(inPack.header, inPack.totalSize);
                continue;
            }

            // Is this the object being replaced? Its hash is in its header.
            const std::vector<ChunkSpan> parts =
                Children(stream, sectionOffset, inPack.payload, inPack.payloadSize);

            const ChunkSpan* objectHeader = nullptr;
            for (const ChunkSpan& part : parts)
                if (part.id == nfs::kChunkSolidObjectHeader) objectHeader = &part;

            const bool isTarget = objectHeader && objectHeader->payloadSize >= 0x18 &&
                                  Read32(objectHeader->payload + 0x10) == targetHash;

            if (!isTarget) {
                writer.Bytes(inPack.header, inPack.totalSize);
                continue;
            }

            // --- the object itself ------------------------------------------
            const size_t objectHeaderAt =
                writer.Begin(inPack.id, 0);

            Aabb bounds;
            for (const nfs::SolidVertex& vertex : mesh.vertices) bounds.Add(vertex.position);

            for (const ChunkSpan& part : parts) {
                if (part.id == nfs::kChunkSolidObjectTexRefs &&
                    !mesh.textureHashes.empty()) {
                    // Eight bytes an entry, of which only the leading hash has
                    // a settled meaning. The other four are taken from an entry
                    // the game already accepts rather than invented.
                    const size_t at = writer.Begin(
                        part.id, writer.FillerFor(nfs::PayloadAlignment(part.id)));

                    u8 tail[4] = { 0, 0, 0, 0 };
                    if (part.payloadSize >= 8) std::memcpy(tail, part.payload + 4, 4);

                    for (const u32 hash : mesh.textureHashes) {
                        u8 entry[8];
                        std::memcpy(entry, &hash, 4);
                        std::memcpy(entry + 4, tail, 4);
                        writer.Bytes(entry, sizeof(entry));
                    }

                    writer.End(at);
                    continue;
                }

                if (part.id == nfs::kChunkSolidObjectHeader) {
                    // Kept as it was apart from the counts and the box, which no
                    // longer describe the mesh that is going in.
                    const size_t at = writer.Begin(part.id, part.totalSize - 8 - part.payloadSize);
                    const size_t payloadAt = writer.out.size();
                    writer.Bytes(part.payload, part.payloadSize);

                    Write32(writer.out, payloadAt + kHdrNumTris, (u32)(mesh.indices.size() / 3));

                    if (!mesh.textureHashes.empty() &&
                        payloadAt + kHdrNumTextures < writer.out.size())
                        writer.out[payloadAt + kHdrNumTextures] =
                            (u8)std::min<size_t>(mesh.textureHashes.size(), 255);
                    WriteVec3(writer.out, payloadAt + kHdrBoundsMin, bounds.min);
                    WriteVec3(writer.out, payloadAt + kHdrBoundsMax, bounds.max);
                    writer.End(at);

                } else if (part.id == nfs::kChunkMeshContainer) {
                    const size_t meshAt = writer.Begin(part.id, 0);

                    for (const ChunkSpan& piece :
                         Children(stream, sectionOffset, part.payload, part.payloadSize)) {

                        if (piece.id == nfs::kChunkMeshDescriptor) {
                            const size_t at =
                                writer.Begin(piece.id, writer.FillerFor(nfs::PayloadAlignment(piece.id)));
                            const size_t payloadAt = writer.out.size();
                            writer.Bytes(piece.payload, piece.payloadSize);

                            Write32(writer.out, payloadAt + kDescNumMaterials,
                                    (u32)mesh.materials.size());
                            Write32(writer.out, payloadAt + kDescNumTriangles,
                                    (u32)(mesh.indices.size() / 3));
                            Write32(writer.out, payloadAt + kDescNumVertices,
                                    (u32)mesh.vertices.size());
                            writer.End(at);

                        } else if (piece.id == nfs::kChunkMeshVertices) {
                            const size_t at =
                                writer.Begin(piece.id, writer.FillerFor(nfs::PayloadAlignment(piece.id)));

                            // Packed into the section's own layout rather than
                            // the editor's. Only two exist across every shipped
                            // stream, and they differ by whether a normal is
                            // stored at all.
                            const u32 stride = mesh.stride == 24 ? 24u : 36u;
                            const size_t colourAt = stride == 24 ? 12 : 24;
                            const size_t uvAt     = stride == 24 ? 16 : 28;

                            std::vector<u8> packed(mesh.vertices.size() * stride, 0);
                            for (size_t v = 0; v < mesh.vertices.size(); ++v) {
                                const nfs::SolidVertex& vertex = mesh.vertices[v];
                                u8* to = packed.data() + v * stride;

                                std::memcpy(to, &vertex.position, 12);
                                if (stride == 36) std::memcpy(to + 12, &vertex.normal, 12);
                                std::memcpy(to + colourAt, &vertex.color, 4);
                                std::memcpy(to + uvAt, &vertex.uv, 8);
                            }

                            writer.Bytes(packed.data(), packed.size());
                            writer.End(at);

                        } else if (piece.id == nfs::kChunkMeshIndices) {
                            const size_t at =
                                writer.Begin(piece.id, writer.FillerFor(nfs::PayloadAlignment(piece.id)));
                            writer.Bytes((const u8*)mesh.indices.data(),
                                         mesh.indices.size() * sizeof(u16));
                            writer.End(at);

                        } else if (piece.id == nfs::kChunkMeshMaterials) {
                            const size_t at =
                                writer.Begin(piece.id, writer.FillerFor(nfs::PayloadAlignment(piece.id)));

                            // Each entry keeps whatever the original had in the
                            // fields this does not understand; only the range,
                            // the texture and the box are rewritten. Copying a
                            // template rather than zeroing keeps flags whose
                            // meaning is still unknown from being lost.
                            const u8* template_ = piece.payload;
                            const size_t templates = piece.payloadSize / kMaterialEntrySize;

                            for (size_t m = 0; m < mesh.materials.size(); ++m) {
                                const size_t entryAt = writer.out.size();
                                const size_t source = templates ? (m < templates ? m : 0) : 0;

                                if (templates)
                                    writer.Bytes(template_ + source * kMaterialEntrySize,
                                                 kMaterialEntrySize);
                                else
                                    writer.out.resize(writer.out.size() + kMaterialEntrySize, 0);

                                const nfs::SolidMaterial& material = mesh.materials[m];
                                Write32(writer.out, entryAt + kMatFirstIndex, material.firstIndex);
                                Write32(writer.out, entryAt + kMatIndexCount, material.indexCount);
                                Write32(writer.out, entryAt + kMatTextureIndex, material.textureIndex);
                                WriteVec3(writer.out, entryAt + kMatBoundsMin, material.bounds.min);
                                WriteVec3(writer.out, entryAt + kMatBoundsMax, material.bounds.max);
                            }
                            writer.End(at);

                        } else {
                            writer.Bytes(piece.header, piece.totalSize);
                        }
                    }
                    writer.End(meshAt);

                } else {
                    writer.Bytes(part.header, part.totalSize);
                }
            }

            writer.End(objectHeaderAt);

            // Everything after this object shifts by however much it grew, and
            // a vertex block is aligned to 128 bytes by its absolute position
            // in the file. Shift the objects that follow by anything else and
            // their padding stops being padding: the reader counts it as
            // vertex data and reports a stride of 25 where the file says 24.
            //
            // Rounding the growth up to a multiple of 128 leaves every later
            // chunk at the same offset modulo its alignment, so copies that
            // were correct stay correct. The padding is a zero-id chunk, which
            // is what the format already uses between objects.
            // The object can shrink as well as grow, so the difference is
            // signed. Padding only ever adds, and what it has to add is
            // whatever brings the shift back to a multiple of 128 -- which for
            // a mesh that lost geometry means padding almost a whole block.
            const long long grownTo = (long long)(writer.out.size() - objectHeaderAt);
            const long long delta   = grownTo - (long long)inPack.totalSize;

            size_t pad = (size_t)(((-delta) % 128 + 128) % 128);
            if (pad != 0 && pad < 8) pad += 128;

            if (pad) {
                const size_t at = writer.Begin(0, 0);
                writer.out.insert(writer.out.end(), pad - 8, 0);
                writer.End(at);
            }

            replaced = true;
        }

        writer.End(packHeaderAt);
    }

    if (!replaced) {
        error = L"That mesh is not in the section it was exported from.";
        return false;
    }

    if (out.bytes.size() > roomAvailable) {
        wchar_t message[256];
        ::_snwprintf_s(message, _TRUNCATE,
                       L"The rebuilt section needs %zu bytes and only %u are free before "
                       L"the next one starts.",
                       out.bytes.size(), roomAvailable);
        error = message;
        return false;
    }

    // Shorter is fine: the tail is zeroed, which is what the format already uses
    // between sections, and the size in the table still covers it.
    if (out.bytes.size() < sectionSize) out.bytes.resize(sectionSize, 0);

    out.grew = out.bytes.size() > sectionSize;

    out.geometrySize = (u32)(out.bytes.size() - TextureDataBytes(out.bytes));

    LOG_INFO("section rebuilt: %u bytes became %zu (%u free)",
             sectionSize, out.bytes.size(), roomAvailable);
    return true;
}

u32 SectionGeometrySize(const std::vector<u8>& section) {
    return (u32)(section.size() - TextureDataBytes(section));
}

bool RebuildTexturePackChunk(ByteSpan pack, u64 fileOffset, const std::vector<NewTexture>& textures,
                             std::vector<u8>& out, std::wstring& error) {
    if (pack.size < 8 || Read32(pack.data) != nfs::kChunkTexturePack) {
        error = L"That is not a texture pack.";
        return false;
    }
    ChunkSpan span;
    span.id          = nfs::kChunkTexturePack;
    span.header      = pack.data;
    span.payload     = pack.data + 8;
    span.payloadSize = Read32(pack.data + 4);
    span.totalSize   = 8 + span.payloadSize;

    out.clear();
    ChunkWriter writer{ out, fileOffset };
    return WriteTexturePack(writer, pack.data, fileOffset, span, textures, error);
}
