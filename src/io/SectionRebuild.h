#pragma once

#include "core/Types.h"
#include "io/TextureImport.h"
#include "nfs/SolidList.h"

#include <string>
#include <vector>

// Rewrites one streaming section with a mesh that changed size.
//
// Moving vertices about can be done in place, because nothing changes length.
// Adding or removing them cannot: the vertex and index chunks grow or shrink,
// every container above them records a size, and the section itself has a length
// in the region's table. So the whole section is rebuilt -- every chunk copied
// across untouched except the one object being replaced, with the sizes of its
// parents recomputed on the way out.
//
// Chunk payloads are aligned by their absolute position in the file, so the
// section has to be rebuilt knowing where it will sit. That is why the offset is
// an argument rather than something the caller patches afterwards.
struct RebuiltSection {
    std::vector<u8> bytes;        // the section's new contents, start to finish
    u64  fileOffset   = 0;        // where they go in the stream
    u32  originalSize = 0;
    bool grew         = false;    // true when the table's size field has to change

    // What goes in the table's second size field.
    //
    // That field is the section's size less the payload of every texture data
    // block in it -- the compressed pixels, which the game accounts for apart
    // from everything else. Measured on the shipped section it is exact: 1 251
    // 728 total less 401 584 of pixel payload is the 850 144 the table holds.
    //
    // It is counted from the bytes that were just written rather than carried
    // along as a running delta, so it cannot drift out of step with them.
    u32 geometrySize = 0;
};

// The replacement geometry, already in the space the mesh is stored in.
struct ReplacementMesh {
    // The layout the section already uses: 24 bytes without a normal, 36 with
    // one. Writing the editor's own 36-byte vertex into a section that stores
    // 24 hands the game a buffer it reads at the wrong step, and every vertex
    // after the first is nonsense.
    u32 stride = 36;

    std::vector<nfs::SolidVertex>  vertices;
    std::vector<u16>               indices;
    std::vector<nfs::SolidMaterial> materials;   // ranges into `indices`

    // The object's own list of texture hashes, which its materials index into.
    // A new texture has to appear here as well as in the pack, or the material
    // pointing at it indexes past the end of a list that never grew. Empty
    // leaves the object's list exactly as it was.
    std::vector<u32> textureHashes;
};

// Rebuilds one whole texture pack chunk (0xB3300000, header included) with
// `textures` added, for a chunk that will land at `fileOffset`. The pack keeps
// its shape; only the four texture tables change.
bool RebuildTexturePackChunk(ByteSpan pack, u64 fileOffset, const std::vector<NewTexture>& textures,
                             std::vector<u8>& out, std::wstring& error);

// The table's second size field for a finished section: its size less the
// payload of every texture data block in it. See RebuiltSection::geometrySize.
u32 SectionGeometrySize(const std::vector<u8>& section);

// `stream` is the whole STREAM<region>.BUN, mapped. `sectionOffset` and
// `sectionSize` come from the region's table.
//
// Returns false and fills `error` when the section cannot be rebuilt: an object
// that is not in it, or a result that will not fit in the room available.
// `textures` are added to the section's own texture pack. A section carries
// the textures its objects use, so a new one has to go in beside them: putting
// it anywhere else would leave the material pointing at a hash this section
// cannot resolve. An empty list leaves the pack exactly as it was.
bool RebuildSection(const u8* stream, size_t streamSize,
                    u64 sectionOffset, u32 sectionSize, u32 roomAvailable,
                    u32 targetHash, const ReplacementMesh& mesh,
                    const std::vector<NewTexture>& textures,
                    RebuiltSection& out, std::wstring& error);
