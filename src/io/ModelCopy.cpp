#include "io/ModelCopy.h"

#include "nfs/ChunkReader.h"
#include "nfs/World.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr u32 kListHeader  = 0x80134001;
constexpr u32 kListInfo    = 0x00134002;
constexpr u32 kListHashes  = 0x00134003;
constexpr size_t kInfoObjectCount = 0x0C;

u32 Read32(const u8* at) { u32 v; std::memcpy(&v, at, 4); return v; }
void Put32(std::vector<u8>& out, size_t at, u32 v) { std::memcpy(out.data() + at, &v, 4); }

// One child of a container, header included, exactly as stored.
struct Raw { u32 id; const u8* at; size_t size; };

std::vector<Raw> Children(const u8* payload, size_t length) {
    std::vector<Raw> out;
    size_t at = 0;
    while (at + 8 <= length) {
        const u32 id = Read32(payload + at), size = Read32(payload + at + 4);
        if (at + 8 + (size_t)size > length) break;
        out.push_back({ id, payload + at, 8 + (size_t)size });
        at += 8 + size;
    }
    return out;
}

// A zero-id chunk that brings the next write to `wanted` modulo 128.
void PadTo(std::vector<u8>& out, u64 base, u64 wanted) {
    const u64 here = base + out.size();
    size_t pad = (size_t)((wanted % 128 + 128 - here % 128) % 128);
    if (pad == 0) return;
    if (pad < 8) pad += 128;
    const size_t at = out.size();
    out.resize(at + pad, 0);
    Put32(out, at + 4, (u32)(pad - 8));
}

} // namespace

bool AppendSolidObject(ByteSpan pack, u64 packOffset, ByteSpan object, u64 objectOffset,
                       u32 hash, std::vector<u8>& out, std::wstring& error) {
    if (pack.size < 8 || Read32(pack.data) != nfs::kChunkSolidListPack ||
        object.size < 8 || Read32(object.data) != nfs::kChunkSolidObject) {
        error = L"The model or the section's object list is not what it should be.";
        return false;
    }

    const std::vector<Raw> children = Children(pack.data + 8, Read32(pack.data + 4));
    out.clear();
    out.resize(8);
    Put32(out, 0, nfs::kChunkSolidListPack);

    bool listed = false, headerDone = false;
    for (size_t k = 0; k < children.size(); ++k) {
        const Raw& child = children[k];
        const u64 originalAt = packOffset + (u64)(child.at - pack.data);

        if (child.id == kListHeader && !listed) {
            // The header, with the count raised and the new hash in its place.
            const size_t headerAt = out.size();
            out.insert(out.end(), child.at, child.at + 8);
            for (const Raw& part : Children(child.at + 8, child.size - 8)) {
                std::vector<u8> bytes(part.at, part.at + part.size);
                if (part.id == kListInfo && bytes.size() >= 8 + kInfoObjectCount + 4) {
                    Put32(bytes, 8 + kInfoObjectCount, Read32(bytes.data() + 8 + kInfoObjectCount) + 1);
                } else if (part.id == kListHashes) {
                    // Pairs of (hash, 0), sorted, possibly after 0x11 filler.
                    size_t first = 8;
                    while (first < bytes.size() && bytes[first] == 0x11) ++first;
                    std::vector<std::pair<u32, u32>> pairs;
                    for (size_t at = first; at + 8 <= bytes.size(); at += 8)
                        pairs.push_back({ Read32(bytes.data() + at), Read32(bytes.data() + at + 4) });
                    const auto place = std::lower_bound(pairs.begin(), pairs.end(),
                                                        std::make_pair(hash, 0u));
                    pairs.insert(place, { hash, 0u });
                    bytes.resize(first);
                    for (const auto& [h, v] : pairs) {
                        const size_t at = bytes.size();
                        bytes.resize(at + 8);
                        Put32(bytes, at, h);
                        Put32(bytes, at + 4, v);
                    }
                    Put32(bytes, 4, (u32)(bytes.size() - 8));
                }
                out.insert(out.end(), bytes.begin(), bytes.end());
            }
            Put32(out, headerAt + 4, (u32)(out.size() - headerAt - 8));
            listed = true;
            headerDone = true;
            continue;
        }

        // The padding after the header is remade: whatever keeps the first
        // object where it was modulo 128.
        if (child.id == 0 && headerDone) continue;

        if (headerDone && child.id != 0) {
            PadTo(out, packOffset, originalAt);
            headerDone = false;
        }
        out.insert(out.end(), child.at, child.at + child.size);
    }
    if (!listed) {
        error = L"The section's object list has no header to list the model in.";
        return false;
    }

    PadTo(out, packOffset, objectOffset);
    out.insert(out.end(), object.data, object.data + object.size);
    Put32(out, 4, (u32)(out.size() - 8));
    return true;
}

