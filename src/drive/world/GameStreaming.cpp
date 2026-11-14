// The game's streaming rules as a data model; see GameStreaming.h and docs/STREAMING-STUDY.md.
#include "drive/world/GameStreaming.h"

#include "core/Log.h"
#include "drive/numerics/Speed2Image.h"
#include "nfs/FreeRoam.h"
#include "nfs/World.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>

namespace bb {

namespace {

template <class T> T Rd(const std::vector<uint8_t>& b, size_t o)
{
    T v{};
    if (o + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + o, sizeof(T));
    return v;
}

struct RawChunk {
    uint32_t Id = 0;
    size_t Data = 0, Size = 0;
};

// One level of chunks in [offset, end), as the game walks them (no alignment handling: the filler chunks are
// chunks too). An overrunning chunk ends the walk.
std::vector<RawChunk> Chunks(const std::vector<uint8_t>& b, size_t offset, size_t end)
{
    std::vector<RawChunk> out;
    while (offset + 8 <= end) {
        const uint32_t id = Rd<uint32_t>(b, offset);
        const int32_t size = Rd<int32_t>(b, offset + 4);
        if (size < 0 || offset + 8 + static_cast<size_t>(size) > end) break;
        out.push_back({ id, offset + 8, static_cast<size_t>(size) });
        offset += 8 + static_cast<size_t>(size);
    }
    return out;
}

constexpr uint32_t kChunkSectionTable = 0x00034110;
constexpr uint32_t kChunkBarriers = 0x00034112;
constexpr uint32_t kChunkVisibility = 0x80034150;
constexpr uint32_t kChunkHeader = 0x00034151;
constexpr uint32_t kChunkLists = 0x00034153;
constexpr uint32_t kChunkTracks = 0x00034154;
constexpr uint32_t kChunkGroups = 0x00034155;

constexpr size_t kSectionStride = 0x50;
constexpr size_t kListStride = 0xA4;       // fixed: at most 72 sections per list
constexpr int kListMax = (int)((kListStride - 0x14) / 2);
constexpr size_t kTrackStride = 32;
constexpr size_t kGroupStride = 76;
constexpr int kUnionCap = 0x180;           // sub_5DFF50
constexpr uint64_t kReserve = 16 * 1024;   // "requested + 16 KiB > free"
constexpr uint32_t kPredictionType = 0x0C;

constexpr int kX0 = 2400, kY0 = 2500, kZ0 = 2600;
constexpr int kLetterX = 24, kLetterY = 25, kLetterZ = 26;

bool InPolygon(const std::vector<float>& xs, const std::vector<float>& ys, float px, float py)
{
    bool inside = false;
    const size_t n = xs.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        if ((ys[i] > py) != (ys[j] > py) && px < (xs[j] - xs[i]) * (py - ys[i]) / (ys[j] - ys[i]) + xs[i])
            inside = !inside;
    }
    return inside;
}

// [I] sub_5D4D50: proper 2D segment intersection.
bool SegmentsCross(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy)
{
    auto orient = [](double px, double py, double qx, double qy, double rx, double ry) {
        const double v = (qx - px) * (ry - py) - (qy - py) * (rx - px);
        return (v > 0) - (v < 0);
    };
    const int o1 = orient(ax, ay, bx, by, cx, cy), o2 = orient(ax, ay, bx, by, dx, dy);
    const int o3 = orient(cx, cy, dx, dy, ax, ay), o4 = orient(cx, cy, dx, dy, bx, by);
    return o1 * o2 < 0 && o3 * o4 < 0;
}

double SegDist(double px, double py, double ax, double ay, double bx, double by)
{
    const double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
    double t = l == 0 ? 0 : ((px - ax) * dx + (py - ay) * dy) / l;
    t = std::max(0.0, std::min(1.0, t));
    return std::hypot(px - ax - t * dx, py - ay - t * dy);
}

// adj.py's dist(A, B): the smallest distance from a vertex of A to an edge of B.
double VertexToEdges(const SectionBoundaryU2& a, const SectionBoundaryU2& b)
{
    double m = 1e9;
    const int n = b.Count();
    for (int p = 0; p < a.Count(); ++p)
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            m = std::min(m, SegDist(a.X[p], a.Y[p], b.X[i], b.Y[i], b.X[j], b.Y[j]));
        }
    return m;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

} // namespace

std::string StreamSectionName(int number)
{
    if (number <= 0) return "--";
    char text[16];
    std::snprintf(text, sizeof text, "%c%d", (char)(0x40 + number / 100), number % 100);
    return text;
}

int DropScore(int number, int base, bool loaded)
{
    const int letter = number / 100, n = number % 100;
    if (letter >= 1 && letter <= 20) {
        if (n >= 2 * base) return loaded ? 999 : 1000;
        if (n >= 1 && n < base) return 10;
        return -1;   // geometry sections base..2base-1 are never dropped
    }
    if (letter == kLetterX || letter == kLetterY) return n == 0 ? 1 : 2;
    return -1;       // Z, U..W
}

// ---------------------------------------------------------------------------------------------------------------

