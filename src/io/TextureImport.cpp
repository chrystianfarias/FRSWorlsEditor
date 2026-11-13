#include "io/TextureImport.h"

#include "core/Log.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>

namespace {

// --- reading an image ------------------------------------------------------
//
// Through the imaging component Windows already has, rather than a PNG decoder
// of our own. It reads PNG, and TGA, JPEG and BMP besides, so the folder can
// hold whatever the person was already working in.
bool LoadImage(const std::wstring& path, u32& width, u32& height,
               std::vector<u32>& bgra) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory))))
        return false;

    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;

    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand,
                                                     &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {

        UINT w = 0, h = 0;
        if (SUCCEEDED(converter->GetSize(&w, &h)) && w && h) {
            bgra.resize((size_t)w * h);
            if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4,
                                                (UINT)(bgra.size() * 4),
                                                (BYTE*)bgra.data()))) {
                width = w;
                height = h;
                ok = true;
            }
        }
    }

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    return ok;
}

// A TGA, read by hand: true colour at 24 or 32 bits, plain or run-length
// encoded, either origin. That covers what the editor writes (32-bit, plain,
// bottom-up) and what image editors save by default.
// The power of two closest to `size`, between 8 and 2048.
u32 NearestPowerOfTwo(u32 size) {
    u32 best = 8;
    for (u32 p = 8; p <= 2048; p *= 2)
        if ((p > size ? p - size : size - p) < (best > size ? best - size : size - best)) best = p;
    return best;
}

// Bilinear, per channel. Shrinking by more than half would want a box filter
// to avoid skipping pixels, but a picture that far from a power of two is rare
// and the nearest one never differs from the source by that much.
std::vector<u32> Resample(const std::vector<u32>& source, u32 width, u32 height,
                          u32 toWidth, u32 toHeight) {
    std::vector<u32> out((size_t)toWidth * toHeight);
    for (u32 y = 0; y < toHeight; ++y) {
        const float sy = std::max(0.0f, (y + 0.5f) * height / toHeight - 0.5f);
        const u32 y0 = std::min((u32)sy, height - 1), y1 = std::min(y0 + 1, height - 1);
        const float fy = sy - (float)y0;
        for (u32 x = 0; x < toWidth; ++x) {
            const float sx = std::max(0.0f, (x + 0.5f) * width / toWidth - 0.5f);
            const u32 x0 = std::min((u32)sx, width - 1), x1 = std::min(x0 + 1, width - 1);
            const float fx = sx - (float)x0;

            const u32 a = source[(size_t)y0 * width + x0], b = source[(size_t)y0 * width + x1];
            const u32 c = source[(size_t)y1 * width + x0], d = source[(size_t)y1 * width + x1];
            u32 pixel = 0;
            for (u32 shift = 0; shift < 32; shift += 8) {
                const float top    = ((a >> shift) & 0xFF) * (1 - fx) + ((b >> shift) & 0xFF) * fx;
                const float bottom = ((c >> shift) & 0xFF) * (1 - fx) + ((d >> shift) & 0xFF) * fx;
                pixel |= (u32)(top * (1 - fy) + bottom * fy + 0.5f) << shift;
            }
            out[(size_t)y * toWidth + x] = pixel;
        }
    }
    return out;
}

bool LoadTga(const std::wstring& path, u32& width, u32& height, std::vector<u32>& bgra) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
    std::vector<u8> data;
    u8 chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) data.insert(data.end(), chunk, chunk + got);
    std::fclose(file);
    if (data.size() < 18) return false;

    const u8 idLength = data[0], colourMapType = data[1], type = data[2];
    const u32 w = data[12] | (data[13] << 8), h = data[14] | (data[15] << 8);
    const u32 bits = data[16];
    const bool topDown = (data[17] & 0x20) != 0;
    if (colourMapType != 0 || (type != 2 && type != 10) || (bits != 24 && bits != 32) || !w || !h)
        return false;

    const size_t bytesPerPixel = bits / 8;
    size_t at = 18 + (size_t)idLength;
    std::vector<u32> pixels((size_t)w * h);
    const auto readPixel = [&](u32& out) {
        if (at + bytesPerPixel > data.size()) return false;
        const u32 b = data[at], g = data[at + 1], r = data[at + 2];
        const u32 a = bytesPerPixel == 4 ? data[at + 3] : 255u;
        out = (a << 24) | (r << 16) | (g << 8) | b;
        at += bytesPerPixel;
        return true;
    };

    size_t written = 0;
    if (type == 2) {
        for (; written < pixels.size(); ++written)
            if (!readPixel(pixels[written])) return false;
    } else {
        while (written < pixels.size()) {
            if (at >= data.size()) return false;
            const u8 header = data[at++];
            const size_t run = (size_t)(header & 0x7F) + 1;
            if (written + run > pixels.size()) return false;
            if (header & 0x80) {
                u32 value;
                if (!readPixel(value)) return false;
                for (size_t k = 0; k < run; ++k) pixels[written++] = value;
            } else {
                for (size_t k = 0; k < run; ++k)
                    if (!readPixel(pixels[written++])) return false;
            }
        }
    }

    // Rows come bottom-up unless the header says otherwise.
    bgra.resize(pixels.size());
    for (u32 y = 0; y < h; ++y) {
        const u32 source = topDown ? y : h - 1 - y;
        std::memcpy(bgra.data() + (size_t)y * w, pixels.data() + (size_t)source * w, (size_t)w * 4);
    }
    width = w;
    height = h;
    return true;
}

