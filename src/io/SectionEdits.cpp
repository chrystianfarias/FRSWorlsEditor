#include "io/SectionEdits.h"

#include "core/Log.h"
#include "io/ModelCopy.h"
#include "io/SectionRebuild.h"
#include "nfs/ChunkReader.h"
#include "nfs/Collision.h"
#include "nfs/TexturePack.h"
#include "nfs/World.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace {

u32 Read32(const u8* at) { u32 v; std::memcpy(&v, at, 4); return v; }

struct TopChunk {
    u32       id = 0;
    u32       order = 0;        // among the section's chunks with this id
    size_t    start = 0, end = 0;
    ByteSpan  whole;
    u64       fileOffset = 0;
    nfs::Chunk chunk;
};

// The section's top-level chunks, zero-id padding left out, each numbered
// among those of its own kind.
std::vector<TopChunk> TopChunks(ByteSpan bytes, u64 fileOffset) {
    std::vector<TopChunk> out;
    std::map<u32, u32> seen;
    nfs::ChunkReader reader(bytes, fileOffset);
    nfs::Chunk chunk;
    while (reader.Next(chunk)) {
        if (chunk.id == 0) continue;
        TopChunk top;
        top.id = chunk.id;
        top.order = seen[chunk.id]++;
        top.start = (size_t)(chunk.headerOffset - fileOffset);
        top.end = top.start + 8 + chunk.raw.size;
        top.whole = ByteSpan{ chunk.raw.data - 8, chunk.raw.size + 8 };
        top.fileOffset = chunk.headerOffset;
        top.chunk = chunk;
        out.push_back(top);
    }
    return out;
}

bool Same(ByteSpan a, ByteSpan b) {
    return a.size == b.size && std::memcmp(a.data, b.data, a.size) == 0;
}

struct ObjectAt {
    u32         hash = 0;
    std::string name;
    ByteSpan    whole;
    u64         fileOffset = 0;
};

std::vector<ObjectAt> Objects(const nfs::Chunk& pack) {
    std::vector<ObjectAt> out;
    nfs::ChunkReader reader = nfs::ChunkReader::Children(pack);
    nfs::Chunk entry;
    while (reader.Next(entry)) {
        if (entry.id != nfs::kChunkSolidObject) continue;
        const nfs::Chunk header = nfs::FindChunk(entry, nfs::kChunkSolidObjectHeader);
        if (header.id == 0) continue;
        ObjectAt object;
        object.hash = header.data.read<u32>(0x10);
        if (header.data.size > 0xA4)
            object.name.assign((const char*)header.data.data + 0xA4,
                               ::strnlen((const char*)header.data.data + 0xA4, header.data.size - 0xA4));
        object.whole = ByteSpan{ entry.raw.data - 8, entry.raw.size + 8 };
        object.fileOffset = entry.headerOffset;
        out.push_back(object);
    }
    return out;
}

bool ToNewTexture(const nfs::Texture& source, NewTexture& copy) {
    copy.name   = source.name;
    copy.hash   = source.hash;
    copy.width  = source.width;
    copy.height = source.height;
    switch (source.format) {
        case nfs::TextureFormat::Dxt1: copy.fourcc = 0x31545844u; break;
        case nfs::TextureFormat::Dxt3: copy.fourcc = 0x33545844u; break;
        case nfs::TextureFormat::Dxt5: copy.fourcc = 0x35545844u; break;
        default: return false;
    }
    copy.hasAlpha = source.format != nfs::TextureFormat::Dxt1;
    copy.pixels.assign(source.pixels.data, source.pixels.data + source.pixels.size);
    copy.mipCount = source.mipCount;
    copy.baseLevelSize = source.baseLevelSize;
    return true;
}

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

