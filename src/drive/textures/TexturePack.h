#pragma once

// Port of the parts of NFSU2Unity Core/Textures/TexturePack.cs and TextureDecode.cs the car
// model needs: raw packs (BUN files) and streaming packs (car TEXTURES.BIN, one EA-compressed
// blob per texture, decoded on first use).

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bb {

// TextureInfo.ImageCompressionType values.
enum class TextureFormat : uint8_t {
    P8 = 8,
    Argb32 = 32,
    Dxt1 = 34,
    Dxt3 = 36,
    Dxt5 = 38,
};

enum class AlphaUsage : int8_t { None = 0, PunchThrough = 1, Modulated = 2 };

enum class AlphaBlend : int8_t {
    SrcCopy = 0, Blend = 1, Additive = 2, Subtractive = 3, Overbright = 4,
    DestBlend = 5, DestAdditive = 6, DestSubtractive = 7, DestOverbright = 8,
};

// One texture: decoded TextureInfo (0x7C) plus image/palette bytes (row 0 = top, full mip chain).
class Texture {
public:
    std::string Name;
    uint32_t NameHash = 0;
    uint32_t ClassNameHash = 0;
    int Width = 0, Height = 0;
    TextureFormat Format = TextureFormat::Dxt1;
    int NumMipMapLevels = 1;
    int NumPaletteEntries = 0;
    int8_t RenderingOrder = 0;
    bool ApplyAlphaSorting = false;
    AlphaUsage AlphaUsage_ = AlphaUsage::None;   // C# AlphaUsage
    AlphaBlend AlphaBlend_ = AlphaBlend::SrcCopy; // C# AlphaBlend
    uint8_t Flags = 0;
    int8_t ScrollType = 0;
    uint32_t D3DFormat = 0;
    std::vector<uint8_t> Image;
    std::vector<uint8_t> Palette;

    // Byte size of one mip level; 0 for an unknown format.
    int MipSize(int level) const;

    // TextureDecode.ToRgba32 but as D3DCOLOR (0xAARRGGBB) texels. False for an unknown format.
    bool ToArgb32(int level, std::vector<uint32_t>& out, int& width, int& height) const;

    static Texture FromInfo(const uint8_t* info);   // 0x7C bytes
};

class TexturePack {
public:
    static constexpr uint32_t Root = 0xB3300000;

    std::string Name;
    bool Streaming = false;
    std::vector<uint32_t> Hashes;

    bool Contains(uint32_t hash) const { return loaded_.count(hash) || stream_.count(hash); }
    // The texture (decompressing it on first use) or nullptr.
    const Texture* Get(uint32_t hash);

    // Every TPK of a file. The pack keeps a reference to the file bytes.
    static std::vector<std::unique_ptr<TexturePack>> ReadAll(std::shared_ptr<const std::vector<uint8_t>> buf);

private:
    struct StreamEntry { int offset, size; uint8_t flags; };
    std::shared_ptr<const std::vector<uint8_t>> buf_;
    std::unordered_map<uint32_t, std::unique_ptr<Texture>> loaded_;
    std::unordered_map<uint32_t, StreamEntry> stream_;
};

} // namespace bb