// --- compressing it --------------------------------------------------------

u16 Pack565(int r, int g, int b) {
    return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void Unpack565(u16 value, int& r, int& g, int& b) {
    r = ((value >> 11) & 0x1F) * 255 / 31;
    g = ((value >> 5) & 0x3F) * 255 / 63;
    b = (value & 0x1F) * 255 / 31;
}

// One 4x4 block of colour, fitted to the line between the two extremes it
// spans.
//
// This is the simple fit rather than the exhaustive one: take the darkest and
// lightest corner of the block's colour box and put the four palette entries
// along it. It is what the format was designed around and costs nothing to
// compute; the elaborate encoders buy a little quality on gradients, which is
// not what a texture in a folder beside an OBJ is usually doing.
void CompressColourBlock(const u32* block, u8* out, bool allowTransparent) {
    int lowR = 255, lowG = 255, lowB = 255;
    int highR = 0, highG = 0, highB = 0;

    for (int i = 0; i < 16; ++i) {
        const int b = block[i] & 0xFF;
        const int g = (block[i] >> 8) & 0xFF;
        const int r = (block[i] >> 16) & 0xFF;

        lowR = std::min(lowR, r); highR = std::max(highR, r);
        lowG = std::min(lowG, g); highG = std::max(highG, g);
        lowB = std::min(lowB, b); highB = std::max(highB, b);
    }

    u16 c0 = Pack565(highR, highG, highB);
    u16 c1 = Pack565(lowR, lowG, lowB);

    // The four-colour mode is the one with the extra shades, and it is selected
    // by c0 being the greater of the two. A block that needs the transparent
    // slot has to go the other way round.
    if (!allowTransparent && c0 < c1) std::swap(c0, c1);
    if (allowTransparent && c0 > c1) std::swap(c0, c1);

    int r0, g0, b0, r1, g1, b1;
    Unpack565(c0, r0, g0, b0);
    Unpack565(c1, r1, g1, b1);

    int palette[4][3];
    palette[0][0] = r0; palette[0][1] = g0; palette[0][2] = b0;
    palette[1][0] = r1; palette[1][1] = g1; palette[1][2] = b1;

    if (!allowTransparent) {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = (2 * palette[0][i] + palette[1][i]) / 3;
            palette[3][i] = (palette[0][i] + 2 * palette[1][i]) / 3;
        }
    } else {
        for (int i = 0; i < 3; ++i)
            palette[2][i] = (palette[0][i] + palette[1][i]) / 2;
        palette[3][0] = palette[3][1] = palette[3][2] = 0;
    }

    u32 indices = 0;
    for (int i = 0; i < 16; ++i) {
        const int b = block[i] & 0xFF;
        const int g = (block[i] >> 8) & 0xFF;
        const int r = (block[i] >> 16) & 0xFF;
        const int a = (block[i] >> 24) & 0xFF;

        int best = 0;
        if (allowTransparent && a < 128) {
            best = 3;                      // the transparent slot
        } else {
            int bestDistance = 1 << 30;
            const int limit = allowTransparent ? 3 : 4;
            for (int p = 0; p < limit; ++p) {
                const int dr = palette[p][0] - r;
                const int dg = palette[p][1] - g;
                const int db = palette[p][2] - b;
                const int distance = dr * dr + dg * dg + db * db;
                if (distance < bestDistance) { bestDistance = distance; best = p; }
            }
        }
        indices |= (u32)best << (2 * i);
    }

    std::memcpy(out, &c0, 2);
    std::memcpy(out + 2, &c1, 2);
    std::memcpy(out + 4, &indices, 4);
}

