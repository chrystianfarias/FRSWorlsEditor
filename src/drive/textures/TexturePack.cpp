#include "drive/textures/TexturePack.h"

#include "drive/cars/CarBin.h"
#include "nfs/EaCompress.h"
#include "nfs/TextureDecode.h"

#include <algorithm>
#include <cstring>

namespace bb {

namespace {

constexpr uint32_t Info = 0xB3310000;
constexpr uint32_t Header = 0x33310001;
constexpr uint32_t Keys = 0x33310002;
constexpr uint32_t StreamEntries = 0x33310003;
constexpr uint32_t TextureInfos = 0x33310004;
constexpr uint32_t PlatInfos = 0x33310005;
constexpr uint32_t Data = 0xB3320000;
constexpr uint32_t VramData = 0x33320002;

template <class T>
T Rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

std::vector<uint8_t> Slice(const std::vector<uint8_t>& b, int off, int len) {
    if (off < 0 || len <= 0 || off >= (int)b.size()) return {};
    len = std::min(len, (int)b.size() - off);
    return std::vector<uint8_t>(b.begin() + off, b.begin() + off + len);
}

} // namespace

int Texture::MipSize(int level) const {
    const int w = std::max(1, Width >> level), h = std::max(1, Height >> level);
    switch (Format) {
        case TextureFormat::Dxt1: return std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 8;
        case TextureFormat::Dxt3:
        case TextureFormat::Dxt5: return std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 16;
        case TextureFormat::Argb32: return w * h * 4;
        case TextureFormat::P8: return w * h;
        default: return 0;
    }
}

bool Texture::ToArgb32(int level, std::vector<uint32_t>& out, int& width, int& height) const {
    width = std::max(1, Width >> level);
    height = std::max(1, Height >> level);
    int offset = 0;
    for (int i = 0; i < level; i++) offset += MipSize(i);
    const int size = MipSize(level);
    if (size == 0 || offset + size > (int)Image.size()) return false;
    out.assign((size_t)width * height, 0);
    switch (Format) {
        case TextureFormat::Dxt1:
        case TextureFormat::Dxt3:
        case TextureFormat::Dxt5: {
            nfs::Texture t;
            t.width = (u16)Width;
            t.height = (u16)Height;
            t.mipCount = (u8)std::max(1, NumMipMapLevels);
            t.format = Format == TextureFormat::Dxt1   ? nfs::TextureFormat::Dxt1
                       : Format == TextureFormat::Dxt3 ? nfs::TextureFormat::Dxt3
                                                       : nfs::TextureFormat::Dxt5;
            t.pixels = { Image.data(), Image.size() };
            u32 w = 0, h = 0;
            std::vector<u32> px;
            if (!nfs::DecodeTexture(t, (u32)level, px, w, h) || (int)w != width || (int)h != height) return false;
            out.assign(px.begin(), px.end());
            return true;
        }
        case TextureFormat::Argb32:
            // D3DFMT_A8R8G8B8 is stored as B,G,R,A bytes = a little-endian D3DCOLOR.
            std::memcpy(out.data(), Image.data() + offset, (size_t)size);
            return true;
        case TextureFormat::P8:
            // Palette entries as PALETTEENTRY (R,G,B,A), as the C# reads them.
            if (Palette.size() < 256 * 4) return false;
            for (int i = 0; i < width * height; i++) {
                const int p = Image[(size_t)offset + i] * 4;
                out[(size_t)i] = (uint32_t)Palette[p + 3] << 24 | (uint32_t)Palette[p] << 16 |
                                 (uint32_t)Palette[p + 1] << 8 | Palette[p + 2];
            }
            return true;
        default:
            return false;
    }
}

Texture Texture::FromInfo(const uint8_t* info) {
    Texture t;
    const char* name = reinterpret_cast<const char*>(info + 0x0C);
    t.Name.assign(name, strnlen(name, 24));
    t.NameHash = Rd<uint32_t>(info + 0x24);
    t.ClassNameHash = Rd<uint32_t>(info + 0x28);
    t.Width = Rd<int16_t>(info + 0x44);
    t.Height = Rd<int16_t>(info + 0x46);
    t.Format = (TextureFormat)info[0x4A];
    t.NumPaletteEntries = Rd<int16_t>(info + 0x4C);
    t.NumMipMapLevels = std::max(1, (int)(int8_t)info[0x4E]);
    t.RenderingOrder = (int8_t)info[0x51];
    t.ScrollType = (int8_t)info[0x52];
    t.ApplyAlphaSorting = info[0x54] != 0;
    t.AlphaUsage_ = (AlphaUsage)(int8_t)info[0x55];
    t.AlphaBlend_ = (AlphaBlend)(int8_t)info[0x56];
    t.Flags = info[0x57];
    return t;
}

const Texture* TexturePack::Get(uint32_t hash) {
    const auto found = loaded_.find(hash);
    if (found != loaded_.end()) return found->second.get();
    const auto e = stream_.find(hash);
    if (e == stream_.end()) return nullptr;
    const std::vector<uint8_t>& buf = *buf_;
    // Blob layout once decompressed: [image][palette?][TextureInfo 0x7C][PlatInfo 0x20]
    std::vector<uint8_t> raw;
    if (e->second.flags & 1) {
        if (e->second.offset < 0 || e->second.offset + e->second.size > (int)buf.size()) return nullptr;
        if (!nfs::EaDecompress({ buf.data() + e->second.offset, (size_t)e->second.size }, raw)) return nullptr;
    } else {
        raw = Slice(buf, e->second.offset, e->second.size);
    }
    if (raw.size() < 0x9C) return nullptr;
    const int infoOff = (int)raw.size() - 0x9C;
    auto t = std::make_unique<Texture>(Texture::FromInfo(raw.data() + infoOff));
    t->D3DFormat = Rd<uint32_t>(raw.data() + raw.size() - 0x20 + 0x14);
    const int image = Rd<int32_t>(raw.data() + infoOff + 0x30), palette = Rd<int32_t>(raw.data() + infoOff + 0x34);
    const int imageSize = Rd<int32_t>(raw.data() + infoOff + 0x38), paletteSize = Rd<int32_t>(raw.data() + infoOff + 0x3C);
    // Placements are global (accumulated over the pack); inside the blob the first of image/palette starts at 0.
    const int basePos = paletteSize > 0 ? std::min(image, palette) : image;
    t->Image = Slice(raw, image - basePos, imageSize);
    if (paletteSize > 0) t->Palette = Slice(raw, palette - basePos, paletteSize);
    if (t->NameHash != hash) return nullptr;
    const Texture* result = t.get();
    loaded_[hash] = std::move(t);
    return result;
}

std::vector<std::unique_ptr<TexturePack>> TexturePack::ReadAll(std::shared_ptr<const std::vector<uint8_t>> shared) {
    std::vector<std::unique_ptr<TexturePack>> packs;
    const std::vector<uint8_t>& buf = *shared;
    carbin::Walk(carbin::Parse(buf), [&](const carbin::Chunk& root) {
        if (root.Id != Root) return;
        const carbin::Chunk* info = root.Find(Info);
        const carbin::Chunk* data = root.Find(Data);
        if (!info || !data) return;
        auto p = std::make_unique<TexturePack>();
        p->buf_ = shared;
        try {
            if (const carbin::Chunk* h = info->Find(Header)) {
                carbin::Reader hr = carbin::ChunkReader(buf, *h);
                hr.Skip(4);
                p->Name = hr.FixedString(0x1C);
            }
            if (const carbin::Chunk* k = info->Find(Keys)) {
                carbin::Reader kr = carbin::ChunkReader(buf, *k);
                for (int i = 0; i < k->Size / 8; i++) {
                    p->Hashes.push_back(kr.U32());
                    kr.Skip(4);
                }
            }
            if (const carbin::Chunk* se = info->Find(StreamEntries)) {
                // eStreamingEntry (0x18): hash, offset (relative to the TPK root), size, uncompressed size,
                // u8 user flags, u8 flags (bit0 = compressed), u16 refcount, ptr
                p->Streaming = true;
                carbin::Reader r = carbin::ChunkReader(buf, *se);
                for (int i = 0; i < se->Size / 0x18; i++) {
                    const uint32_t hash = r.U32();
                    const int off = r.S32();
                    const int size = r.S32();
                    r.Skip(5);
                    const uint8_t flags = r.U8();
                    r.Skip(6);
                    p->stream_[hash] = { root.Offset + off, size, flags };
                }
            } else {
                const carbin::Chunk* ti = info->Find(TextureInfos);
                const carbin::Chunk* pi = info->Find(PlatInfos);
                const carbin::Chunk* vd = data->Find(VramData);
                if (!ti || !vd) return;
                const int dataStart = (vd->DataOffset() + 0x7F) & ~0x7F;
                const int n = ti->Size / 0x7C;
                for (int i = 0; i < n; i++) {
                    const int io = ti->DataOffset() + i * 0x7C;
                    auto t = std::make_unique<Texture>(Texture::FromInfo(buf.data() + io));
                    const int image = Rd<int32_t>(buf.data() + io + 0x30), palette = Rd<int32_t>(buf.data() + io + 0x34);
                    const int imageSize = Rd<int32_t>(buf.data() + io + 0x38);
                    const int paletteSize = Rd<int32_t>(buf.data() + io + 0x3C);
                    t->Image = Slice(buf, dataStart + image, imageSize);
                    if (paletteSize > 0) t->Palette = Slice(buf, dataStart + palette, paletteSize);
                    if (pi && pi->DataOffset() + i * 0x20 + 0x18 <= pi->End())
                        t->D3DFormat = Rd<uint32_t>(buf.data() + pi->DataOffset() + i * 0x20 + 0x14);
                    const uint32_t h = t->NameHash;
                    p->loaded_[h] = std::move(t);
                }
            }
        } catch (const std::exception&) {
            return;
        }
        packs.push_back(std::move(p));
    });
    return packs;
}

} // namespace bb