GameStreamingModel::GameStreamingModel(const std::vector<uint8_t>& layout,
                                       const std::vector<nfs::FreeRoamZone>& freeRoam, const std::wstring& region,
                                       const Speed2Image* exe)
{
    if (exe) {
        try {
            for (int i = 0; i < 3; ++i) k_.PoolKiB[i] = exe->U32(0x79DC54 + 4 * i);
            k_.PredictSeconds = exe->F32(0x7A47A4);
            k_.PredictMax = exe->F32(0x7A47A8);
            k_.PredictMaxSpeed = exe->F32(0x7A52F4);
            k_.FromExe = true;
        } catch (const std::exception& e) {
            LOG_WARN("game streaming: constants not read from SPEED2.EXE (%s), using v1.2 values", e.what());
            k_ = Constants{};
        }
    }
    std::wstring upper = region;
    for (wchar_t& c : upper) c = (wchar_t)towupper(c);
    // Index 1 is split screen (dword_89E7C0 == 2), index 2 the region L4RG.
    poolBytes_ = (uint64_t)k_.PoolKiB[upper == L"L4RG" ? 2 : 0] * 1024;

    ReadLayout(layout);

    for (const nfs::FreeRoamZone& z : freeRoam) {
        if (z.type != kPredictionType || z.polygon.size() < 3) continue;
        PredictionZone p;
        p.MinX = z.boundsMin.x; p.MinY = z.boundsMin.y;
        p.MaxX = z.boundsMax.x; p.MaxY = z.boundsMax.y;
        for (const Vec2& v : z.polygon) { p.X.push_back(v.x); p.Y.push_back(v.y); }
        // +0x30..+0x3C, which the generic reader names hash, cell x, cell y and extra.
        p.Zones[0] = (int32_t)z.hash;
        p.Zones[1] = z.cellX;
        p.Zones[2] = z.cellY;
        p.Zones[3] = (int32_t)z.extra;
        prediction_.push_back(std::move(p));
    }
}

void GameStreamingModel::ReadLayout(const std::vector<uint8_t>& b)
{
    for (const RawChunk& c : Chunks(b, 0, b.size())) {
        if (c.Id == kChunkSectionTable) {
            for (size_t i = 0; i + kSectionStride <= c.Size; i += kSectionStride) {
                const size_t e = c.Data + i;
                StreamSection s;
                s.Number = Rd<int16_t>(b, e + 0x08);
                s.TableIndex = (int)(i / kSectionStride);
                s.FullSize = Rd<uint32_t>(b, e + 0x18);
                s.PermanentSize = Rd<uint32_t>(b, e + 0x1C);
                s.OrderKey = Rd<uint32_t>(b, e + 0x20);
                s.CentreX = Rd<float>(b, e + 0x24);
                s.CentreY = Rd<float>(b, e + 0x28);
                s.Radius = Rd<float>(b, e + 0x2C);
                s.Name = StreamSectionName(s.Number);
                if (s.Number > 0 && !byNumber_.count(s.Number)) byNumber_[s.Number] = sections_.size();
                sections_.push_back(std::move(s));
            }
        } else if (c.Id == kChunkBarriers) {
            for (size_t i = 0; i + 16 <= c.Size; i += 16)
                barriers_.push_back({ Rd<float>(b, c.Data + i), Rd<float>(b, c.Data + i + 4),
                                      Rd<float>(b, c.Data + i + 8), Rd<float>(b, c.Data + i + 12) });
        } else if (c.Id == kChunkVisibility) {
            for (const RawChunk& s : Chunks(b, c.Data, c.Data + c.Size)) {
                if (s.Id == kChunkHeader && s.Size >= 4) {
                    const uint32_t base = Rd<uint32_t>(b, s.Data);
                    if (base > 0 && base < 50) base_ = (int)base;
                } else if (s.Id == kChunkLists) {
                    for (size_t i = 0; i + kListStride <= s.Size; i += kListStride) {
                        const size_t r = s.Data + i;
                        const int zone = Rd<int16_t>(b, r + 0x08);
                        const int count = std::clamp(Rd<int32_t>(b, r + 0x10), 0, kListMax);
                        std::vector<int16_t> list;
                        for (int k = 0; k < count; ++k) list.push_back(Rd<int16_t>(b, r + 0x14 + 2 * (size_t)k));
                        if (!lists_.count(zone)) listOrder_.push_back(zone);
                        lists_[zone] = std::move(list);
                    }
                } else if (s.Id == kChunkTracks) {
                    for (size_t i = 0; i + kTrackStride <= s.Size; i += kTrackStride) {
                        const size_t r = s.Data + i;
                        TrackSections t;
                        t.TrackId = Rd<int32_t>(b, r + 0x08);
                        const int count = std::clamp(Rd<int32_t>(b, r + 0x0C), 0, 8);
                        for (int k = 0; k < count; ++k) t.Sections.push_back(Rd<int16_t>(b, r + 0x10 + 2 * (size_t)k));
                        tracks_.push_back(std::move(t));
                    }
                } else if (s.Id == kChunkGroups) {
                    for (size_t i = 0; i + kGroupStride <= s.Size; i += kGroupStride) {
                        const size_t r = s.Data + i;
                        CcGroup g;
                        for (size_t k = 0; k < 16 && b[r + 8 + k]; ++k) g.Name += (char)b[r + 8 + k];
                        const int n1 = std::clamp((int)Rd<int16_t>(b, r + 0x18), 0, 16);
                        for (int k = 0; k < n1; ++k) g.Zones.push_back(Rd<int16_t>(b, r + 0x1A + 2 * (size_t)k));
                        const int n2 = std::clamp((int)Rd<int16_t>(b, r + 0x3A), 0, 8);
                        for (int k = 0; k < n2; ++k) g.Sections.push_back(Rd<int16_t>(b, r + 0x3C + 2 * (size_t)k));
                        groups_.push_back(std::move(g));
                    }
                }
            }
        }
    }
    outlines_ = VisibleSectionsU2::Read(b);
    // The ported reader leaves DrivableLimit at its default when 0x34151 is missing; keep the two in step.
    outlines_.DrivableLimit = base_;
    for (const SectionBoundaryU2& o : outlines_.Boundaries)
        if (IsDrivableZone(o.Section)) drivable_.push_back(o.Section);
}

