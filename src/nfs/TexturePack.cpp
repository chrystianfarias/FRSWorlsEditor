#include "nfs/TexturePack.h"

#include "core/Log.h"
#include "nfs/EaCompress.h"

#include <algorithm>
#include <cstring>

namespace nfs {

namespace {

// --- Texture header, chunk 0x33310004 --------------------------------------
constexpr size_t kTextureEntrySize = 124;
constexpr size_t kTexName          = 0x0C;  // 20 bytes, zero terminated
constexpr size_t kTexNameCapacity  = 20;
constexpr size_t kTexHash          = 0x24;
constexpr size_t kTexDataOffset    = 0x30;  // byte offset into the pixel block
constexpr size_t kTexPaletteOffset = 0x34;  // zero unless the texture is palettised
constexpr size_t kTexDataSize      = 0x38;  // bytes covering every mip level
constexpr size_t kTexPaletteSize   = 0x3C;  // 1024 for a 256-entry palette
constexpr size_t kTexBaseLevelSize = 0x40;
constexpr size_t kTexWidth         = 0x44;
constexpr size_t kTexHeight        = 0x46;
constexpr size_t kTexMipCount      = 0x4E;
// Across the 1 821 textures of the eight track streams +0x55 only ever holds
// 0..2 and +0x56 0..2, and they line up with what the game draws: every neon
// sign and flare is 2/2, the barrier tape and smoke 2/1, trees and chain-link
// fences 1/0, and the road textures -- whose alpha is gloss -- 2/0 or 0/0.
constexpr size_t kTexAlphaUsage    = 0x55;
constexpr size_t kTexAlphaBlend    = 0x56;

// --- Format entry, chunk 0x33310005 ----------------------------------------
constexpr size_t kFormatEntrySize = 32;
constexpr size_t kFormatFourCc    = 0x14;

TextureFormat FormatFromFourCc(u32 fourCc) {
    switch (fourCc) {
        case 0x31545844: return TextureFormat::Dxt1;  // "DXT1"
        case 0x33545844: return TextureFormat::Dxt3;  // "DXT3"
        case 0x35545844: return TextureFormat::Dxt5;  // "DXT5"
        case 41:         return TextureFormat::Palettised8;  // D3DFMT_P8
        default:         return TextureFormat::Unknown;
    }
}

std::string ReadFixedName(ByteSpan span, size_t offset, size_t capacity) {
    if (offset >= span.size) return {};
    const size_t available = std::min(capacity, span.size - offset);
    const char* text = (const char*)span.data + offset;
    return std::string(text, ::strnlen(text, available));
}

} // namespace

const char* ToString(TextureFormat format) {
    switch (format) {
        case TextureFormat::Dxt1: return "DXT1";
        case TextureFormat::Dxt3: return "DXT3";
        case TextureFormat::Dxt5: return "DXT5";
        case TextureFormat::Palettised8: return "P8";
        default:                  return "unknown";
    }
}

static Texture ReadHeader(ByteSpan entry, ByteSpan format) {
    Texture texture;
    texture.name          = ReadFixedName(entry, kTexName, kTexNameCapacity);
    texture.hash          = entry.read<u32>(kTexHash);
    texture.width         = entry.read<u16>(kTexWidth);
    texture.height        = entry.read<u16>(kTexHeight);
    texture.mipCount      = entry.read<u8>(kTexMipCount);
    texture.baseLevelSize = entry.read<u32>(kTexBaseLevelSize);
    texture.alphaUsage    = (AlphaUsage)std::min<u8>(entry.read<u8>(kTexAlphaUsage), 2);
    texture.alphaBlend    = (AlphaBlend)std::min<u8>(entry.read<u8>(kTexAlphaBlend), 2);
    if (!format.empty()) texture.format = FormatFromFourCc(format.read<u32>(kFormatFourCc));
    if (texture.mipCount == 0) texture.mipCount = 1;
    return texture;
}

bool TexturePack::ParseCompressed(const Chunk& packChunk, ByteSpan index) {
    // Index entries: hash, absolute file offset, compressed size, size, flags, 0.
    const u8* file = packChunk.data.data - packChunk.dataOffset;
    const u64 fileEnd = packChunk.dataOffset + packChunk.data.size;
    constexpr size_t kEntry = 24;
    constexpr size_t kTail = kTextureEntrySize + kFormatEntrySize;
    size_t failed = 0;
    for (size_t i = 0; i + kEntry <= index.size; i += kEntry) {
        const u32 offset = index.read<u32>(i + 4), size = index.read<u32>(i + 8);
        if (offset < packChunk.dataOffset || (u64)offset + size > fileEnd) { ++failed; continue; }
        std::vector<u8> block;
        if (!EaDecompress({ file + offset, size }, block) || block.size() < kTail) { ++failed; continue; }
        owned_.push_back(std::move(block));
        const ByteSpan all{ owned_.back().data(), owned_.back().size() };
        const ByteSpan entry = all.sub(all.size - kTail, kTextureEntrySize);
        Texture texture = ReadHeader(entry, all.sub(all.size - kFormatEntrySize, kFormatEntrySize));
        texture.pixels = all.sub(0, std::min<size_t>(entry.read<u32>(kTexDataSize), all.size - kTail));
        if (texture.format == TextureFormat::Palettised8) { ++failed; continue; }   // none ship
        byHash_.emplace(texture.hash, textures_.size());
        textures_.push_back(std::move(texture));
    }
    LOG_INFO("compressed texture pack '%s': %zu textures, %zu unreadable", name_.c_str(), textures_.size(), failed);
    return !textures_.empty();
}

bool TexturePack::Parse(const Chunk& packChunk) {
    textures_.clear();
    byHash_.clear();
    owned_.clear();

    ByteSpan headers, formats, pixels, compressed;

    ChunkReader top = ChunkReader::Children(packChunk);
    Chunk block;
    while (top.Next(block)) {
        if (block.id == kChunkTpkInfoBlock) {
            ChunkReader info = ChunkReader::Children(block);
            Chunk part;
            while (info.Next(part)) {
                switch (part.id) {
                    case kChunkTpkHeader:
                        name_ = ReadFixedName(part.data, 0x0C, 28);
                        break;
                    case kChunkTpkTextureHeaders: headers = part.data; break;
                    case kChunkTpkFormatTable:    formats = part.data; break;
                    case kChunkTpkCompressedIndex: compressed = part.data; break;
                    default: break;
                }
            }
        } else if (block.id == kChunkTpkDataBlock) {
            ChunkReader data = ChunkReader::Children(block);
            Chunk part;
            while (data.Next(part)) {
                if (part.id == kChunkTpkPixelData) pixels = part.data;
            }
        }
    }

    if (headers.empty() && !compressed.empty()) return ParseCompressed(packChunk, compressed);
    if (headers.empty() || pixels.empty()) {
        LOG_WARN("texture pack '%s' is missing headers or pixel data", name_.c_str());
        return false;
    }

    const size_t count = headers.size / kTextureEntrySize;
    textures_.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const ByteSpan entry = headers.sub(i * kTextureEntrySize, kTextureEntrySize);

        // The format table runs parallel to the header table.
        const ByteSpan format = (i + 1) * kFormatEntrySize <= formats.size
                                    ? formats.sub(i * kFormatEntrySize, kFormatEntrySize) : ByteSpan{};
        Texture texture = ReadHeader(entry, format);

        const u32 dataOffset = entry.read<u32>(kTexDataOffset);
        const u32 dataSize   = entry.read<u32>(kTexDataSize);
        texture.pixels = pixels.sub(dataOffset, dataSize);

        // Palettes live in the gap the pixel block leaves before its first
        // texture slice, addressed the same way as the pixels.
        if (const u32 paletteSize = entry.read<u32>(kTexPaletteSize))
            texture.palette = pixels.sub(entry.read<u32>(kTexPaletteOffset), paletteSize);

        byHash_.emplace(texture.hash, textures_.size());
        textures_.push_back(std::move(texture));
    }

    LOG_INFO("texture pack '%s': %zu textures", name_.c_str(), textures_.size());
    return !textures_.empty();
}

const Texture* TexturePack::Find(u32 hash) const {
    const auto it = byHash_.find(hash);
    return it == byHash_.end() ? nullptr : &textures_[it->second];
}

} // namespace nfs