// DXT5's alpha half: two endpoints and three bits of interpolation per texel.
void CompressAlphaBlock(const u32* block, u8* out) {
    int low = 255, high = 0;
    for (int i = 0; i < 16; ++i) {
        const int a = (block[i] >> 24) & 0xFF;
        low = std::min(low, a);
        high = std::max(high, a);
    }

    out[0] = (u8)high;
    out[1] = (u8)low;

    u64 bits = 0;
    for (int i = 0; i < 16; ++i) {
        const int a = (block[i] >> 24) & 0xFF;

        int index = 0;
        if (high == low) {
            index = 0;
        } else {
            // Index 0 is the high endpoint, 1 the low, and 2..7 walk between
            // them, which is why this is not a plain proportion.
            const int step = (int)(((high - a) * 7 + (high - low) / 2) / (high - low));
            index = step == 0 ? 0 : (step == 7 ? 1 : step + 1);
        }
        bits |= (u64)index << (3 * i);
    }

    for (int i = 0; i < 6; ++i) out[2 + i] = (u8)((bits >> (8 * i)) & 0xFF);
}

// One level down, by averaging each two-by-two square.
//
// A box filter is what the format's own tooling used and what a block-compressed
// texture can show: the level below already quantises colour to a line through
// each block, so a sharper kernel would be spending precision the encoder then
// throws away.
void Halve(const std::vector<u32>& in, u32 width, u32 height,
           std::vector<u32>& out, u32& outWidth, u32& outHeight) {
    outWidth  = std::max(1u, width / 2);
    outHeight = std::max(1u, height / 2);
    out.assign((size_t)outWidth * outHeight, 0);

    for (u32 y = 0; y < outHeight; ++y) {
        for (u32 x = 0; x < outWidth; ++x) {
            u32 channel[4] = { 0, 0, 0, 0 };
            for (u32 dy = 0; dy < 2; ++dy) {
                for (u32 dx = 0; dx < 2; ++dx) {
                    const u32 sx = std::min(x * 2 + dx, width - 1);
                    const u32 sy = std::min(y * 2 + dy, height - 1);
                    const u32 pixel = in[(size_t)sy * width + sx];
                    for (int c = 0; c < 4; ++c)
                        channel[c] += (pixel >> (8 * c)) & 0xFF;
                }
            }
            u32 pixel = 0;
            for (int c = 0; c < 4; ++c) pixel |= ((channel[c] / 4) & 0xFF) << (8 * c);
            out[(size_t)y * outWidth + x] = pixel;
        }
    }
}

void Compress(const std::vector<u32>& bgra, u32 width, u32 height, bool alpha,
              std::vector<u8>& out) {
    const u32 blocksX = std::max(1u, (width + 3) / 4);
    const u32 blocksY = std::max(1u, (height + 3) / 4);
    const u32 blockBytes = alpha ? 16u : 8u;

    out.assign((size_t)blocksX * blocksY * blockBytes, 0);

    for (u32 by = 0; by < blocksY; ++by) {
        for (u32 bx = 0; bx < blocksX; ++bx) {
            u32 block[16];
            for (u32 y = 0; y < 4; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    // Blocks at the edge of a texture that is not a multiple of
                    // four repeat the last row and column rather than reading
                    // past the image.
                    const u32 px = std::min(bx * 4 + x, width - 1);
                    const u32 py = std::min(by * 4 + y, height - 1);
                    block[y * 4 + x] = bgra[(size_t)py * width + px];
                }
            }

            u8* at = out.data() + ((size_t)by * blocksX + bx) * blockBytes;
            if (alpha) {
                CompressAlphaBlock(block, at);
                CompressColourBlock(block, at + 8, false);
            } else {
                CompressColourBlock(block, at, false);
            }
        }
    }
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t c : text) out.push_back(c < 128 ? (char)c : '_');
    return out;
}

} // namespace

u32 TextureNameHash(const std::string& name) {
    u32 hash = 0xFFFFFFFFu;
    for (unsigned char c : name) hash = hash * 33u + c;
    return hash;
}

bool LoadTextureFolder(const std::wstring& objPath, std::vector<NewTexture>& out,
                       std::wstring& error) {
    // The folder is the OBJ's name without its extension.
    std::wstring folder = objPath;
    const size_t dot = folder.find_last_of(L'.');
    const size_t slash = folder.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        folder = folder.substr(0, dot);

    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return true;   // no folder, no textures

    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring file = found.cFileName;
        const size_t extension = file.find_last_of(L'.');
        if (extension == std::wstring::npos) continue;

        std::wstring suffix = file.substr(extension);
        for (wchar_t& c : suffix) c = (wchar_t)::towlower(c);
        if (suffix != L".png" && suffix != L".tga" && suffix != L".bmp" &&
            suffix != L".jpg" && suffix != L".jpeg")
            continue;

        NewTexture texture;
        if (!LoadTextureFile(folder + L"\\" + file, texture, error)) {
            ::FindClose(search);
            return false;
        }
        out.push_back(std::move(texture));
    } while (::FindNextFileW(search, &found));

    ::FindClose(search);
    return true;
}