std::vector<uint8_t> GameStreamingModel::EditedLayout(const nfs::World& world)
{
    const ByteSpan shipped = world.LayoutBytes();
    std::vector<uint8_t> layout(shipped.begin(), shipped.end());
    for (const SpanOverlay& span : world.Spans()) {
        if (!span.layoutFile || span.bytes.empty()) continue;
        const size_t end = (size_t)span.fileOffset + span.bytes.size();
        if (end > layout.size()) layout.resize(end, 0);
        std::copy(span.bytes.begin(), span.bytes.end(), layout.begin() + (std::ptrdiff_t)span.fileOffset);
    }
    return layout;
}

std::unique_ptr<GameStreamingModel> GameStreamingModel::FromWorld(nfs::World& world, const std::wstring& gameFolder,
                                                                  const Speed2Image* exe, std::wstring& error)
{
    if (!world.IsOpen()) { error = L"No region is open."; return nullptr; }
    std::vector<nfs::FreeRoamZone> freeRoam;
    if (!gameFolder.empty()) nfs::ReadFreeRoamZones(gameFolder, world.Region(), freeRoam);
    auto model = std::make_unique<GameStreamingModel>(EditedLayout(world), freeRoam, world.Region(), exe);
    if (model->lists_.empty() || model->drivable_.empty()) {
        error = L"The region has no streaming zones (chunk 0x80034150).";
        return nullptr;
    }
    model->AttachWorld(world);
    return model;
}

void GameStreamingModel::AttachWorld(nfs::World& world)
{
    for (StreamSection& s : sections_) s.PackHeaders.clear();
    models_.clear();
    for (const nfs::Section& entry : world.Table().Sections()) {
        const int number = (int16_t)entry.number;
        const auto at = byNumber_.find(number);
        if (number <= 0 || at == byNumber_.end()) continue;
        const nfs::LoadedSection* loaded = world.LoadSection(entry);
        if (!loaded) continue;
        StreamSection& s = sections_[at->second];
        for (const nfs::CollisionPack& pack : loaded->collision) s.PackHeaders.push_back((int16_t)pack.sectionNumber);
        std::vector<uint32_t>& hashes = models_[number];
        for (const nfs::SolidList& list : loaded->solidLists)
            for (const nfs::SolidObject& object : list.objects) hashes.push_back(object.hash);
        // Models the next save copies into the section (placement of a model from another section).
        for (uint32_t h : loaded->importedModels) hashes.push_back(h);
    }
    worldAttached_ = true;
}

const StreamSection* GameStreamingModel::Section(int number) const
{
    const auto at = byNumber_.find(number);
    return at == byNumber_.end() ? nullptr : &sections_[at->second];
}

const std::vector<int16_t>* GameStreamingModel::ListOf(int zone) const
{
    const auto at = lists_.find(zone);
    return at == lists_.end() ? nullptr : &at->second;
}

bool GameStreamingModel::IsDrivableZone(int number) const
{
    const int letter = number / 100, n = number % 100;
    return letter >= 1 && letter <= 20 && n >= 1 && n < base_;
}

std::unique_ptr<TopologyManagerU2> GameStreamingModel::NewFinder() const
{
    return std::make_unique<TopologyManagerU2>(outlines_, TopologyGroupsU2{});
}

int GameStreamingModel::ZoneAt(TopologyManagerU2& finder, float x, float y)
{
    const SectionBoundaryU2* b = finder.FindBoundary(x, y);
    return b ? b->Section : 0;
}

