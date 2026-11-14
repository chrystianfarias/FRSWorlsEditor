#include "nfs/World.h"

#include "io/MeshOverlay.h"

#include "core/Log.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <iterator>

namespace nfs {

u64 PlacementKey(u32 modelHash, const Mat4& transform) {
    // Quantise so that values differing only by float noise agree. An eighth of
    // a unit is far below anything visible at this world scale, and the rotation
    // is folded in coarsely so two copies turned differently stay distinct.
    auto quantise = [](float v) { return (i64)std::lround(v * 8.0f); };

    u64 key = 1469598103934665603ull;                 // FNV-1a offset basis
    auto mix = [&key](u64 value) {
        key ^= value;
        key *= 1099511628211ull;
    };

    mix(modelHash);
    for (int axis = 0; axis < 3; ++axis)
        mix((u64)quantise(transform.m[3][axis]));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            mix((u64)(i64)std::lround(transform.m[row][col] * 64.0f));
    return key;
}

size_t LoadedSection::ObjectCount() const {
    size_t total = 0;
    for (const SolidList& list : solidLists) total += list.objects.size();
    return total;
}

size_t LoadedSection::TriangleCount() const {
    size_t total = 0;
    for (const SolidList& list : solidLists)
        for (const SolidObject& object : list.objects)
            total += object.TriangleCount();
    return total;
}

bool LoadedSection::HoldsModel(u32 hash) const {
    for (const SolidList& list : solidLists)
        for (const SolidObject& object : list.objects)
            if (object.hash == hash) return true;
    return std::find(importedModels.begin(), importedModels.end(), hash) != importedModels.end();
}

const Aabb& LoadedSection::FocusBounds() const {
    if (terrainBounds.Valid()) return terrainBounds;
    if (trackBounds.Valid())   return trackBounds;
    return bounds;
}

std::vector<std::wstring> World::AvailableRegions(const std::wstring& tracksDirectory) {
    std::vector<std::wstring> regions;

    WIN32_FIND_DATAW find{};
    const std::wstring pattern = tracksDirectory + L"\\STREAM*.BUN";
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE) return regions;

    do {
        std::wstring name = find.cFileName;               // STREAML4RA.BUN
        if (name.size() <= 10) continue;
        name = name.substr(6, name.size() - 6 - 4);       // L4RA
        regions.push_back(name);
    } while (::FindNextFileW(handle, &find));

    ::FindClose(handle);
    std::sort(regions.begin(), regions.end());
    return regions;
}

bool World::Open(const std::wstring& tracksDirectory, const std::wstring& region) {
    Close();

    if (!layout_.Open(tracksDirectory + L"\\" + region + L".BUN"))
        return false;

    if (!stream_.Open(tracksDirectory + L"\\STREAM" + region + L".BUN")) {
        layout_.Close();
        return false;
    }

    // Visibility is optional: a region without it simply falls back to
    // drawing by distance, which is worse but not fatal.
    preculler_.Parse(layout_.Span());
    shippedVisibility_ = preculler_;

    // The edits live beside the game, not in it, so they are gathered here and
    // applied to each mesh as its section is parsed. That way the viewport and
    // the running game are looking at the same geometry.
    overlays_.clear();
    if (!overlayFolder_.empty()) {
        const size_t loaded = ReadMeshOverlays(overlayFolder_, region, overlays_);
        if (loaded) LOG_INFO("%zu edited meshes loaded from the models folder", loaded);

        spans_.clear();
        const size_t spans = ReadSpanOverlays(overlayFolder_, region, spans_);
        LOG_INFO("models folder %S: %zu edited meshes, %zu rebuilt spans",
                 overlayFolder_.c_str(), loaded, spans);
    }

    if (!table_.Parse(layout_.Span())) {
        Close();
        return false;
    }

    // The table corrections come after it is parsed, so a section that moved is
    // looked for where its bytes now are.
    bool layoutEdited = false;
    for (const SpanOverlay& span : spans_)
        if (span.layoutFile) {
            table_.ApplyBytes(span.fileOffset, span.bytes.data(), span.bytes.size());
            layoutEdited = true;
        }
    // Edited visibility lists (ui/ZoneTools.h) are spans of the layout file
    // too: the zones are read again from the file as the game will see it.
    if (layoutEdited) {
        const ByteSpan original = layout_.Span();
        std::vector<u8> patched(original.data, original.data + original.size);
        for (const SpanOverlay& span : spans_)
            if (span.layoutFile && span.fileOffset + span.bytes.size() <= patched.size())
                std::copy(span.bytes.begin(), span.bytes.end(), patched.begin() + (ptrdiff_t)span.fileOffset);
        preculler_.Parse(ByteSpan{ patched.data(), patched.size() });
    }

    OpenGlobalPacks(tracksDirectory);
    ReadRaceBarrierGroups();
    objectAnimations_.Parse(layout_.Span());

    region_ = region;
    sectionToLoaded_.assign(table_.Sections().size(), -1);
    return true;
}

