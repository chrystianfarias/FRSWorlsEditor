#include "io/MarkerEffects.h"

#include "core/Log.h"
#include "io/MeshOverlay.h"
#include "io/SectionEdits.h"
#include "nfs/ChunkIds.h"
#include "nfs/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace {

constexpr size_t kHeaderBytes = 16;    // section number, 0, 6, count
constexpr size_t kRecordBytes = 208;
constexpr size_t kSection     = 0x2A;  // u16: the section the emitter belongs to
constexpr size_t kRows        = 0x40;  // three rows of four floats
constexpr size_t kPosition    = 0x70;
constexpr size_t kName        = 0x90;

// How far from its shop's zone a shipped emitter may stand to count as part of
// that shop's set.
constexpr float kSetRadius = 40.0f;

float GetF(const std::vector<u8>& bytes, size_t at) {
    float v;
    std::memcpy(&v, bytes.data() + at, 4);
    return v;
}
void SetF(std::vector<u8>& bytes, size_t at, float v) { std::memcpy(bytes.data() + at, &v, 4); }
u32 Get32(const std::vector<u8>& bytes, size_t at) {
    u32 v;
    std::memcpy(&v, bytes.data() + at, 4);
    return v;
}
void Set32(std::vector<u8>& bytes, size_t at, u32 v) { std::memcpy(bytes.data() + at, &v, 4); }

// "XEE_Event_BODYSHOP_" and so on.
std::string EffectPrefix(nfs::MarkerKind kind) {
    const char* part = nfs::MarkerEffectPrefix(kind);
    return part ? std::string("XEE_Event_") + part : std::string{};
}

struct Emitter {
    std::vector<u8> record;
    std::string name;
    Vec3 position;
};

// Every emitter the region shipped with.
std::vector<Emitter> ShippedEmitters(const nfs::World& world) {
    std::vector<Emitter> out;
    for (const nfs::Section& section : world.Table().Sections()) {
        std::vector<u8> data;
        if (!ShippedChunkData(world, section, nfs::kChunkEmitters, data) || data.size() < kHeaderBytes)
            continue;
        const u32 count = Get32(data, 12);
        for (u32 i = 0; i < count && kHeaderBytes + (i + 1) * kRecordBytes <= data.size(); ++i) {
            Emitter emitter;
            const size_t at = kHeaderBytes + i * kRecordBytes;
            emitter.record.assign(data.begin() + (std::ptrdiff_t)at,
                                  data.begin() + (std::ptrdiff_t)(at + kRecordBytes));
            const char* name = (const char*)emitter.record.data() + kName;
            emitter.name.assign(name, ::strnlen(name, kRecordBytes - kName));
            emitter.position = { GetF(emitter.record, kPosition), GetF(emitter.record, kPosition + 4),
                                 GetF(emitter.record, kPosition + 8) };
            out.push_back(std::move(emitter));
        }
    }
    return out;
}

// The set a shipped shop of `kind` carries, and where that shop stands: the
// zone's centre and heading, and the ground under it (the ring lies on it).
struct EffectSet {
    std::vector<const Emitter*> emitters;
    Vec2  centre;
    float heading = 0.0f;
    float ground = 0.0f;
};

bool FindSet(const nfs::MarkerSet& markers, const std::vector<Emitter>& shipped, nfs::MarkerKind kind,
             EffectSet& set) {
    const std::string prefix = EffectPrefix(kind);
    if (prefix.empty()) return false;
    const size_t length = prefix.size();
    for (const nfs::Marker& shop : markers.Markers()) {
        if (shop.IsPlaced() || shop.kind != kind) continue;
        EffectSet found;
        found.centre = { shop.position.x, shop.position.y };
        found.heading = shop.heading;
        bool ring = false;
        float lowest = 1e30f;
        for (const Emitter& emitter : shipped) {
            if (emitter.name.compare(0, length, prefix) != 0) continue;
            const float dx = emitter.position.x - found.centre.x, dy = emitter.position.y - found.centre.y;
            if (dx * dx + dy * dy > kSetRadius * kSetRadius) continue;
            found.emitters.push_back(&emitter);
            lowest = std::fmin(lowest, emitter.position.z);
            if (emitter.name.find("_Ring") != std::string::npos) {
                found.ground = emitter.position.z;
                ring = true;
            }
        }
        if (found.emitters.empty()) continue;
        if (!ring) found.ground = lowest;
        set = std::move(found);
        return true;
    }
    return false;
}

// One set's emitters, turned and moved so the set stands at `marker` as it
// stood at its own shop.
void PlaceSet(const EffectSet& set, const nfs::Marker& marker, u32 sectionNumber,
              std::vector<std::vector<u8>>& out) {
    const float turn = Radians(marker.heading - set.heading);
    const float c = std::cos(turn), s = std::sin(turn);
    for (const Emitter* emitter : set.emitters) {
        std::vector<u8> record = emitter->record;
        const u16 number = (u16)sectionNumber;
        std::memcpy(record.data() + kSection, &number, 2);
        for (int row = 0; row < 3; ++row) {
            const size_t at = kRows + (size_t)row * 16;
            const float x = GetF(record, at), y = GetF(record, at + 4);
            SetF(record, at, x * c - y * s);
            SetF(record, at + 4, x * s + y * c);
        }
        const float dx = emitter->position.x - set.centre.x, dy = emitter->position.y - set.centre.y;
        SetF(record, kPosition, marker.position.x + dx * c - dy * s);
        SetF(record, kPosition + 4, marker.position.y + dx * s + dy * c);
        SetF(record, kPosition + 8, marker.position.z + (emitter->position.z - set.ground));
        out.push_back(std::move(record));
    }
}

} // namespace