LoadingZoneResult GameStreamingModel::LoadingZone(TopologyManagerU2& finder, float x, float y, float vx,
                                                  float vy) const
{
    LoadingZoneResult r;
    r.PredX = x;
    r.PredY = y;
    bool taken = false;
    int takenZone = 0;
    float predX = x, predY = y;
    const float speed = std::sqrt(vx * vx + vy * vy);
    for (size_t i = 0; i < prediction_.size(); ++i) {
        const PredictionZone& p = prediction_[i];
        // [I] TrackPathManager::FindZone: box, then the polygon.
        if (x < p.MinX || x > p.MaxX || y < p.MinY || y > p.MaxY || !InPolygon(p.X, p.Y, x, y)) continue;
        if (r.PredictionZone < 0) r.PredictionZone = (int)i;
        float px = x, py = y;
        if (speed <= k_.PredictMaxSpeed) {
            float dx = vx * k_.PredictSeconds, dy = vy * k_.PredictSeconds;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > k_.PredictMax) { dx *= k_.PredictMax / len; dy *= k_.PredictMax / len; }
            px = x + dx;
            py = y + dy;
        }
        const int zone = ZoneAt(finder, px, py);
        r.PredX = px;
        r.PredY = py;
        r.PredictedZone = zone;
        // [I] unused slots are 0 and are not matched.
        if (zone != 0 && (zone == p.Zones[0] || zone == p.Zones[1] || zone == p.Zones[2] || zone == p.Zones[3])) {
            taken = true;
            takenZone = zone;
            predX = px;
            predY = py;
            r.PredictionZone = (int)i;
            break;
        }
    }
    if (taken && (predX != x || predY != y)) {
        for (const StreamBarrier& b : barriers_)
            if (SegmentsCross(x, y, predX, predY, b.X1, b.Y1, b.X2, b.Y2)) {
                r.BarrierCancelled = true;
                taken = false;
                break;
            }
    }
    r.ZoneAtCar = ZoneAt(finder, x, y);
    r.Zone = taken ? takenZone : r.ZoneAtCar;
    r.Predicted = taken && takenZone != r.ZoneAtCar;
    return r;
}

int GameStreamingModel::CollisionZone(TopologyManagerU2& finder, TopologyCoordinate* coordinate, float x, float y)
{
    if (!coordinate) return 0;
    finder.SetSectionPosition(coordinate, x, y, 0.0f);
    return coordinate->Boundary ? coordinate->Boundary->Section : 0;
}

const CcGroup* GameStreamingModel::GroupOf(int zone) const
{
    for (const CcGroup& g : groups_)
        for (int16_t z : g.Zones)
            if (z == zone) return &g;
    return nullptr;
}

std::vector<int16_t> GameStreamingModel::WantedSet(int zone, int trackId, const CcGroup** groupOut) const
{
    std::vector<int16_t> out;
    std::unordered_set<int> seen;
    auto add = [&](int number) {
        if (number <= 0 || !byNumber_.count(number) || !seen.insert(number).second) return;
        out.push_back((int16_t)number);
    };
    add(kX0);
    add(kY0);
    add(kZ0);
    if (trackId != 0)
        for (const TrackSections& t : tracks_)
            if (t.TrackId == trackId)
                for (int16_t s : t.Sections) add(s);
    const CcGroup* group = nullptr;
    if (zone > 0) {
        group = GroupOf(zone);
        if (group) {
            // sub_5DFF50: the union of every group zone's list, then the group's sections (a drivable id brings
            // its geometry section with it), at most 0x180 entries.
            int entries = 0;
            for (int16_t z : group->Zones)
                if (const auto* list = ListOf(z))
                    for (int16_t s : *list)
                        if (entries++ < kUnionCap) add(s);
            for (int16_t s : group->Sections) {
                if (entries++ < kUnionCap) add(s);
                if (IsDrivableZone(s) && entries++ < kUnionCap) add(s + base_);
            }
        } else if (const auto* list = ListOf(zone)) {
            for (int16_t s : *list) add(s);
        }
    }
    if (groupOut) *groupOut = group;
    return out;
}

uint64_t GameStreamingModel::SectionSize(int number, SizeModel model) const
{
    const StreamSection* s = Section(number);
    if (!s) return 0;
    return model == SizeModel::Full ? s->FullSize : s->PermanentSize;
}

uint64_t GameStreamingModel::TotalSize(const std::vector<int16_t>& numbers, SizeModel model) const
{
    uint64_t total = 0;
    for (int16_t n : numbers) total += SectionSize(n, model);
    return total;
}

PoolFitResult GameStreamingModel::PoolFit(const std::vector<int16_t>& wanted, SizeModel newModel,
                                          const std::unordered_set<int>* alreadyLoaded) const
{
    PoolFitResult r;
    r.PoolBytes = poolBytes_;
    std::vector<uint64_t> size(wanted.size());
    std::vector<bool> loaded(wanted.size());
    for (size_t i = 0; i < wanted.size(); ++i) {
        loaded[i] = alreadyLoaded && alreadyLoaded->count(wanted[i]);
        size[i] = SectionSize(wanted[i], loaded[i] ? SizeModel::Permanent : newModel);
        r.WantedBytes += size[i];
    }
    std::vector<bool> dropped(wanted.size(), false);
    uint64_t total = r.WantedBytes;
    while (total + kReserve > poolBytes_) {
        int best = -1, bestScore = -1;
        for (size_t i = 0; i < wanted.size(); ++i) {
            if (dropped[i]) continue;
            const int score = DropScore(wanted[i], base_, loaded[i]);
            if (score > bestScore) { bestScore = score; best = (int)i; }   // [I] first on a tie
        }
        if (best < 0) break;   // only never-dropped sections left: the rest is simply not loaded
        dropped[best] = true;
        total -= size[best];
        r.Dropped.push_back(wanted[best]);
    }
    for (size_t i = 0; i < wanted.size(); ++i)
        if (!dropped[i]) r.Kept.push_back(wanted[i]);
    r.KeptBytes = total;
    return r;
}