// A whole chunk -- header, 0x11 filler, payload -- for one that will start at
// `fileOffset`. The emitters carry the default 16-byte alignment, so the filler
// depends on where the chunk lands, and an edit keeps only the payload.
std::vector<u8> WrapChunk(u32 id, const std::vector<u8>& payload, u64 fileOffset) {
    const size_t filler = (size_t)((16 - (fileOffset + 8) % 16) % 16);
    std::vector<u8> out(8 + filler, 0x11);
    const u32 size = (u32)(filler + payload.size());
    std::memcpy(out.data(), &id, 4);
    std::memcpy(out.data() + 4, &size, 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

SectionEdit EmittersEdit(const TopChunk& top) {
    SectionEdit edit;
    edit.kind = SectionEdit::Kind::Emitters;
    edit.label = "effects";
    edit.order = top.order;
    edit.record.assign(top.chunk.data.data, top.chunk.data.data + top.chunk.data.size);
    u32 count = 0;
    if (top.chunk.data.size >= 16) count = top.chunk.data.read<u32>(12);
    wchar_t text[64];
    ::swprintf_s(text, L"%u placed effects", count);
    edit.detail = text;
    return edit;
}

// The object list with some objects swapped for edited versions. Each goes on
// the position modulo 128 it was laid out for; everything else keeps its
// shipped one. Padding is remade rather than copied.
bool ReplaceObjects(ByteSpan pack, u64 packOffset, const std::vector<const SectionEdit*>& meshes,
                    std::vector<u8>& out) {
    out.clear();
    out.resize(8);
    const u32 id = nfs::kChunkSolidListPack;
    std::memcpy(out.data(), &id, 4);

    const size_t length = Read32(pack.data + 4);
    size_t at = 8;
    while (at + 8 <= 8 + length) {
        const u32 childId = Read32(pack.data + at), size = Read32(pack.data + at + 4);
        if (at + 8 + (size_t)size > 8 + length) break;
        const u8* child = pack.data + at;
        const size_t total = 8 + (size_t)size;
        at += total;
        if (childId == 0) continue;

        const SectionEdit* swap = nullptr;
        if (childId == nfs::kChunkSolidObject) {
            // The object's hash is in its header, which may sit after filler.
            nfs::Chunk wrapper;
            wrapper.id = childId;
            wrapper.headerOffset = packOffset + (u64)(child - pack.data);
            wrapper.raw = ByteSpan{ child + 8, size };
            wrapper.data = wrapper.raw;
            const nfs::Chunk header = nfs::FindChunk(wrapper, nfs::kChunkSolidObjectHeader);
            const u32 hash = header.id ? header.data.read<u32>(0x10) : 0;
            for (const SectionEdit* mesh : meshes)
                if (mesh->hash == hash) swap = mesh;
        }

        if (swap) {
            PadToModulo(out, packOffset, swap->mod128);
            out.insert(out.end(), swap->chunk.begin(), swap->chunk.end());
        } else {
            PadToModulo(out, packOffset, packOffset + (u64)(child - pack.data));
            out.insert(out.end(), child, child + total);
        }
    }
    const u32 payload = (u32)(out.size() - 8);
    std::memcpy(out.data() + 4, &payload, 4);
    return true;
}

// The bounds in an object chunk's header, for a chunk held in an edit.
bool ChunkBounds(const SectionEdit& edit, Aabb& out) {
    if (edit.chunk.size() < 16) return false;
    nfs::Chunk wrapper;
    wrapper.id = nfs::kChunkSolidObject;
    wrapper.headerOffset = edit.mod128;
    wrapper.dataOffset = edit.mod128 + 8;
    wrapper.raw = ByteSpan{ edit.chunk.data() + 8, edit.chunk.size() - 8 };
    wrapper.data = wrapper.raw;
    const nfs::Chunk header = nfs::FindChunk(wrapper, nfs::kChunkSolidObjectHeader);
    if (!header.id || header.data.size < 0x3C) return false;
    Aabb box;
    std::memcpy(&box.min, header.data.data + 0x20, 12);
    std::memcpy(&box.max, header.data.data + 0x30, 12);
    if (!box.Valid()) return false;
    out = box;
    return true;
}

} // namespace

void PadToModulo(std::vector<u8>& out, u64 base, u64 wanted) {
    const u64 here = base + out.size();
    size_t pad = (size_t)((wanted % 128 + 128 - here % 128) % 128);
    if (pad == 0) return;
    if (pad < 8) pad += 128;
    const size_t at = out.size();
    out.resize(at + pad, 0);
    const u32 size = (u32)(pad - 8);
    std::memcpy(out.data() + at + 4, &size, 4);
}

const wchar_t* EditKindName(SectionEdit::Kind kind) {
    switch (kind) {
        case SectionEdit::Kind::MeshChanged:    return L"Mesh changed";
        case SectionEdit::Kind::ModelAdded:     return L"Model added";
        case SectionEdit::Kind::TextureAdded:   return L"Texture added";
        case SectionEdit::Kind::TextureChanged: return L"Texture changed";
        case SectionEdit::Kind::Collision:      return L"Collision edited";
        case SectionEdit::Kind::Placement:      return L"Object placed";
        case SectionEdit::Kind::NativeDeleted:  return L"Object deleted";
        case SectionEdit::Kind::NativeChanged:  return L"Object changed";
        case SectionEdit::Kind::Emitters:       return L"Effects";
        default:                                return L"Other change";
    }
}

bool DiffSection(const nfs::World& world, const nfs::Section& section,
                 std::vector<SectionEdit>& out, std::wstring& error) {
    return DiffSectionBytes(world, section, world.SectionBytes(section), section.streamOffset,
                            out, error);
}

bool DiffSectionBytes(const nfs::World& world, const nfs::Section& section, ByteSpan current,
                      u64 currentOffset, std::vector<SectionEdit>& out, std::wstring& error) {
    const ByteSpan stream = world.StreamBytes();
    if (section.originalStreamOffset + (u64)section.originalStreamSize > stream.size) {
        error = L"The shipped section is not in the stream file.";
        return false;
    }
    const ByteSpan original = stream.sub(section.originalStreamOffset, section.originalStreamSize);
    if (current.data == original.data && current.size == original.size) return true;

    const auto shipped = TopChunks(original, section.originalStreamOffset);
    const auto now = TopChunks(current, currentOffset);

    const auto find = [](const std::vector<TopChunk>& list, u32 id, u32 order) -> const TopChunk* {
        for (const TopChunk& top : list)
            if (top.id == id && top.order == order) return &top;
        return nullptr;
    };

    for (const TopChunk& b : now) {
        const TopChunk* a = find(shipped, b.id, b.order);
        if (!a && b.id == nfs::kChunkEmitters) {
            out.push_back(EmittersEdit(b));
            continue;
        }
        if (!a) {
            SectionEdit other;
            wchar_t text[64];
            ::swprintf_s(text, L"a chunk %08X the section did not ship with", b.id);
            other.detail = text;
            out.push_back(std::move(other));
            continue;
        }
        if (Same(a->whole, b.whole)) continue;

        switch (b.id) {
            case nfs::kChunkSolidListPack: {
                const auto before = Objects(a->chunk);
                for (const ObjectAt& object : Objects(b.chunk)) {
                    const auto was = std::find_if(before.begin(), before.end(),
                                                  [&](const ObjectAt& o) { return o.hash == object.hash; });
                    if (was != before.end() && Same(was->whole, object.whole)) continue;
                    SectionEdit edit;
                    edit.kind = was == before.end() ? SectionEdit::Kind::ModelAdded
                                                    : SectionEdit::Kind::MeshChanged;
                    edit.label = object.name;
                    edit.hash = object.hash;
                    edit.order = b.order;
                    edit.chunk.assign(object.whole.data, object.whole.data + object.whole.size);
                    edit.mod128 = (u32)(object.fileOffset % 128);
                    wchar_t text[96];
                    ::swprintf_s(text, L"%zu bytes%s", object.whole.size,
                                 was == before.end() ? L"" : L", was different");
                    edit.detail = text;
                    out.push_back(std::move(edit));
                }
                break;
            }
            case nfs::kChunkTexturePack: {
                nfs::TexturePack was, is;
                was.Parse(a->chunk);
                is.Parse(b.chunk);
                for (const nfs::Texture& texture : is.Textures()) {
                    const nfs::Texture* before = was.Find(texture.hash);
                    if (before && before->pixels.size == texture.pixels.size &&
                        std::memcmp(before->pixels.data, texture.pixels.data, texture.pixels.size) == 0 &&
                        before->width == texture.width && before->height == texture.height)
                        continue;
                    SectionEdit edit;
                    edit.kind = before ? SectionEdit::Kind::TextureChanged : SectionEdit::Kind::TextureAdded;
                    edit.label = texture.name;
                    edit.hash = texture.hash;
                    edit.order = b.order;
                    if (!ToNewTexture(texture, edit.texture)) edit.kind = SectionEdit::Kind::Other;
                    wchar_t text[96];
                    ::swprintf_s(text, L"%ux%u, %u mips", texture.width, texture.height, texture.mipCount);
                    edit.detail = text;
                    out.push_back(std::move(edit));
                }
                break;
            }
            case nfs::kChunkCollisionPack: {
                SectionEdit edit;
                edit.kind = SectionEdit::Kind::Collision;
                edit.label = "collision";
                edit.order = b.order;
                edit.chunk.assign(b.whole.data, b.whole.data + b.whole.size);
                edit.mod128 = (u32)(b.fileOffset % 128);
                nfs::CollisionPack was, is;
                nfs::ParseCollision(a->chunk, was);
                nfs::ParseCollision(b.chunk, is);
                wchar_t text[96];
                ::swprintf_s(text, L"%zu polygons, shipped with %zu", is.polygons.size(), was.polygons.size());
                edit.detail = text;
                out.push_back(std::move(edit));
                break;
            }
            case nfs::kChunkTrackScenery: {
                nfs::SceneryChunkInfo info;
                nfs::ReadSceneryChunk(b.whole, b.fileOffset, info);
                std::vector<std::vector<u8>> before, after;
                nfs::ReadSceneryRecords(a->whole, a->fileOffset, before);
                nfs::ReadSceneryRecords(b.whole, b.fileOffset, after);
                if (after.size() < before.size()) {
                    SectionEdit other;
                    other.detail = L"scenery records were removed";
                    out.push_back(std::move(other));
                    break;
                }
                for (size_t i = 0; i < before.size(); ++i) {
                    if (before[i] == after[i]) continue;
                    SectionEdit edit;
                    edit.kind = nfs::IsDeletedRecord(after[i]) ? SectionEdit::Kind::NativeDeleted
                                                               : SectionEdit::Kind::NativeChanged;
                    edit.label = nfs::SceneryRecordModel(a->whole, a->fileOffset, i);
                    edit.sceneryNumber = info.number;
                    edit.index = (u32)i;
                    edit.record = after[i];
                    float t[3];
                    std::memcpy(t, before[i].data() + 0x20, 12);
                    wchar_t text[96];
                    ::swprintf_s(text, L"at %.1f, %.1f, %.1f", t[0], t[1], t[2]);
                    edit.detail = text;
                    out.push_back(std::move(edit));
                }
                std::vector<nfs::PlacedInstance> placed;
                nfs::ReadSceneryPlacements(b.whole, b.fileOffset, before.size(), placed);
                for (size_t k = 0; k < placed.size(); ++k) {
                    SectionEdit edit;
                    edit.kind = SectionEdit::Kind::Placement;
                    edit.label = placed[k].modelName;
                    edit.sceneryNumber = info.number;
                    edit.index = (u32)(before.size() + k);
                    edit.placement = placed[k];
                    const Mat4& m = placed[k].transform;
                    wchar_t text[96];
                    ::swprintf_s(text, L"at %.1f, %.1f, %.1f", m.m[3][0], m.m[3][1], m.m[3][2]);
                    edit.detail = text;
                    out.push_back(std::move(edit));
                }
                break;
            }
            case nfs::kChunkEmitters:
                out.push_back(EmittersEdit(b));
                break;
            default: {
                SectionEdit other;
                wchar_t text[64];
                ::swprintf_s(text, L"chunk %08X differs", b.id);
                other.detail = text;
                out.push_back(std::move(other));
                break;
            }
        }
    }
    return true;
}

bool BuildSection(const nfs::World& world, const nfs::Section& section,
                  const std::vector<SectionEdit>& edits, std::vector<u8>& out,
                  std::wstring& error) {
    const ByteSpan stream = world.StreamBytes();
    if (section.originalStreamOffset + (u64)section.originalStreamSize > stream.size) {
        error = L"The shipped section is not in the stream file.";
        return false;
    }
    const ByteSpan original = stream.sub(section.originalStreamOffset, section.originalStreamSize);
    const auto shipped = TopChunks(original, section.originalStreamOffset);

    for (const SectionEdit& edit : edits)
        if (edit.kind == SectionEdit::Kind::Other) {
            error = L"The section holds a change the editor cannot rebuild: " + edit.detail;
            return false;
        }

    std::vector<ChunkEdit> splices;
    for (const TopChunk& top : shipped) {
        ChunkEdit splice;
        splice.start = top.start;
        splice.end = top.end;

        if (top.id == nfs::kChunkSolidListPack) {
            std::vector<const SectionEdit*> meshes, added;
            for (const SectionEdit& edit : edits) {
                if (edit.order != top.order) continue;
                if (edit.kind == SectionEdit::Kind::MeshChanged) meshes.push_back(&edit);
                if (edit.kind == SectionEdit::Kind::ModelAdded) added.push_back(&edit);
            }
            if (meshes.empty() && added.empty()) continue;

            std::vector<u8> pack(top.whole.data, top.whole.data + top.whole.size);
            if (!meshes.empty()) ReplaceObjects(top.whole, top.fileOffset, meshes, pack);
            for (const SectionEdit* model : added) {
                std::vector<u8> grown;
                if (!AppendSolidObject(ByteSpan{ pack.data(), pack.size() }, top.fileOffset,
                                       ByteSpan{ model->chunk.data(), model->chunk.size() },
                                       model->mod128, model->hash, grown, error))
                    return false;
                pack = std::move(grown);
            }
            splice.bytes = std::move(pack);
        } else if (top.id == nfs::kChunkTexturePack) {
            std::vector<NewTexture> textures;
            for (const SectionEdit& edit : edits)
                if (edit.order == top.order && (edit.kind == SectionEdit::Kind::TextureAdded ||
                                                edit.kind == SectionEdit::Kind::TextureChanged))
                    textures.push_back(edit.texture);
            if (textures.empty()) continue;
            if (!RebuildTexturePackChunk(top.whole, top.fileOffset, textures, splice.bytes, error))
                return false;
        } else if (top.id == nfs::kChunkCollisionPack) {
            const SectionEdit* collision = nullptr;
            for (const SectionEdit& edit : edits)
                if (edit.kind == SectionEdit::Kind::Collision && edit.order == top.order) collision = &edit;
            if (!collision) continue;
            splice.bytes = collision->chunk;
        } else if (top.id == nfs::kChunkTrackScenery) {
            nfs::SceneryChunkInfo info;
            nfs::ReadSceneryChunk(top.whole, top.fileOffset, info);
            std::vector<nfs::PlacedInstance> placed;
            std::vector<nfs::ReplacedRecord> replaced;
            for (const SectionEdit& edit : edits) {
                if (edit.sceneryNumber != info.number) continue;
                if (edit.kind == SectionEdit::Kind::Placement) {
                    nfs::PlacedInstance p = edit.placement;
                    // The model as this rebuild writes it, when it is one of
                    // the edits: the editor's loaded copy is the shape before.
                    const auto own = std::find_if(edits.begin(), edits.end(), [&](const SectionEdit& e) {
                        return (e.kind == SectionEdit::Kind::ModelAdded ||
                                e.kind == SectionEdit::Kind::MeshChanged) && e.hash == p.modelHash;
                    });
                    Aabb written;
                    if (own != edits.end() && ChunkBounds(*own, written)) p.modelBounds = written;
                    else if (const nfs::SolidObject* model = world.FindModel(p.modelHash)) p.modelBounds = model->bounds;
                    if (!p.modelBounds.Valid()) {
                        p.modelBounds.Add({ -0.5f, -0.5f, 0.0f });
                        p.modelBounds.Add({ 0.5f, 0.5f, 1.0f });
                    }
                    placed.push_back(std::move(p));
                } else if (edit.kind == SectionEdit::Kind::NativeDeleted ||
                           edit.kind == SectionEdit::Kind::NativeChanged) {
                    replaced.push_back({ edit.index, edit.record });
                }
            }
            if (placed.empty() && replaced.empty()) continue;
            std::string why;
            if (!nfs::AppendScenery(top.whole, top.fileOffset, placed, splice.bytes, error, replaced))
                return false;
            if (!nfs::ValidateScenery(ByteSpan{ splice.bytes.data(), splice.bytes.size() },
                                      top.fileOffset, why)) {
                error = L"The rebuilt scenery would not be valid: " + Widen(why);
                return false;
            }
        } else if (top.id == nfs::kChunkEmitters) {
            const SectionEdit* effects = nullptr;
            for (const SectionEdit& edit : edits)
                if (edit.kind == SectionEdit::Kind::Emitters && edit.order == top.order) effects = &edit;
            if (!effects) continue;
            splice.bytes = WrapChunk(nfs::kChunkEmitters, effects->record, top.fileOffset);
        } else {
            continue;
        }
        splices.push_back(std::move(splice));
    }

    // Effects for a section that shipped with none: a chunk of their own, where
    // the sections that have one keep it -- just before 0x80036000, or last.
    const bool shippedEffects = std::any_of(shipped.begin(), shipped.end(),
                                            [](const TopChunk& t) { return t.id == nfs::kChunkEmitters; });
    if (!shippedEffects) {
        for (const SectionEdit& edit : edits) {
            if (edit.kind != SectionEdit::Kind::Emitters) continue;
            size_t at = shipped.empty() ? 0 : shipped.back().end;
            for (const TopChunk& top : shipped)
                if (top.id == nfs::kChunkAfterEmitters) { at = top.start; break; }
            ChunkEdit insert;
            insert.start = insert.end = at;
            insert.bytes = WrapChunk(nfs::kChunkEmitters, edit.record,
                                     section.originalStreamOffset + at);
            splices.push_back(std::move(insert));
            break;
        }
    }

    out = SpliceSection(original, std::move(splices));
    return true;
}

bool ShippedChunkData(const nfs::World& world, const nfs::Section& section, u32 id,
                      std::vector<u8>& data) {
    const ByteSpan stream = world.StreamBytes();
    if (section.originalStreamOffset + (u64)section.originalStreamSize > stream.size) return false;
    const ByteSpan original = stream.sub(section.originalStreamOffset, section.originalStreamSize);
    for (const TopChunk& top : TopChunks(original, section.originalStreamOffset)) {
        if (top.id != id) continue;
        data.assign(top.chunk.data.data, top.chunk.data.data + top.chunk.data.size);
        return true;
    }
    return false;
}

bool PlanSectionFromEdits(const nfs::World& world, const nfs::Section& section,
                          const std::vector<SectionEdit>& edits,
                          const std::vector<SpanOverlayInfo>& existing,
                          SectionSpanPlan& out, std::wstring& error) {
    if (edits.empty()) {
        // Back to what the game shipped: the section's spans simply go.
        for (const SpanOverlayInfo& span : existing) {
            const bool sameSection =
                span.layoutFile
                    ? span.fileOffset < section.tableEntryOffset + 80 &&
                      span.fileOffset + span.byteCount > section.tableEntryOffset
                    : span.fileOffset == section.streamOffset;
            if (sameSection) out.superseded.push_back(span.path);
        }
        out.offset = section.originalStreamOffset;
        out.size = section.originalStreamSize;
        return true;
    }

    std::vector<u8> bytes;
    if (!BuildSection(world, section, edits, bytes, error)) return false;
    PlanSectionSpans(world, section, std::move(bytes), existing, "edits", out);
    return true;
}

void SelfTestSectionEdits(const nfs::World& world) {
    const ByteSpan stream = world.StreamBytes();
    for (const auto& loaded : world.Loaded()) {
        if (!loaded->info) continue;
        const nfs::Section& section = *loaded->info;
        const ByteSpan current = world.SectionBytes(section);
        const bool edited = current.data < stream.data || current.data >= stream.data + stream.size;
        if (!edited) continue;

        std::vector<SectionEdit> edits;
        std::wstring error;
        if (!DiffSection(world, section, edits, error)) {
            LOG_WARN("edits self-test %s: diff failed: %S", section.name.c_str(), error.c_str());
            continue;
        }

        // All of them: the rebuilt section has to carry the same edits.
        std::vector<u8> all;
        std::vector<SectionEdit> again;
        bool same = BuildSection(world, section, edits, all, error) &&
                    DiffSectionBytes(world, section, ByteSpan{ all.data(), all.size() },
                                     section.originalStreamOffset, again, error) &&
                    again.size() == edits.size();
        for (size_t i = 0; same && i < edits.size(); ++i) {
            const SectionEdit& a = edits[i];
            const SectionEdit& b = again[i];
            same = a.kind == b.kind && a.hash == b.hash && a.index == b.index &&
                   a.chunk == b.chunk && a.record == b.record &&
                   a.texture.pixels == b.texture.pixels &&
                   std::memcmp(&a.placement.transform, &b.placement.transform, sizeof(Mat4)) == 0;
        }

        // None of them: the shipped bytes, exactly.
        std::vector<u8> none;
        const ByteSpan original = stream.sub(section.originalStreamOffset, section.originalStreamSize);
        const bool shipped = BuildSection(world, section, {}, none, error) &&
                             none.size() == original.size &&
                             std::memcmp(none.data(), original.data, original.size) == 0;

        // Each one left out in turn: the rest have to come back, and only them.
        size_t leaveOneOut = 0;
        for (size_t skip = 0; skip < edits.size(); ++skip) {
            std::vector<SectionEdit> rest;
            for (size_t i = 0; i < edits.size(); ++i)
                if (i != skip) rest.push_back(edits[i]);
            std::vector<u8> built;
            std::vector<SectionEdit> found;
            std::wstring why;
            bool ok = BuildSection(world, section, rest, built, why) &&
                      DiffSectionBytes(world, section, ByteSpan{ built.data(), built.size() },
                                       section.originalStreamOffset, found, why) &&
                      found.size() == rest.size();
            for (size_t i = 0; ok && i < rest.size(); ++i)
                ok = found[i].kind == rest[i].kind && found[i].hash == rest[i].hash &&
                     found[i].chunk == rest[i].chunk;
            if (ok) ++leaveOneOut;
            else LOG_WARN("edits self-test %s: without %S %s the rebuild differs %S",
                          section.name.c_str(), EditKindName(edits[skip].kind),
                          edits[skip].label.c_str(), why.c_str());
        }
        LOG_INFO("edits self-test %s: %zu of %zu leave-one-out rebuilds exact",
                 section.name.c_str(), leaveOneOut, edits.size());

        std::map<std::wstring, int> kinds;
        for (const SectionEdit& edit : edits) ++kinds[EditKindName(edit.kind)];
        std::wstring summary;
        for (const auto& [name, count] : kinds) summary += std::to_wstring(count) + L" " + name + L"; ";

        LOG_INFO("edits self-test %s: %zu edits (%S) rebuilt with all: %s, with none: %s%s%S",
                 section.name.c_str(), edits.size(), summary.c_str(),
                 same ? "same edits" : "DIFFERENT", shipped ? "shipped bytes" : "DIFFERENT",
                 error.empty() ? "" : " -- ", error.c_str());
    }
}