std::vector<MarkerEffectPreview> PreviewMarkerEffects(const nfs::World& world, const nfs::MarkerSet& markers,
                                                      const nfs::Marker& marker) {
    // Read once per region: every section's emitters is a walk of the file.
    static const nfs::World* cachedWorld = nullptr;
    static std::wstring cachedRegion;
    static std::vector<Emitter> shipped;
    if (cachedWorld != &world || cachedRegion != world.Region()) {
        shipped = ShippedEmitters(world);
        cachedWorld = &world;
        cachedRegion = world.Region();
    }
    std::vector<MarkerEffectPreview> out;
    if (marker.IsRaceStart()) return out;
    EffectSet set;
    if (!FindSet(markers, shipped, marker.GameKind(), set)) return out;
    const float turn = Radians(marker.heading - set.heading);
    const float c = std::cos(turn), s = std::sin(turn);
    for (const Emitter* emitter : set.emitters) {
        const float dx = emitter->position.x - set.centre.x, dy = emitter->position.y - set.centre.y;
        out.push_back({ { marker.position.x + dx * c - dy * s, marker.position.y + dx * s + dy * c,
                          marker.position.z + (emitter->position.z - set.ground) },
                        emitter->name });
    }
    return out;
}

bool PlanMarkerEffects(const nfs::World& world, const std::wstring& modelsFolder,
                       const nfs::MarkerSet& markers, MarkerEffectsPlan& plan,
                       std::wstring& error) {
    const std::vector<Emitter> shipped = ShippedEmitters(world);

    // The records each section is to gain, by loaded index.
    std::map<size_t, std::vector<std::vector<u8>>> wanted;
    for (const nfs::Marker& marker : markers.Markers()) {
        // A race start has no effects of its own: the game draws its beacon.
        if (!marker.IsPlaced() || marker.IsRaceStart()) continue;
        EffectSet set;
        if (!FindSet(markers, shipped, marker.style, set)) {
            plan.report += L"No shipped effects for the kind of " +
                           std::wstring(marker.name.begin(), marker.name.end()) + L".\n";
            continue;
        }
        const size_t index = world.SectionForPlacement(marker.position);
        if (index >= world.Loaded().size() || !world.Loaded()[index]->info) continue;
        PlaceSet(set, marker, world.Loaded()[index]->info->number, wanted[index]);
        plan.effects += set.emitters.size();
    }

    std::vector<SpanOverlayInfo> existing;
    ListSpanOverlays(modelsFolder, world.Region(), existing);

    const auto& loaded = world.Loaded();
    for (size_t index = 0; index < loaded.size(); ++index) {
        if (!loaded[index]->info) continue;
        const nfs::Section& section = *loaded[index]->info;

        std::vector<SectionEdit> edits;
        std::wstring why;
        if (!DiffSection(world, section, edits, why)) {
            plan.report += std::wstring(section.name.begin(), section.name.end()) + L": " + why + L"\n";
            continue;
        }
        bool had = false;
        std::vector<SectionEdit> kept;
        for (SectionEdit& edit : edits) {
            if (edit.kind == SectionEdit::Kind::Emitters) had = true;
            else kept.push_back(std::move(edit));
        }
        const auto want = wanted.find(index);
        if (!had && want == wanted.end()) continue;

        if (want != wanted.end()) {
            // The shipped effects, then the markers'.
            std::vector<u8> payload;
            if (!ShippedChunkData(world, section, nfs::kChunkEmitters, payload) ||
                payload.size() < kHeaderBytes) {
                payload.assign(kHeaderBytes, 0);
                Set32(payload, 0, section.number);
                Set32(payload, 8, 6);
            }
            Set32(payload, 12, Get32(payload, 12) + (u32)want->second.size());
            for (const auto& record : want->second) payload.insert(payload.end(), record.begin(), record.end());

            SectionEdit effects;
            effects.kind = SectionEdit::Kind::Emitters;
            effects.label = "effects";
            effects.record = std::move(payload);
            kept.push_back(std::move(effects));
        }

        SectionSpanPlan spans;
        if (!PlanSectionFromEdits(world, section, kept, existing, spans, error)) {
            error = std::wstring(section.name.begin(), section.name.end()) + L": " + error;
            return false;
        }
        for (const SectionSpan& span : spans.spans) {
            SpanOverlayInfo info;
            info.fileOffset = span.fileOffset;
            info.byteCount = (u32)span.bytes.size();
            info.layoutFile = span.layoutFile;
            existing.push_back(info);   // so the next moved section does not land on it
        }
        for (SectionSpan& span : spans.spans) plan.spans.push_back(std::move(span));
        for (std::wstring& path : spans.superseded) plan.superseded.push_back(std::move(path));
        ++plan.sections;
        plan.report += std::wstring(section.name.begin(), section.name.end()) +
                       (want != wanted.end() ? L": marker effects written" : L": marker effects removed") +
                       (spans.moved ? L" (the section moves past the end of the stream)\n" : L"\n");
        LOG_INFO("marker effects %s: %zu emitters, section %u bytes at %llu", section.name.c_str(),
                 want != wanted.end() ? want->second.size() : 0, spans.size, spans.offset);
    }
    return true;
}