std::vector<int16_t> GameStreamingModel::DrawSet(int drawingZone, const std::vector<int16_t>& loaded) const
{
    std::vector<int16_t> out;
    const std::vector<int16_t>* list = drawingZone > 0 ? ListOf(drawingZone) : nullptr;
    for (int16_t n : loaded) {
        bool draw;
        if (drawingZone > 0)
            draw = n / 100 == kLetterZ || (list && std::find(list->begin(), list->end(), n) != list->end());
        else
            draw = n % 100 < 10;
        if (draw) out.push_back(n);
    }
    return out;
}

int GameStreamingModel::Slot(int number) const
{
    // sub_5D3C10, as TopologyManagerU2::Slot.
    if (number == 0) return 0;
    int n = number;
    const int rem = n % 100;
    if (rem >= base_ && rem < 2 * base_) n = (int16_t)(n - base_);
    const int letter = (int8_t)(uint8_t)(n / 100 + 0x40);
    return (n % 100) + (letter - 0x41) * 20;
}

CollisionPackStatus GameStreamingModel::CollisionPackFor(int zone, const std::vector<int16_t>& loaded) const
{
    CollisionPackStatus r;
    r.Zone = zone;
    if (zone <= 0) return r;
    r.Slot = Slot(zone);
    // [I] activation order: Y libraries and X0/Y0/Z0 (phase 2), X libraries (phase 4), then the rest by order
    // key (phase 6), the geometry sections base..2base-1 last (one activation per step after their read).
    std::vector<const StreamSection*> order;
    for (int16_t n : loaded)
        if (const StreamSection* s = Section(n)) order.push_back(s);
    auto phase = [&](const StreamSection* s) {
        const int letter = s->Number / 100, n = s->Number % 100;
        if (letter == kLetterY || s->Number == kX0 || s->Number == kZ0) return 0;
        if (letter == kLetterX) return 1;
        if (letter >= 1 && letter <= 20 && n >= base_ && n < 2 * base_) return 3;
        return 2;
    };
    std::stable_sort(order.begin(), order.end(), [&](const StreamSection* a, const StreamSection* b) {
        const int pa = phase(a), pb = phase(b);
        return pa != pb ? pa < pb : a->OrderKey < b->OrderKey;
    });
    for (const StreamSection* s : order)
        for (int16_t h : s->PackHeaders) {
            const CollisionPackRef ref{ s->Number, h, Slot(h) };
            const int letter = h / 100, n = h % 100;
            const bool folds = letter >= 1 && letter <= 20 && n >= 1 && n < 2 * base_;
            if (!folds) r.Aliases.push_back(ref);
            if (ref.Slot == r.Slot) r.Providers.push_back(ref);
        }
    return r;
}

std::unordered_set<uint32_t> GameStreamingModel::LoadedModels(const std::vector<int16_t>& loaded) const
{
    std::unordered_set<uint32_t> out;
    for (int16_t n : loaded) {
        const auto at = models_.find(n);
        if (at != models_.end()) out.insert(at->second.begin(), at->second.end());
    }
    return out;
}

GameStreamingModel::Tracker GameStreamingModel::NewTracker() const
{
    Tracker t;
    t.Loading = NewFinder();
    t.Drawing = NewFinder();
    t.Collision = NewFinder();
    t.Coordinate = t.Collision->CreateCoordinate();
    return t;
}

void GameStreamingModel::Update(Tracker& t, float carX, float carY, float velX, float velY, float camX, float camY,
                                StreamingState& out, const std::unordered_set<int>* alreadyLoaded) const
{
    out.Loading = LoadingZone(*t.Loading, carX, carY, velX, velY);
    out.DrawingZone = DrawingZone(*t.Drawing, camX, camY);
    out.CollisionZone = CollisionZone(*t.Collision, t.Coordinate, carX, carY);
    out.Wanted = WantedSet(out.Loading.Zone, 0, &out.Group);
    out.Fit = PoolFit(out.Wanted, SizeModel::Full, alreadyLoaded);
    out.Drawn = DrawSet(out.DrawingZone, out.Fit.Kept);
    out.Collision = CollisionPackFor(out.CollisionZone, out.Fit.Kept);
}