std::vector<size_t> World::SectionsLoadedThroughout(const std::vector<Vec3>& points) const {
    std::vector<size_t> out;
    if (preculler_.IsEmpty() || points.empty()) return out;
    std::vector<u32> common;
    bool first = true;
    const auto narrow = [&](std::vector<u32> listed) {
        std::sort(listed.begin(), listed.end());
        if (first) { common = std::move(listed); first = false; return; }
        std::vector<u32> both;
        std::set_intersection(common.begin(), common.end(), listed.begin(), listed.end(), std::back_inserter(both));
        common.swap(both);
    };
    for (const Vec3& point : points) {
        std::vector<u32> listed;
        if (preculler_.VisibleAt(point, listed)) { narrow(std::move(listed)); continue; }
        // No zone claims it: the car keeps the zone it came from, any of the
        // ones around -- every one of them must load it.
        std::vector<std::vector<u32>> around;
        preculler_.ListsNear(point, 80.0f, around);
        for (auto& list : around) narrow(std::move(list));
    }
    for (size_t s = 0; s < loaded_.size(); ++s)
        if (loaded_[s]->info && !IsLibrary(*loaded_[s]) &&
            std::binary_search(common.begin(), common.end(), loaded_[s]->info->number))
            out.push_back(s);
    return out;
}

// 32-byte records {u32 hash, u32 polygon count, u8 1, char name[23]}, the
// first one "NONE". Read from the top level of the layout file.
void World::ReadRaceBarrierGroups() {
    raceGroups_.clear();
    ChunkReader reader(layout_.Span(), 0);
    Chunk chunk;
    while (reader.Next(chunk)) {
        if (chunk.id != kChunkRaceBarrierGroups) continue;
        for (size_t at = 0; at + 32 <= chunk.data.size; at += 32) {
            const char* name = reinterpret_cast<const char*>(chunk.data.data + at + 9);
            raceGroups_.emplace_back(name, strnlen(name, 23));
        }
        break;
    }
    LOG_INFO("%zu race barrier groups", raceGroups_.size());
}

// 0x30300102: 52-byte entries {name[24], first frame hash, frame count, fps,
// 16 bytes unknown}; 0x30300103: every entry's frames in turn, 16 bytes each
// with the texture hash first.
void World::ReadTextureAnimations(const Chunk& container) {
    ByteSpan entries, frames;
    ChunkReader reader = ChunkReader::Children(container);
    Chunk child;
    while (reader.Next(child)) {
        if (child.id == 0x30300102u) entries = child.data;
        else if (child.id == 0x30300103u) frames = child.data;
    }
    size_t next = 0;
    for (size_t at = 0; at + 52 <= entries.size; at += 52) {
        TextureAnimation animation;
        const char* name = reinterpret_cast<const char*>(entries.data + at);
        animation.name.assign(name, strnlen(name, 24));
        const u32 hash = entries.read<u32>(at + 24);
        const u32 count = entries.read<u32>(at + 28);
        animation.fps = (float)entries.read<u32>(at + 32);
        for (u32 k = 0; k < count && (next + 1) * 16 <= frames.size; ++k, ++next)
            animation.frames.push_back(frames.read<u32>(next * 16));
        if (animation.frames.size() > 1 && animation.fps > 0.0f) {
            LOG_INFO("texture animation %s: %zu frames at %.0f/s", animation.name.c_str(), animation.frames.size(),
                     animation.fps);
            textureAnimations_[hash] = std::move(animation);
        }
    }
}

