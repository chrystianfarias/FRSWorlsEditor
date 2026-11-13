#pragma once

#include "core/Types.h"
#include "nfs/ChunkReader.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace nfs {

// Pixel formats that appear in the shipped track packs.
enum class TextureFormat {
    Unknown,
    Dxt1,
    Dxt3,
    Dxt5,
    // D3DFMT_P8: one byte per texel into a 256-entry palette. Only 23 textures
    // in a region use it, but they are the road lanes, intersections, parking
    // bays and pavements -- 3986 of the 27 987 material slots, which is every
    // street surface in the city.
    Palettised8,
};

const char* ToString(TextureFormat format);

// How a texture's alpha is used, from +0x55 of its header.
enum class AlphaUsage : u8 {
    None       = 0,   // alpha is not opacity (the road textures keep gloss there)
    PunchThru  = 1,   // cut-out: trees, chain-link fences, railings
    Modulated  = 2,   // opacity that takes part in blending
};

// How the texture combines with what is behind it, from +0x56.
enum class AlphaBlend : u8 {
    SrcCopy  = 0,     // replaces what is behind it
    Blend    = 1,     // src * alpha + dst * (1 - alpha): smoke, glass, barrier tape
    Additive = 2,     // src * alpha + dst: neon, flares, light beams on black
};

// One texture inside a pack.
//
// Materials reference textures by hash, so the pack keeps a hash lookup. The
// pixel data is not copied: `pixels` points into the mapped stream file and
// stays valid as long as the mapping does. Textures of a compressed pack point
// into the pack's own decompressed copy instead.
struct Texture {
    std::string   name;
    u32           hash   = 0;
    u16           width  = 0;
    u16           height = 0;
    u8            mipCount = 1;
    TextureFormat format = TextureFormat::Unknown;
    AlphaUsage    alphaUsage = AlphaUsage::None;
    AlphaBlend    alphaBlend = AlphaBlend::SrcCopy;

    // Scrolling, from +0x52 (type: 0 none, 1 smooth, 2 in steps) and the
    // 8.8 fixed-point values at +0x58: seconds a step, texture lengths a
    // second along S and T. The fountains' and waterfalls' water flows by
    // it, rolling neon signs turn by it.
    u8    scrollType = 0;
    float scrollStep = 1.0f;
    float scrollS = 0.0f, scrollT = 0.0f;

    u32 baseLevelSize = 0;   // bytes of mip 0
    ByteSpan pixels;         // all mip levels, tightly packed
    ByteSpan palette;        // 256 entries, empty unless the format is palettised
};

// A 0xB3300000 texture pack.
class TexturePack {
public:
    // Parses one pack chunk. `out` textures point into the buffer the chunk
    // came from, so keep the file mapped for as long as the pack is used.
    bool Parse(const Chunk& packChunk);

    const std::vector<Texture>& Textures() const { return textures_; }

    const Texture* Find(u32 hash) const;

    const std::string& Name() const { return name_; }

private:
    // A compressed pack (TRACKS\LOC4DYNTEX.BIN: the sky, the weather and the
    // other textures the game swaps) keeps each texture as an EA-compressed
    // block holding its pixels, then its 124-byte header and 32-byte format.
    bool ParseCompressed(const Chunk& packChunk, ByteSpan index);

    std::string          name_;
    std::vector<Texture> textures_;
    std::vector<std::vector<u8>> owned_;   // decompressed blocks
    std::unordered_map<u32, size_t> byHash_;
};

} // namespace nfs