GameStreamingModel::AdjacencyAudit GameStreamingModel::AuditAdjacency(float maxDistance) const
{
    AdjacencyAudit audit;
    // adj.py keys the outlines by id (a later duplicate replaces an earlier one).
    std::map<int, const SectionBoundaryU2*> outl;
    for (const SectionBoundaryU2& o : outlines_.Boundaries)
        if (IsDrivableZone(o.Section) && o.Count() >= 1) outl[o.Section] = &o;
    struct Box { float x0, y0, x1, y1; };
    std::map<int, Box> box;
    for (const auto& [id, o] : outl) {
        Box b{ 1e30f, 1e30f, -1e30f, -1e30f };
        for (int i = 0; i < o->Count(); ++i) {
            b.x0 = std::min(b.x0, o->X[i]); b.y0 = std::min(b.y0, o->Y[i]);
            b.x1 = std::max(b.x1, o->X[i]); b.y1 = std::max(b.y1, o->Y[i]);
        }
        box[id] = b;
    }
    std::map<int, std::unordered_set<int>> wanted;
    for (const auto& [id, o] : outl) {
        (void)o;
        const auto w = WantedSet(id);
        wanted[id] = std::unordered_set<int>(w.begin(), w.end());
    }
    for (const auto& [a, oa] : outl)
        for (const auto& [b, ob] : outl) {
            if (a == b) continue;
            const Box& A = box[a];
            const Box& B = box[b];
            if (A.x0 > B.x1 + 2 || B.x0 > A.x1 + 2 || A.y0 > B.y1 + 2 || B.y0 > A.y1 + 2) continue;
            if (std::min(VertexToEdges(*oa, *ob), VertexToEdges(*ob, *oa)) >= maxDistance) continue;
            ++audit.Pairs;
            const auto* list = ListOf(a);
            const int geometry = b + base_;
            if (!list || std::find(list->begin(), list->end(), geometry) == list->end())
                audit.NotInList.push_back({ a, b });
            if (!wanted[a].count(geometry)) audit.NotWanted.push_back({ a, b });
        }
    return audit;
}

// ---------------------------------------------------------------------------------------------------------------
// Self-test (FRS_STREAM_SELFTEST=1)
// ---------------------------------------------------------------------------------------------------------------

namespace {

constexpr double kMiB = 1048576.0;
int g_fails = 0;

void Check(const char* what, bool ok, const char* detail = "")
{
    if (!ok) ++g_fails;
    LOG_INFO("stream selftest: [%s] %s %s", ok ? "pass" : "FAIL", what, detail);
}

void Report(const GameStreamingModel& m, const char* label, bool expectStudy)
{
    LOG_INFO("stream selftest: ---- %s layout ----", label);
    const auto& c = m.Consts();
    LOG_INFO("stream selftest: pool %u/%u/%u KiB (%s), this region %.1f MiB; predict %.2f s, max %.1f m, "
             "no prediction above %.2f m/s",
             c.PoolKiB[0], c.PoolKiB[1], c.PoolKiB[2], c.FromExe ? "SPEED2.EXE 0x79DC54" : "defaults",
             m.PoolBytes() / kMiB, c.PredictSeconds, c.PredictMax, c.PredictMaxSpeed);

    size_t longest = 0;
    for (const auto& [z, l] : m.Lists()) longest = std::max(longest, l.size());
    std::unordered_set<int> inGroups;
    int withSections = 0;
    for (const CcGroup& g : m.Groups()) {
        for (int16_t z : g.Zones) inGroups.insert(z);
        if (!g.Sections.empty()) ++withSections;
    }
    LOG_INFO("stream selftest: %zu table entries, base %d, %zu drivable outlines, %zu lists (longest %zu), "
             "%zu CC groups over %zu zones (%d with sections), %zu track records, %zu barriers, %zu prediction zones",
             m.Sections().size(), m.Base(), m.DrivableZones().size(), m.Lists().size(), longest, m.Groups().size(),
             inGroups.size(), withSections, m.Tracks().size(), m.Barriers().size(), m.PredictionZones().size());

    // Per zone: the study's figure (own list + X0/Y0/Z0, full size) and the game's wanted set (CC unions).
    struct Row { double mib; int zone; size_t count; };
    std::vector<Row> own, game, perm;
    int overPool = 0, dropping = 0;
    for (const auto& [zone, list] : m.Lists()) {
        std::vector<int16_t> set = { 2400, 2500, 2600 };
        for (int16_t s : list)
            if (m.Section(s) && std::find(set.begin(), set.end(), s) == set.end()) set.push_back(s);
        own.push_back({ m.TotalSize(set, SizeModel::Full) / kMiB, zone, set.size() });
        const auto wanted = m.WantedSet(zone);
        const double full = m.TotalSize(wanted, SizeModel::Full) / kMiB;
        game.push_back({ full, zone, wanted.size() });
        perm.push_back({ m.TotalSize(wanted, SizeModel::Permanent) / kMiB, zone, wanted.size() });
        if (full * kMiB > m.PoolBytes()) ++overPool;
        if (!m.PoolFit(wanted).Fits()) ++dropping;
    }
    auto byMib = [](const Row& a, const Row& b) { return a.mib < b.mib; };
    std::sort(own.begin(), own.end(), byMib);
    std::sort(game.begin(), game.end(), byMib);
    std::sort(perm.begin(), perm.end(), byMib);
    if (own.empty()) { Check("zone lists present", false); return; }
    LOG_INFO("stream selftest: own list + X0/Y0/Z0, full size: min %s %.1f MiB (%zu), max %s %.1f MiB (%zu)",
             StreamSectionName(own.front().zone).c_str(), own.front().mib, own.front().count,
             StreamSectionName(own.back().zone).c_str(), own.back().mib, own.back().count);
    std::string largest;
    for (size_t i = own.size() - std::min<size_t>(8, own.size()); i < own.size(); ++i) {
        char t[48];
        std::snprintf(t, sizeof t, " %s %.1f (%zu)", StreamSectionName(own[i].zone).c_str(), own[i].mib, own[i].count);
        largest += t;
    }
    LOG_INFO("stream selftest: largest 8:%s", largest.c_str());
    LOG_INFO("stream selftest: game wanted set (CC unions), full size: min %s %.1f MiB, max %s %.1f MiB (%zu "
             "sections); permanent size (+0x1C): min %s %.1f MiB, max %s %.1f MiB; zones over the pool %d, zones "
             "that drop sections %d",
             StreamSectionName(game.front().zone).c_str(), game.front().mib,
             StreamSectionName(game.back().zone).c_str(), game.back().mib, game.back().count,
             StreamSectionName(perm.front().zone).c_str(), perm.front().mib,
             StreamSectionName(perm.back().zone).c_str(), perm.back().mib, overPool, dropping);

    // Never listed (and not always loaded).
    std::unordered_set<int> listed = { 2400, 2500, 2600 };
    for (const auto& [z, l] : m.Lists()) listed.insert(l.begin(), l.end());
    std::string never;
    int neverCount = 0;
    for (const StreamSection& s : m.Sections())
        if (s.Number > 0 && !listed.count(s.Number)) { never += " " + s.Name; ++neverCount; }
    LOG_INFO("stream selftest: never listed (%d):%s", neverCount, never.c_str());

    const auto adj = m.AuditAdjacency(1.0f);
    std::string sample;
    for (size_t i = 0; i < adj.NotWanted.size() && i < 12; ++i)
        sample += " " + StreamSectionName(adj.NotWanted[i].first) + "->" + StreamSectionName(adj.NotWanted[i].second);
    LOG_INFO("stream selftest: adjacent outline pairs (< 1 m) %d; neighbour geometry not in own list %zu; not in "
             "the wanted set (CC unions) %zu; e.g.%s",
             adj.Pairs, adj.NotInList.size(), adj.NotWanted.size(), sample.c_str());

    // Prediction: from each prediction zone's vertices' centroid, aimed at each of its zones' outline centres.
    int tried = 0, predicted = 0, cancelled = 0;
    auto finder = m.NewFinder();
    for (const PredictionZone& p : m.PredictionZones()) {
        float cx = 0, cy = 0;
        for (size_t i = 0; i < p.X.size(); ++i) { cx += p.X[i]; cy += p.Y[i]; }
        cx /= (float)p.X.size();
        cy /= (float)p.Y.size();
        for (int z : p.Zones) {
            if (z <= 0) continue;
            const SectionBoundaryU2* target = nullptr;
            for (const SectionBoundaryU2& o : m.Outlines().Boundaries) if (o.Section == z) target = &o;
            if (!target) continue;
            const float tx = (target->MinX + target->MaxX) * 0.5f, ty = (target->MinY + target->MaxY) * 0.5f;
            const float dx = tx - cx, dy = ty - cy, len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0f) continue;
            ++tried;
            const auto r = m.LoadingZone(*finder, cx, cy, dx / len * 40.0f, dy / len * 40.0f);
            if (r.Predicted) ++predicted;
            if (r.BarrierCancelled) ++cancelled;
        }
    }
    LOG_INFO("stream selftest: prediction probes (40 m/s from each prediction zone towards its zones): %d tried, "
             "%d switched the loading zone early, %d cancelled by a barrier", tried, predicted, cancelled);