bool LoadTextureFile(const std::wstring& path, NewTexture& texture, std::wstring& error) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t extension = file.find_last_of(L'.');
    if (extension == std::wstring::npos) {
        error = file + L" has no extension to tell its format by.";
        return false;
    }
    std::wstring suffix = file.substr(extension);
    for (wchar_t& c : suffix) c = (wchar_t)::towlower(c);

    {
        u32 width = 0, height = 0;
        std::vector<u32> bgra;
        // Windows' imaging component reads PNG, BMP and JPEG but has no TGA
        // decoder, and TGA is what the editor exports the game's textures as.
        const bool read = suffix == L".tga" ? LoadTga(path, width, height, bgra)
                                            : LoadImage(path, width, height, bgra);
        if (!read) {
            error = L"Could not read " + file;
            return false;
        }

        // A picture off the internet is whatever size it is -- 236x236, say --
        // and the block formats and the mip chain both want powers of two, as
        // every one of the game's own textures is. Resampled rather than
        // refused, since nobody picks a wall texture by its pixel count.
        const u32 fitWidth = NearestPowerOfTwo(width), fitHeight = NearestPowerOfTwo(height);
        if (fitWidth != width || fitHeight != height) {
            LOG_INFO("texture %S is %ux%u, resampled to %ux%u", file.c_str(), width, height,
                     fitWidth, fitHeight);
            bgra = Resample(bgra, width, height, fitWidth, fitHeight);
            width = fitWidth;
            height = fitHeight;
        }

        // The game's own textures are all powers of two, and the block formats
        // want multiples of four in any case.
        if (width % 4 || height % 4) {
            error = file + L" is " + std::to_wstring(width) + L"x" +
                    std::to_wstring(height) +
                    L". A block-compressed texture needs both sides to be a "
                    L"multiple of four.";
            return false;
        }

        std::string name = Narrow(file.substr(0, extension));
        for (char& c : name) c = (char)::toupper((unsigned char)c);
        TextureFromPixels(name, bgra, width, height, texture);
        texture.fromTga = suffix == L".tga";
    }
    return true;
}

void TextureFromPixels(const std::string& name, const std::vector<u32>& bgra, u32 width,
                       u32 height, NewTexture& texture) {
    texture = NewTexture{};
    texture.name = name;

    texture.hash   = TextureNameHash(texture.name);
    texture.width  = width;
    texture.height = height;

    // Alpha decides the format. Carrying an alpha channel that is opaque
    // everywhere doubles the size of a texture for nothing.
    for (u32 pixel : bgra)
        if ((pixel >> 24) < 255) { texture.hasAlpha = true; break; }

    texture.fourcc = texture.hasAlpha ? 0x35545844u   // "DXT5"
                                      : 0x31545844u;  // "DXT1"

    // Levels stop at eight pixels a side rather than at one. That is where
    // the shipped packs stop -- a 256x256 track texture has six levels, not
    // eight -- and below it a block-compressed level is mostly padding.
    std::vector<u32> level = bgra;
    u32 levelWidth = width, levelHeight = height, levels = 0;
    for (;;) {
        std::vector<u8> compressed;
        Compress(level, levelWidth, levelHeight, texture.hasAlpha, compressed);

        if (texture.pixels.empty())
            texture.baseLevelSize = (u32)compressed.size();

        texture.pixels.insert(texture.pixels.end(),
                              compressed.begin(), compressed.end());
        ++levels;

        if (levelWidth <= 8 || levelHeight <= 8) break;

        std::vector<u32> smaller;
        u32 nextWidth = 0, nextHeight = 0;
        Halve(level, levelWidth, levelHeight, smaller, nextWidth, nextHeight);
        level = std::move(smaller);
        levelWidth = nextWidth;
        levelHeight = nextHeight;
    }

    texture.mipCount = levels;

    LOG_INFO("texture '%s' read: %ux%u %s, hash %08X, %u mip levels, %zu bytes",
             texture.name.c_str(), width, height,
             texture.hasAlpha ? "DXT5" : "DXT1", texture.hash,
             texture.mipCount, texture.pixels.size());
}