std::string World::RaceBarrierGroupName(u8 group) const {
    if (group == 0) return "NONE";
    if (group < raceGroups_.size()) return raceGroups_[group];
    return "GROUP_" + std::to_string(group);
}

void World::Close() {
    region_.clear();
    raceGroups_.clear();
    textureAnimations_.clear();
    objectAnimations_.Clear();
    layout_.Close();
    stream_.Close();
    globalPacks_.clear();
    globalFiles_.clear();
    loaded_.clear();
    sectionToLoaded_.clear();
    modelsByHash_.clear();
    modelHome_.clear();
    instancedHashes_.clear();
    placements_.clear();
    instanceCount_ = 0;
    textureUsage_.clear();
    bounds_ = {};
}

const LoadedSection* World::LoadSection(const Section& section) {
    const auto& sections = table_.Sections();
    const size_t index = (size_t)(&section - sections.data());
    if (index >= sections.size()) return nullptr;

    if (sectionToLoaded_[index] >= 0)
        return loaded_[(size_t)sectionToLoaded_[index]].get();

    // Mark as visited up front so a section without geometry is not re-parsed.
    sectionToLoaded_[index] = -2;

    // A section that moved past the end of the file is not an error: its bytes
    // live in a span, and the check below would reject it for pointing at
    // nothing.
    bool relocated = false;
    for (const SpanOverlay& span : spans_)
        if (!span.layoutFile && span.fileOffset == section.streamOffset) relocated = true;

    if (!relocated && section.streamOffset + section.streamSize > stream_.Size()) {
        LOG_WARN("section %s points outside the stream file", section.name.c_str());
        return nullptr;
    }

    const ByteSpan buffer = SectionBytes(section);

    auto loadedSection = std::make_unique<LoadedSection>();
    loadedSection->info = &section;

    // When the section comes from an earlier save, the shipped scenery counts
    // say which records are the editor's own.
    std::vector<size_t> shippedInstances;
    const bool fromSpan = buffer.data < stream_.Span().data ||
                          buffer.data >= stream_.Span().data + stream_.Size();
    if (fromSpan &&
        section.originalStreamOffset + (u64)section.originalStreamSize <= stream_.Size()) {
        const ByteSpan original =
            stream_.Span().sub(section.originalStreamOffset, section.originalStreamSize);
        ChunkReader shipped(original, section.originalStreamOffset);
        Chunk chunk;
        while (shipped.Next(chunk)) {
            if (chunk.id != kChunkTrackScenery) continue;
            SceneryChunkInfo info;
            ReadSceneryChunk(ByteSpan{ chunk.raw.data - 8, chunk.raw.size + 8 },
                             chunk.headerOffset, info);
            shippedInstances.push_back(info.instanceCount);
        }
    }
    size_t sceneryIndex = 0;

    ChunkReader reader(buffer, section.streamOffset);
    Chunk chunk;
    while (reader.Next(chunk)) {
        if (chunk.id == kChunkSolidListPack) {
            SolidList list;
            if (ParseSolidList(chunk, list)) {
                // An edited mesh replaces its positions here, before anything
                // measures bounds or uploads it, so every consumer sees the
                // shape the game will be handed and not the shipped one.
                if (!overlays_.empty()) {
                    for (SolidObject& object : list.objects) {
                        const auto found = overlays_.find(object.hash);
                        if (found == overlays_.end()) continue;
                        if (found->second.size() != object.vertices.size()) {
                            LOG_WARN("edited mesh '%s' has %zu vertices against the "
                                     "game's %zu, so it was ignored",
                                     object.name.c_str(), found->second.size(),
                                     object.vertices.size());
                            continue;
                        }

                        for (size_t i = 0; i < object.vertices.size(); ++i)
                            object.vertices[i].position = found->second[i];

                        object.bounds = {};
                        for (const SolidVertex& vertex : object.vertices)
                            object.bounds.Add(vertex.position);
                    }
                }

                loadedSection->solidLists.push_back(std::move(list));
            }
        } else if (chunk.id == kChunkTrackScenery) {
            const ByteSpan whole{ chunk.raw.data - 8, chunk.raw.size + 8 };
            SceneryChunkInfo info;
            ReadSceneryChunk(whole, chunk.headerOffset, info);
            loadedSection->sceneryNumbers.push_back(info.number);

            // Opt-in check of the scenery writer: FRS_SCENERY_SELFTEST=1.
            static const bool selfTest = std::getenv("FRS_SCENERY_SELFTEST") != nullptr;
            if (selfTest) {
                std::vector<u8> same, grown;
                std::wstring error;
                std::string why, whyGrown;
                const bool identical = AppendScenery(whole, chunk.headerOffset, {}, same, error) &&
                                       same.size() == whole.size &&
                                       std::memcmp(same.data(), whole.data, whole.size) == 0;
                const bool valid = ValidateScenery(whole, chunk.headerOffset, why);

                // And with three made-up placements appended: the result has
                // to be a valid tree holding every record once.
                bool grownValid = false;
                std::vector<PlacedInstance> probe(3);
                for (size_t p = 0; p < probe.size(); ++p) {
                    probe[p].modelHash = 0x12345678u + (u32)p;
                    probe[p].modelName = "SELFTEST";
                    probe[p].transform = Mat4::Translation(
                        { section.position.x + p * 7.0f, section.position.y, section.position.z });
                    probe[p].modelBounds.Add({ -1, -1, 0 });
                    probe[p].modelBounds.Add({ 1, 1, 2 });
                }
                if (AppendScenery(whole, chunk.headerOffset, probe, grown, error)) {
                    SceneryChunkInfo grownInfo;
                    grownValid = ValidateScenery(ByteSpan{ grown.data(), grown.size() },
                                                 chunk.headerOffset, whyGrown) &&
                                 ReadSceneryChunk(ByteSpan{ grown.data(), grown.size() },
                                                  chunk.headerOffset, grownInfo) &&
                                 grownInfo.instanceCount == info.instanceCount + 3;
                }
                LOG_INFO("scenery self-test %s #%u: re-encode %s, shipped tree %s, grown %s",
                         section.name.c_str(), info.number, identical ? "identical" : "DIFFERENT",
                         valid ? "valid" : why.c_str(),
                         grownValid ? "valid" : (whyGrown.empty() ? "FAILED" : whyGrown.c_str()));
            }

            Scenery& scenery = loadedSection->scenery;
            const size_t before = scenery.instances.size();
            ParseScenery(chunk, scenery);
            for (size_t i = before; i < scenery.instances.size(); ++i)
                scenery.instances[i].sceneryNumber = info.number;

            const size_t k = sceneryIndex++;
            if (k < shippedInstances.size() && info.instanceCount > shippedInstances[k] &&
                scenery.instances.size() - before == info.instanceCount) {
                std::vector<PlacedInstance> placed;
                ReadSceneryPlacements(whole, chunk.headerOffset, shippedInstances[k], placed);
                scenery.instances.resize(before + shippedInstances[k]);
                LOG_INFO("section %s: %zu objects placed by the editor read back", section.name.c_str(),
                         placed.size());
                for (PlacedInstance& p : placed)
                    loadedSection->placements.push_back({ info.number, std::move(p) });
            }
        } else if (chunk.id == kChunkCollisionPack) {
            CollisionPack pack;
            if (ParseCollision(chunk, pack)) {
                // Opt-in check that the encoder reproduces the shipped pack:
                // set FRS_COLLISION_SELFTEST=1 before starting the editor.
                static const bool selfTest = std::getenv("FRS_COLLISION_SELFTEST") != nullptr;
                if (selfTest) {
                    std::vector<u8> encoded;
                    std::wstring error;
                    const size_t originalSize = (size_t)chunk.raw.size + 8;
                    const u8* original = chunk.raw.data - 8;
                    const bool same = EncodeCollision(pack, encoded, error) &&
                                      encoded.size() == originalSize &&
                                      std::memcmp(encoded.data(), original, originalSize) == 0;
                    size_t firstDiff = 0;
                    while (!same && firstDiff < std::min(encoded.size(), originalSize) &&
                           encoded[firstDiff] == original[firstDiff]) ++firstDiff;
                    LOG_INFO("collision self-test %s: %s (%zu vs %zu bytes, first difference at %zu)",
                             section.name.c_str(), same ? "identical" : "DIFFERENT",
                             encoded.size(), originalSize, same ? (size_t)0 : firstDiff);
                }
                loadedSection->collision.push_back(std::move(pack));
            }
        } else if (chunk.id == kChunkTexturePack) {
            auto pack = std::make_unique<TexturePack>();
            if (pack->Parse(chunk))
                loadedSection->texturePacks.push_back(std::move(pack));
        } else if (chunk.id == 0xB0300100u) {
            ReadTextureAnimations(chunk);
        }
    }

    if (loadedSection->solidLists.empty() && loadedSection->texturePacks.empty() &&
        loadedSection->scenery.Empty() && loadedSection->collision.empty() &&
        loadedSection->placements.empty() && loadedSection->sceneryNumbers.empty())
        return nullptr;

    for (SolidList& list : loadedSection->solidLists) {
        for (SolidObject& object : list.objects) {
            // Claim this placement. A later copy of the same mesh in the same
            // spot is redundant and must not be drawn again.
            if (!placements_.insert(PlacementKey(object.hash, object.transform)).second)
                object.duplicatePlacement = true;

            const Aabb worldBounds = object.WorldBounds();
            loadedSection->bounds.Add(worldBounds);
            if (object.IsBackdrop()) continue;
            loadedSection->trackBounds.Add(worldBounds);
            if (object.IsTerrain()) loadedSection->terrainBounds.Add(worldBounds);
        }
    }

    // Instances are the bulk of a section's content, so they have to be part of
    // its bounds or frustum culling would discard the props with their block.
    for (const SceneryInstance& instance : loadedSection->scenery.instances) {
        loadedSection->bounds.Add(instance.worldBounds);
        loadedSection->trackBounds.Add(instance.worldBounds);
    }

    // A scenery instance names a mesh by hash, so the index must point at the
    // copy that actually got uploaded -- never at one suppressed as a duplicate.
    for (const SolidList& list : loadedSection->solidLists)
        for (const SolidObject& object : list.objects)
            if (!object.duplicatePlacement) {
                modelsByHash_.emplace(object.hash, &object);
                modelHome_.emplace(object.hash, loaded_.size());
            }

    for (const SceneryInstance& instance : loadedSection->scenery.instances)
        instancedHashes_.insert(loadedSection->scenery.ModelHash(instance));

    for (const SolidList& list : loadedSection->solidLists)
        for (const SolidObject& object : list.objects)
            for (const SolidMaterial& material : object.materials)
                if (material.textureIndex < object.textureHashes.size())
                    textureUsage_[object.textureHashes[material.textureIndex]]++;

    instanceCount_ += loadedSection->scenery.instances.size();

    bounds_.Add(loadedSection->bounds);

    sectionToLoaded_[index] = (int)loaded_.size();
    loaded_.push_back(std::move(loadedSection));
    return loaded_.back().get();
}