    if (!expectStudy) return;
    char d[160];
    Check("pool 60000 KiB", m.PoolBytes() == 60000ull * 1024);
    Check("base 20", m.Base() == 20);
    std::snprintf(d, sizeof d, "(%zu)", m.DrivableZones().size());
    Check("255 drivable outlines", m.DrivableZones().size() == 255, d);
    std::snprintf(d, sizeof d, "(%zu, longest %zu)", m.Lists().size(), longest);
    Check("255 lists, longest 68", m.Lists().size() == 255 && longest == 68, d);
    std::snprintf(d, sizeof d, "(%zu / %zu / %d)", m.Groups().size(), inGroups.size(), withSections);
    Check("107 CC groups, 160 zones, 10 with sections", m.Groups().size() == 107 && inGroups.size() == 160 &&
          withSections == 10, d);
    std::snprintf(d, sizeof d, "(%zu)", m.Barriers().size());
    Check("6 streaming barriers", m.Barriers().size() == 6, d);
    std::snprintf(d, sizeof d, "(%zu)", m.PredictionZones().size());
    Check("131 prediction zones", m.PredictionZones().size() == 131, d);
    std::snprintf(d, sizeof d, "(%s %.2f MiB)", StreamSectionName(own.back().zone).c_str(), own.back().mib);
    Check("peak zone C3 38.5 MiB", own.back().zone == 303 && std::fabs(own.back().mib - 38.5) < 0.05, d);
    std::snprintf(d, sizeof d, "(%s %.2f MiB)", StreamSectionName(own.front().zone).c_str(), own.front().mib);
    Check("lowest zone P17 32.7 MiB", own.front().zone == 1617 && std::fabs(own.front().mib - 32.7) < 0.05, d);
    std::snprintf(d, sizeof d, "(%d)", neverCount);
    Check("9 never-listed sections", neverCount == 9, d);
    std::snprintf(d, sizeof d, "(%d / %zu / %zu)", adj.Pairs, adj.NotInList.size(), adj.NotWanted.size());
    Check("1614 adjacent pairs, 356 not preloaded, 319 after CC unions",
          adj.Pairs == 1614 && adj.NotInList.size() == 356 && adj.NotWanted.size() == 319, d);
    Check("no zone over the pool", overPool == 0);
}

} // namespace

