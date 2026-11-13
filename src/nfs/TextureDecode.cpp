#include "nfs/TextureDecode.h"

#include <algorithm>

namespace nfs {

namespace {

u32 Bgra(u32 b, u32 g, u32 r, u32 a) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Expands a 5:6:5 colour to 8 bits per channel.
void Unpack565(u16 value, u32& r, u32& g, u32& b) {
    r = ((value >> 11) & 0x1F) * 255 / 31;
    g = ((value >> 5) & 0x3F) * 255 / 63;
    b = (value & 0x1F) * 255 / 31;
}

// Builds the four-entry palette of one DXT colour block. DXT1 switches to a
// three-colour mode with a transparent slot when the first endpoint is not
// greater than the second; DXT3 and DXT5 always use the four-colour mode.
void ColourPalette(u16 c0, u16 c1, bool allowTransparent, u32 palette[4]) {
    u32 r0, g0, b0, r1, g1, b1;
    Unpack565(c0, r0, g0, b0);
    Unpack565(c1, r1, g1, b1);

    palette[0] = Bgra(b0, g0, r0, 255);
    palette[1] = Bgra(b1, g1, r1, 255);

    if (!allowTransparent || c0 > c1) {
        palette[2] = Bgra((2 * b0 + b1) / 3, (2 * g0 + g1) / 3, (2 * r0 + r1) / 3, 255);
        palette[3] = Bgra((b0 + 2 * b1) / 3, (g0 + 2 * g1) / 3, (r0 + 2 * r1) / 3, 255);
    } else {
        palette[2] = Bgra((b0 + b1) / 2, (g0 + g1) / 2, (r0 + r1) / 2, 255);
        palette[3] = 0;   // transparent black
    }
}

void StoreBlock(std::vector<u32>& out, u32 width, u32 height,
                u32 blockX, u32 blockY, const u32 colours[16]) {
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 px = blockX + x;
            const u32 py = blockY + y;
            if (px < width && py < height)
                out[(size_t)py * width + px] = colours[y * 4 + x];
        }
    }
}

u16 Read16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
u32 Read32(const u8* p) { return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }

bool DecodeDxt(const u8* source, size_t available, TextureFormat format,
               u32 width, u32 height, std::vector<u32>& out) {
    const bool dxt1 = format == TextureFormat::Dxt1;
    const u32 blockBytes = dxt1 ? 8u : 16u;
    const u32 blocksX = std::max(1u, (width + 3) / 4);
    const u32 blocksY = std::max(1u, (height + 3) / 4);

    if (available < (size_t)blocksX * blocksY * blockBytes) return false;

    for (u32 by = 0; by < blocksY; ++by) {
        for (u32 bx = 0; bx < blocksX; ++bx) {
            const u8* block = source + ((size_t)by * blocksX + bx) * blockBytes;
            const u8* colourBlock = dxt1 ? block : block + 8;

            u32 palette[4];
            ColourPalette(Read16(colourBlock), Read16(colourBlock + 2), dxt1, palette);
            const u32 indices = Read32(colourBlock + 4);

            u32 alpha[16];
            if (dxt1) {
                for (u32 i = 0; i < 16; ++i) alpha[i] = 0xFF;
            } else if (format == TextureFormat::Dxt3) {
                // Four bits of explicit alpha per texel.
                for (u32 i = 0; i < 16; ++i) {
                    const u32 nibble = (block[i / 2] >> ((i & 1) * 4)) & 0x0F;
                    alpha[i] = nibble * 255 / 15;
                }
            } else {
                // Two endpoints and three-bit interpolation indices.
                const u32 a0 = block[0], a1 = block[1];
                u32 ramp[8] = { a0, a1 };
                if (a0 > a1) {
                    for (u32 i = 1; i < 7; ++i)
                        ramp[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                } else {
                    for (u32 i = 1; i < 5; ++i)
                        ramp[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                    ramp[6] = 0;
                    ramp[7] = 255;
                }
                u64 bits = 0;
                for (int i = 0; i < 6; ++i) bits |= (u64)block[2 + i] << (8 * i);
                for (u32 i = 0; i < 16; ++i) alpha[i] = ramp[(bits >> (3 * i)) & 7];
            }

            u32 colours[16];
            for (u32 i = 0; i < 16; ++i) {
                const u32 colour = palette[(indices >> (2 * i)) & 3];
                const u32 a = dxt1 ? (colour >> 24) : alpha[i];
                colours[i] = (colour & 0x00FFFFFF) | (a << 24);
            }

            StoreBlock(out, width, height, bx * 4, by * 4, colours);
        }
    }
    return true;
}

bool DecodePalettised(const u8* source, size_t available, ByteSpan palette,
                      u32 width, u32 height, std::vector<u32>& out) {
    if (available < (size_t)width * height || palette.size < 256 * 4) return false;

    for (size_t i = 0; i < (size_t)width * height; ++i) {
        const u8* entry = palette.data + (size_t)source[i] * 4;
        // The palette holds D3DCOLOR values whose fourth byte is a gloss
        // channel, not opacity, so the pixels come out opaque.
        out[i] = Bgra(entry[0], entry[1], entry[2], 255);
    }
    return true;
}

} // namespace

u32 TextureLevelSize(const Texture& texture, u32 level) {
    const u32 width  = std::max(1u, (u32)texture.width >> level);
    const u32 height = std::max(1u, (u32)texture.height >> level);

    switch (texture.format) {
        case TextureFormat::Palettised8:
            return width * height;
        case TextureFormat::Dxt1:
            return std::max(1u, (width + 3) / 4) * std::max(1u, (height + 3) / 4) * 8;
        case TextureFormat::Dxt3:
        case TextureFormat::Dxt5:
            return std::max(1u, (width + 3) / 4) * std::max(1u, (height + 3) / 4) * 16;
        default:
            return 0;
    }
}

bool DecodeTexture(const Texture& texture, u32 level,
                   std::vector<u32>& outPixels, u32& outWidth, u32& outHeight) {
    if (texture.format == TextureFormat::Unknown) return false;
    if (level >= texture.mipCount) return false;

    // Levels are packed largest first.
    size_t offset = 0;
    for (u32 i = 0; i < level; ++i) offset += TextureLevelSize(texture, i);

    outWidth  = std::max(1u, (u32)texture.width >> level);
    outHeight = std::max(1u, (u32)texture.height >> level);
    if (outWidth == 0 || outHeight == 0) return false;

    const u32 size = TextureLevelSize(texture, level);
    if (size == 0 || offset + size > texture.pixels.size) return false;

    outPixels.assign((size_t)outWidth * outHeight, 0);
    const u8* source = texture.pixels.data + offset;
    const size_t available = texture.pixels.size - offset;

    if (texture.format == TextureFormat::Palettised8)
        return DecodePalettised(source, available, texture.palette,
                                outWidth, outHeight, outPixels);

    return DecodeDxt(source, available, texture.format, outWidth, outHeight, outPixels);
}

} // namespace nfs
