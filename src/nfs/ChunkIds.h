#pragma once

#include "core/Types.h"

// Chunk identifiers used by the NFSU2 asset containers.
//
// The high bit marks a container whose payload is a list of child chunks; the
// rest carry raw data. The ids are shared with the other Black Box titles of the
// era, which is why the numbering looks arbitrary.
namespace nfs {

enum ChunkId : u32 {
    // --- Track / world layout, found in L4Rx.BUN -------------------------------
    kChunkTrackBounds        = 0x00034112,  // world-space extents of the region
    kChunkTrackScenery       = 0x80034100,  // scenery container for one section
    kChunkSceneryHeader      = 0x00034101,
    kChunkSceneryInfos       = 0x00034102,  // model name + hash per unique model
    kChunkSceneryInstances   = 0x00034103,  // placement of each model in the world
    kChunkSceneryGroups      = 0x00034104,
    kChunkSceneryOverrides   = 0x00034106,
    kChunkSectionTable       = 0x00034110,  // streaming sections: offset/size into STREAMx.BUN
    kChunkVisibleSections    = 0x80034150,

    // --- Collision, one pack per streaming section ------------------------------
    kChunkCollisionPack      = 0x80034130,
    kChunkCollisionHeader    = 0x00034131,  // section number, grid size, bounds
    kChunkCollisionPolygons  = 0x00034134,  // 32-byte fixed-point ground polygons and barriers
    kChunkCollisionCellRefs  = 0x00034136,  // (polygon index, 4x4 sub-cell mask) per cell entry
    kChunkCollisionCells     = 0x00034137,  // (first ref, ref count) per 32-unit grid cell
    kChunkRaceBarrierGroups  = 0x0003412F,  // layout file: names of the race barrier groups (polygon byte 0)

    // --- Geometry -------------------------------------------------------------
    kChunkSolidListPack      = 0x80134000,
    kChunkSolidListHeader    = 0x80134001,
    kChunkSolidListPadding   = 0x00134002,
    kChunkSolidListHashes    = 0x00134003,  // (hash, offset) pairs for lookup
    kChunkSolidObject        = 0x80134010,
    kChunkSolidObjectHeader  = 0x00134011,  // name, hash, bounds, world matrix
    kChunkSolidObjectTexRefs = 0x00134012,  // texture hashes referenced by the materials
    kChunkSolidObjectShaders = 0x00134013,
    kChunkMeshContainer      = 0x80134100,
    kChunkMeshDescriptor     = 0x00134900,  // counts: materials, triangles, vertices
    kChunkMeshVertices       = 0x00134B01,
    kChunkMeshMaterials      = 0x00134B02,  // per-material index range + bounds
    kChunkMeshIndices        = 0x00134B03,

    // --- Textures -------------------------------------------------------------
    kChunkTexturePack        = 0xB3300000,
    kChunkTpkInfoBlock       = 0xB3310000,
    kChunkTpkHeader          = 0x33310001,  // pack name and path
    kChunkTpkHashTable       = 0x33310002,
    kChunkTpkTextureHeaders  = 0x33310004,
    kChunkTpkFormatTable     = 0x33310005,
    kChunkTpkDataBlock       = 0xB3320000,
    kChunkTpkDataHeader      = 0x33320001,
    kChunkTpkPixelData       = 0x33320002,

    // --- Effects ---------------------------------------------------------------
    // A section's placed particle emitters (the shop icon, ring and smoke among
    // them), and in L4Rx.BUN the library that defines them.
    kChunkEmitters           = 0x00035020,
    kChunkEmitterLibrary     = 0x00035021,
    kChunkAfterEmitters      = 0x80036000,  // follows the emitters in every section that has them
};

inline bool IsContainer(u32 id) { return (id & 0x80000000u) != 0; }

// Chunk payloads are preceded by a run of 0x11 filler bytes that aligns the data
// to a boundary the game can stream into directly. The filler is not a header,
// so the real payload starts at the aligned offset.
//
// Getting one of these wrong is silent and destructive: the texture pixel block
// carries 120 bytes of filler in every pack, and reading it 120 bytes early
// leaves every texture in the game displaced by a fraction of a row -- still
// texture-like, so it survives a glance, while nothing in the world matches.
inline u32 PayloadAlignment(u32 id) {
    switch (id) {
        case kChunkMeshVertices:        return 128;
        case kChunkTpkPixelData:        return 128;
        case kChunkSolidObjectTexRefs:  return 8;
        default:                        return 16;
    }
}

} // namespace nfs