void RunGameStreamingSelfTest(nfs::World& world, const std::wstring& gameFolder)
{
    g_fails = 0;
    LOG_INFO("stream selftest: region %S, game folder %S", world.Region().c_str(), gameFolder.c_str());
    std::unique_ptr<Speed2Image> exe;
    try {
        exe = std::make_unique<Speed2Image>(gameFolder + L"\\SPEED2.EXE");
    } catch (const std::exception& e) {
        LOG_WARN("stream selftest: SPEED2.EXE not read: %s", e.what());
    }
    std::vector<nfs::FreeRoamZone> freeRoam;
    nfs::ReadFreeRoamZones(gameFolder, world.Region(), freeRoam);
    const bool l4ra = world.Region() == L"L4RA";

    const ByteSpan shipped = world.LayoutBytes();
    GameStreamingModel original(std::vector<uint8_t>(shipped.begin(), shipped.end()), freeRoam, world.Region(),
                                exe.get());
    Report(original, "shipped", l4ra);

    std::wstring error;
    auto edited = GameStreamingModel::FromWorld(world, gameFolder, exe.get(), error);
    if (!edited) {
        LOG_WARN("stream selftest: edited model not built: %S", error.c_str());
        return;
    }
    Report(*edited, "edited (as the editor will write it)", false);

    // Collision: every zone's pack in its own loaded set, by header number.
    int packs = 0, aliasPacks = 0, missing = 0, conflicts = 0;
    std::string missingList, conflictList;
    for (const StreamSection& s : edited->Sections()) {
        packs += (int)s.PackHeaders.size();
        for (int16_t h : s.PackHeaders) {
            const int n = h % 100, letter = h / 100;
            if (!(letter >= 1 && letter <= 20 && n >= 1 && n < 2 * edited->Base())) ++aliasPacks;
        }
    }
    for (int zone : edited->DrivableZones()) {
        const auto fit = edited->PoolFit(edited->WantedSet(zone));
        const auto status = edited->CollisionPackFor(zone, fit.Kept);
        if (status.Missing()) { ++missing; if (missing <= 12) missingList += " " + StreamSectionName(zone); }
        if (status.Conflict()) { ++conflicts; if (conflicts <= 12) conflictList += " " + StreamSectionName(zone); }
    }
    LOG_INFO("stream selftest: collision packs %d (%d with an aliasing header); zones whose pack is not in their "
             "own loaded set %d:%s; zones with two packs on their slot %d:%s",
             packs, aliasPacks, missing, missingList.c_str(), conflicts, conflictList.c_str());
    if (l4ra) {
        char d[96];
        std::snprintf(d, sizeof d, "(%d packs, %d aliasing, %d missing, %d conflicts)", packs, aliasPacks, missing,
                      conflicts);
        Check("collision: every zone has exactly one pack of its own", missing == 0 && conflicts == 0, d);
    }

    // Zone lookups at each drivable outline's first-vertex-inset point and outside the city.
    auto finder = edited->NewFinder();
    int found = 0, total = 0;
    for (const SectionBoundaryU2& o : edited->Outlines().Boundaries) {
        if (!edited->IsDrivableZone(o.Section) || o.Count() < 3) continue;
        // The centroid of the vertices, kept only when the outline holds it.
        float cx = 0, cy = 0;
        for (int i = 0; i < o.Count(); ++i) { cx += o.X[i]; cy += o.Y[i]; }
        cx /= (float)o.Count();
        cy /= (float)o.Count();
        if (!TopologyManagerU2::Contains(o, cx, cy)) continue;
        ++total;
        if (GameStreamingModel::ZoneAt(*finder, cx, cy) == o.Section) ++found;
    }
    LOG_INFO("stream selftest: zone lookup at outline centroids: %d of %d give their own outline (overlaps take "
             "the MRU one)", found, total);
    const int outside = GameStreamingModel::ZoneAt(*finder, 1.0e6f, 1.0e6f);
    Check("far outside every outline is zone 0", outside == 0);
    const auto empty = edited->WantedSet(0);
    Check("zone 0 wants only X0/Y0/Z0", empty.size() <= 3);

    // The drawing rule.
    if (const auto* list = edited->ListOf(303)) {
        const auto fit = edited->PoolFit(edited->WantedSet(303));
        const auto drawn = edited->DrawSet(303, fit.Kept);
        const auto drawnElsewhere = edited->DrawSet(0, fit.Kept);
        LOG_INFO("stream selftest: C3 loads %zu, draws %zu from C3, %zu with the camera outside every outline "
                 "(list %zu); %zu model hashes resident", fit.Kept.size(), drawn.size(), drawnElsewhere.size(),
                 list->size(), edited->LoadedModels(fit.Kept).size());
    }
    LOG_INFO("stream selftest: done, %d failure(s)", g_fails);
}

} // namespace bb