ByteSpan World::SectionBytes(const Section& section) const {
    // A rebuilt section is read out of its span instead of out of the file. The
    // span holds the section whole, so it stands in for that range exactly.
    for (const SpanOverlay& span : spans_) {
        if (span.layoutFile || span.fileOffset != section.streamOffset) continue;
        return ByteSpan{ span.bytes.data(), span.bytes.size() };
    }
    return stream_.Span().sub(section.streamOffset, section.streamSize);
}

void World::LoadAllSections(void (*progress)(size_t, size_t, void*), void* userData) {
    const auto& sections = table_.Sections();
    for (size_t i = 0; i < sections.size(); ++i) {
        LoadSection(sections[i]);
        if (progress) progress(i + 1, sections.size(), userData);
    }

    size_t objects = 0, triangles = 0;
    for (const auto& section : loaded_) {
        objects   += section->ObjectCount();
        triangles += section->TriangleCount();
    }
    LOG_INFO("region %ls: %zu sections with data, %zu objects, %zu triangles, "
             "%zu scenery instances (%zu distinct models placed)",
             region_.c_str(), loaded_.size(), objects, triangles,
             instanceCount_, instancedHashes_.size());
}

Aabb World::TrackBounds() const {
    // Built from the section table rather than the geometry. Terrain and props
    // routinely reach past the block that stores them -- in L4RB the terrain
    // spans 10 700 units while the section centres span only 3 300 -- so a box
    // grown from the meshes frames the camera several times too far back.
    Aabb bounds = table_.Bounds();
    if (!bounds.Valid()) return bounds_;

    bounds = Pad(bounds, SectionPadding());
    return bounds;
}