bool FindModelChunk(const nfs::World& world, u32 hash, ByteSpan& object, u64& fileOffset,
                    u32* packOrder) {
    const size_t home = world.ModelHome(hash);
    if (home >= world.Loaded().size() || !world.Loaded()[home]->info) return false;
    const nfs::Section& section = *world.Loaded()[home]->info;
    const ByteSpan bytes = world.SectionBytes(section);

    nfs::ChunkReader top(bytes, section.streamOffset);
    nfs::Chunk pack;
    u32 order = 0;
    while (top.Next(pack)) {
        if (pack.id != nfs::kChunkSolidListPack) continue;
        const u32 thisPack = order++;
        nfs::ChunkReader inner = nfs::ChunkReader::Children(pack);
        nfs::Chunk entry;
        while (inner.Next(entry)) {
            if (entry.id != nfs::kChunkSolidObject) continue;
            const nfs::Chunk header = nfs::FindChunk(entry, nfs::kChunkSolidObjectHeader);
            if (header.id == 0 || header.data.read<u32>(0x10) != hash) continue;
            object = ByteSpan{ entry.raw.data - 8, entry.raw.size + 8 };
            fileOffset = entry.headerOffset;
            if (packOrder) *packOrder = thisPack;
            return true;
        }
    }
    return false;
}

bool TextureVisibleTo(const nfs::World& world, u32 hash, size_t targetSection) {
    if (targetSection >= world.Loaded().size()) return false;
    const nfs::LoadedSection& target = *world.Loaded()[targetSection];
    for (const auto& section : world.Loaded()) {
        if (section.get() != &target && !world.IsLibrary(*section)) continue;
        for (const auto& pack : section->texturePacks)
            if (pack->Find(hash)) return true;
    }
    return false;
}

const nfs::Texture* FindGameTextureByFile(const nfs::World& world, const std::string& stem) {
    const auto safe = [](const std::string& name) {
        std::string out;
        for (char c : name) {
            const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9') || c == '_' || c == '-';
            out.push_back(plain ? (char)::toupper((unsigned char)c) : '_');
        }
        return out;
    };
    const std::string wanted = safe(stem);
    for (const nfs::Texture* texture : world.AllTextures())
        if (safe(texture->name) == wanted) return texture;
    // A name the export could not spell back: try it as the game's own name.
    std::string upper = stem;
    for (char& c : upper) c = (char)::toupper((unsigned char)c);
    return world.FindTexture(TextureNameHash(upper));
}

bool CopyGameTexture(const nfs::Texture& source, NewTexture& copy) {
    copy.name   = source.name;
    copy.hash   = source.hash;
    copy.width  = source.width;
    copy.height = source.height;
    switch (source.format) {
        case nfs::TextureFormat::Dxt1: copy.fourcc = 0x31545844u; break;   // "DXT1"
        case nfs::TextureFormat::Dxt3: copy.fourcc = 0x33545844u; break;   // "DXT3"
        case nfs::TextureFormat::Dxt5: copy.fourcc = 0x35545844u; break;   // "DXT5"
        default: return false;
    }
    copy.hasAlpha = source.format != nfs::TextureFormat::Dxt1;
    copy.pixels.assign(source.pixels.data, source.pixels.data + source.pixels.size);
    copy.mipCount = source.mipCount;
    copy.baseLevelSize = source.baseLevelSize;
    return true;
}

std::vector<NewTexture> TexturesToCopy(const nfs::World& world, u32 hash, size_t targetSection,
                                       std::wstring& problem) {
    std::vector<NewTexture> out;
    const nfs::SolidObject* model = world.FindModel(hash);
    if (!model || targetSection >= world.Loaded().size()) return out;
    const nfs::LoadedSection& target = *world.Loaded()[targetSection];

    for (const u32 texture : model->textureHashes) {
        bool visible = false;
        for (const auto& section : world.Loaded()) {
            const bool counts = section.get() == &target || world.IsLibrary(*section);
            if (!counts) continue;
            for (const auto& pack : section->texturePacks)
                if (pack->Find(texture)) visible = true;
        }
        if (visible) continue;

        const nfs::Texture* source = world.FindTexture(texture);
        if (!source) {
            problem += L"A texture of the model is not in the region at all. ";
            continue;
        }
        NewTexture copy;
        copy.name   = source->name;
        copy.hash   = source->hash;
        copy.width  = source->width;
        copy.height = source->height;
        switch (source->format) {
            case nfs::TextureFormat::Dxt1: copy.fourcc = 0x31545844u; break;   // "DXT1"
            case nfs::TextureFormat::Dxt3: copy.fourcc = 0x33545844u; break;   // "DXT3"
            case nfs::TextureFormat::Dxt5: copy.fourcc = 0x35545844u; break;   // "DXT5"
            default:
                problem += L"A texture of the model is palettised, which cannot be copied yet. ";
                continue;
        }
        copy.hasAlpha = source->format != nfs::TextureFormat::Dxt1;
        copy.pixels.assign(source->pixels.data, source->pixels.data + source->pixels.size);
        copy.mipCount = source->mipCount;
        copy.baseLevelSize = source->baseLevelSize;
        if (std::none_of(out.begin(), out.end(), [&](const NewTexture& t) { return t.hash == copy.hash; }))
            out.push_back(std::move(copy));
    }
    return out;
}
