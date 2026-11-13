#include "io/PlacementWrite.h"

#include "core/Log.h"
#include "io/ModelCopy.h"
#include "io/SectionEdits.h"
#include "io/SectionRebuild.h"
#include <algorithm>
#include "nfs/ChunkReader.h"
#include "nfs/SceneryEdit.h"
#include "nfs/World.h"

#include <sstream>

namespace {

} // namespace

bool PlanPlacementSave(const nfs::World& world, const std::wstring& modelsFolder,
                       PlacementSavePlan& plan) {
    std::wostringstream report;

    std::vector<SpanOverlayInfo> existing;
    ListSpanOverlays(modelsFolder, world.Region(), existing);

    const auto& loaded = world.Loaded();
    for (size_t index = 0; index < loaded.size(); ++index) {
        const nfs::LoadedSection& section = *loaded[index];
        if (!section.placementsChanged || !section.info) continue;
        const nfs::Section& entry = *section.info;
        const std::wstring name(entry.name.begin(), entry.name.end());

        // The edits in force, with the placements replaced by the editor's
        // own list.
        std::vector<SectionEdit> edits;
        std::wstring error;
        if (!DiffSection(world, entry, edits, error)) {
            report << name << L": " << error << L" Left alone.\n";
            plan.ok = false;
            continue;
        }
        std::vector<SectionEdit> kept;
        for (SectionEdit& edit : edits)
            if (edit.kind != SectionEdit::Kind::Placement) kept.push_back(std::move(edit));

        // Shipped records removed in the editor: kept in place, matrix zeroed.
        if (!section.removedNatives.empty()) {
            const ByteSpan original =
                world.StreamBytes().sub(entry.originalStreamOffset, entry.originalStreamSize);
            nfs::ChunkReader reader(original, entry.originalStreamOffset);
            nfs::Chunk chunk;
            while (reader.Next(chunk)) {
                if (chunk.id != nfs::kChunkTrackScenery) continue;
                const ByteSpan whole{ chunk.raw.data - 8, chunk.raw.size + 8 };
                nfs::SceneryChunkInfo info;
                nfs::ReadSceneryChunk(whole, chunk.headerOffset, info);
                std::vector<std::vector<u8>> records;
                nfs::ReadSceneryRecords(whole, chunk.headerOffset, records);
                for (const auto& [number, record] : section.removedNatives) {
                    if (number != info.number || record >= records.size()) continue;
                    const bool already = std::any_of(kept.begin(), kept.end(), [&](const SectionEdit& e) {
                        return e.kind == SectionEdit::Kind::NativeDeleted && e.sceneryNumber == number &&
                               e.index == record;
                    });
                    if (already) continue;
                    SectionEdit edit;
                    edit.kind = SectionEdit::Kind::NativeDeleted;
                    edit.sceneryNumber = number;
                    edit.index = record;
                    edit.record = nfs::DeletedRecord(records[record]);
                    kept.push_back(std::move(edit));
                }
            }
        }

        for (const auto& placement : section.placements) {
            SectionEdit edit;
            edit.kind = SectionEdit::Kind::Placement;
            edit.sceneryNumber = placement.sceneryNumber;
            edit.placement = placement.instance;
            edit.label = placement.instance.modelName;
            kept.push_back(std::move(edit));
        }

        // Models to copy in, with the textures the section cannot see.
        size_t copiedModels = 0, copiedTextures = 0;
        bool failed = false;
        for (const u32 hash : section.importedModels) {
            const bool present = std::any_of(kept.begin(), kept.end(), [&](const SectionEdit& e) {
                return e.kind == SectionEdit::Kind::ModelAdded && e.hash == hash;
            });
            bool shipped = false;
            for (const nfs::SolidList& solids : section.solidLists)
                for (const nfs::SolidObject& object : solids.objects)
                    if (object.hash == hash) shipped = true;
            if (present || shipped) continue;

            ByteSpan object;
            u64 objectOffset = 0;
            if (!FindModelChunk(world, hash, object, objectOffset)) {
                report << name << L": a model to copy in could not be found. Left alone.\n";
                failed = true;
                break;
            }
            SectionEdit model;
            model.kind = SectionEdit::Kind::ModelAdded;
            model.hash = hash;
            model.chunk.assign(object.data, object.data + object.size);
            model.mod128 = (u32)(objectOffset % 128);
            if (const nfs::SolidObject* solid = world.FindModel(hash)) model.label = solid->name;
            kept.push_back(std::move(model));
            ++copiedModels;

            std::wstring problem;
            for (NewTexture& texture : TexturesToCopy(world, hash, index, problem)) {
                const bool had = std::any_of(kept.begin(), kept.end(), [&](const SectionEdit& e) {
                    return (e.kind == SectionEdit::Kind::TextureAdded ||
                            e.kind == SectionEdit::Kind::TextureChanged) && e.hash == texture.hash;
                });
                if (had) continue;
                SectionEdit edit;
                edit.kind = SectionEdit::Kind::TextureAdded;
                edit.hash = texture.hash;
                edit.label = texture.name;
                edit.texture = std::move(texture);
                kept.push_back(std::move(edit));
                ++copiedTextures;
            }
            if (!problem.empty()) {
                report << name << L": " << problem << L"Left alone.\n";
                failed = true;
                break;
            }
        }
        if (failed) { plan.ok = false; continue; }

        SectionSpanPlan spans;
        if (!PlanSectionFromEdits(world, entry, kept, existing, spans, error)) {
            report << name << L": " << error << L" Left alone.\n";
            plan.ok = false;
            continue;
        }
        for (SectionSpan& span : spans.spans) plan.spans.push_back(std::move(span));
        for (std::wstring& path : spans.superseded) plan.superseded.push_back(std::move(path));
        plan.sections.push_back(index);

        report << name << L": " << section.placements.size() << L" placed objects, "
               << section.removedNatives.size() << L" removed";
        if (copiedModels) report << L", " << copiedModels << L" models and " << copiedTextures
                                 << L" textures copied in";
        if (spans.moved) report << L"; the section moves past the end of the stream";
        report << L".\n";
        LOG_INFO("placements %s: %zu placed, section %u bytes at %llu%s",
                 entry.name.c_str(), section.placements.size(), spans.size,
                 spans.offset, spans.tableChanged ? " (table corrected)" : "");
    }

    plan.report = report.str();
    return plan.ok && (!plan.spans.empty() || !plan.superseded.empty());
}