Aabb World::SectionBounds(const LoadedSection& section) const {
    if (section.info && (section.info->position.x != 0.0f || section.info->position.y != 0.0f)) {
        Aabb box;
        box.Add(section.info->position);
        return Pad(box, SectionPadding());
    }
    return section.FocusBounds();
}

float World::SectionPadding() const {
    // Half a section reaches from a centre to the edge of its own block. Regions
    // with a single positioned section have no spacing to measure, so fall back
    // to something on the scale of a city block.
    const float spacing = table_.TypicalSectionSpacing();
    return spacing > 0.0f ? spacing * 0.5f : 300.0f;
}

size_t World::ModelHome(u32 hash) const {
    const auto it = modelHome_.find(hash);
    return it == modelHome_.end() ? SIZE_MAX : it->second;
}

bool World::IsLibrary(const LoadedSection& section) const {
    return !section.info ||
           (section.info->position.x == 0.0f && section.info->position.y == 0.0f);
}

size_t World::SectionForPlacement(const Vec3& point) const {
    size_t best = SIZE_MAX, nearest = SIZE_MAX;
    float bestDistance = 1e30f, nearestDistance = 1e30f;

    for (size_t i = 0; i < loaded_.size(); ++i) {
        const LoadedSection& candidate = *loaded_[i];
        if (!candidate.info || IsLibrary(candidate) || IsUnusedAlternate(*candidate.info)) continue;
        if (std::find(candidate.sceneryNumbers.begin(), candidate.sceneryNumbers.end(),
                      candidate.info->number) == candidate.sceneryNumbers.end())
            continue;

        const float dx = candidate.info->position.x - point.x;
        const float dy = candidate.info->position.y - point.y;
        const float distance = dx * dx + dy * dy;
        if (distance < nearestDistance) { nearestDistance = distance; nearest = i; }

        const Aabb& ground = candidate.terrainBounds;
        if (ground.Valid() && point.x >= ground.min.x && point.x <= ground.max.x &&
            point.y >= ground.min.y && point.y <= ground.max.y && distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best != SIZE_MAX ? best : nearest;
}

size_t World::SectionForPlacementAlong(const std::vector<Vec3>& points) const {
    if (points.empty()) return SIZE_MAX;
    Vec3 middle{};
    for (const Vec3& p : points) middle = middle + p;
    middle = middle * (1.0f / (float)points.size());
    if (preculler_.IsEmpty()) return SectionForPlacement(middle);
    const size_t usual = SectionForPlacement(middle);
    size_t best = SIZE_MAX;
    float bestDistance = 1e30f;
    for (size_t i : SectionsLoadedThroughout(points)) {
        const LoadedSection& candidate = *loaded_[i];
        if (!candidate.info || IsLibrary(candidate) || IsUnusedAlternate(*candidate.info)) continue;
        if (std::find(candidate.sceneryNumbers.begin(), candidate.sceneryNumbers.end(), candidate.info->number) ==
            candidate.sceneryNumbers.end())
            continue;
        if (i == usual) return i;
        const float dx = candidate.info->position.x - middle.x, dy = candidate.info->position.y - middle.y;
        if (dx * dx + dy * dy < bestDistance) { bestDistance = dx * dx + dy * dy; best = i; }
    }
    return best;
}

std::vector<const SolidObject*> World::AllModels() const {
    std::vector<const SolidObject*> out;
    out.reserve(modelsByHash_.size());
    for (const auto& [hash, model] : modelsByHash_) out.push_back(model);
    std::sort(out.begin(), out.end(), [](const SolidObject* a, const SolidObject* b) {
        return a->name < b->name;
    });
    return out;
}

const SolidObject* World::FindModel(u32 hash) const {
    const auto it = modelsByHash_.find(hash);
    return it == modelsByHash_.end() ? nullptr : it->second;
}

std::vector<const Texture*> World::AllTextures() const {
    std::vector<const Texture*> result;
    std::unordered_set<u32> seen;

    for (const auto& section : loaded_) {
        for (const auto& pack : section->texturePacks) {
            for (const Texture& texture : pack->Textures()) {
                if (seen.insert(texture.hash).second)
                    result.push_back(&texture);
            }
        }
    }
    return result;
}

u32 World::TextureUsage(u32 hash) const {
    const auto it = textureUsage_.find(hash);
    return it == textureUsage_.end() ? 0u : it->second;
}

const Texture* World::FindTexture(u32 hash) const {
    for (const auto& section : loaded_)
        for (const auto& pack : section->texturePacks)
            if (const Texture* texture = pack->Find(hash)) return texture;
    return nullptr;
}

const Texture* World::FindDisplayTexture(u32 hash) const {
    if (const Texture* texture = FindTexture(hash)) return texture;
    for (const auto& pack : globalPacks_)
        if (const Texture* texture = pack->Find(hash)) return texture;
    return nullptr;
}

// Optional: a missing file only leaves those textures grey, as before.
void World::OpenGlobalPacks(const std::wstring& tracksDirectory) {
    const size_t slash = tracksDirectory.find_last_of(L"\\/");
    const std::wstring game = slash == std::wstring::npos ? L"." : tracksDirectory.substr(0, slash);

    // The global packs, and the city's dynamic textures (TRACKS\LOC4DYNTEX.BIN):
    // the sky dome draws from those, which the game swaps with the weather.
    std::vector<std::wstring> paths;
    for (const wchar_t* name : { L"InGameCommon.bun", L"GLOBALB.BUN" }) paths.push_back(game + L"\\GLOBAL\\" + name);
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((tracksDirectory + L"\\LOC*DYNTEX.BIN").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do paths.push_back(tracksDirectory + L"\\" + found.cFileName);
        while (::FindNextFileW(search, &found));
        ::FindClose(search);
    }
    for (const std::wstring& path : paths) {
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

        auto file = std::make_unique<FileMap>();
        if (!file->Open(path)) continue;

        size_t packs = 0;
        ChunkReader top(file->Span(), 0);
        Chunk chunk;
        while (top.Next(chunk)) {
            if (chunk.id != kChunkTexturePack) continue;
            auto pack = std::make_unique<TexturePack>();
            if (pack->Parse(chunk)) { globalPacks_.push_back(std::move(pack)); ++packs; }
        }
        LOG_INFO("global textures: %zu packs from %S", packs, path.c_str());
        globalFiles_.push_back(std::move(file));
    }
}

} // namespace nfs
