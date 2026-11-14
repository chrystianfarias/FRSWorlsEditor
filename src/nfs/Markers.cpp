#include "nfs/Markers.h"

#include "core/Log.h"
#include "nfs/World.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace nfs {

namespace {

constexpr u32 kCareer         = 0x80034A10;
constexpr u32 kCareerRaces    = 0x00034A11;
constexpr u32 kCareerShops    = 0x00034A12;
constexpr u32 kCareerStrings  = 0x00034A1D;

constexpr u32 kZoneRaceTrigger = 0x0D;
constexpr u32 kZoneShop        = 0x0E;

constexpr size_t kRaceRecord  = 136;
constexpr size_t kShopRecord  = 160;

std::vector<u8> ReadWholeFile(const std::wstring& path) {
    std::vector<u8> bytes;
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return bytes;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
        bytes.resize((size_t)size);
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
    }
    std::fclose(file);
    return bytes;
}

template <typename T> T Read(const std::vector<u8>& bytes, size_t at) {
    T value{};
    if (at + sizeof(T) <= bytes.size()) std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

// Payload offset and size of each chunk in [begin, end). These files hold no
// alignment filler, so the chunks follow one another directly.
struct Span { u32 id; size_t data; size_t size; };
std::vector<Span> Chunks(const std::vector<u8>& bytes, size_t begin, size_t end) {
    std::vector<Span> out;
    size_t at = begin;
    while (at + 8 <= end) {
        const u32 id = Read<u32>(bytes, at);
        const u32 size = Read<u32>(bytes, at + 4);
        if (at + 8 + size > end) break;
        out.push_back({ id, at + 8, size });
        at += 8 + size;
    }
    return out;
}

// The hash the career's string table uses for trigger names.
u32 BinHash(const char* text) {
    u32 h = 0xFFFFFFFF;
    for (; *text; ++text) h = h * 33 + (u8)*text;
    return h;
}

MarkerKind KindForScene(const std::string& scene) {
    if (scene.rfind("PAINTSHOP", 0) == 0) return MarkerKind::Graphics;
    if (scene.rfind("PARTSSHOP", 0) == 0) return MarkerKind::BodyShop;
    if (scene.rfind("PERFSHOP", 0) == 0)  return MarkerKind::Performance;
    if (scene.rfind("AUDIOSHOP", 0) == 0) return MarkerKind::CarSpecialties;
    if (scene.rfind("CARLOT", 0) == 0)    return MarkerKind::CarLot;
    if (scene.rfind("CRIB", 0) == 0)      return MarkerKind::Garage;
    return MarkerKind::OtherShop;
}

// The kind tokens of the marker file.
const char* StyleToken(MarkerKind kind) {
    switch (kind) {
        case MarkerKind::Graphics:       return "GRAPHICS";
        case MarkerKind::BodyShop:       return "BODYSHOP";
        case MarkerKind::Performance:    return "PERFORMANCE";
        case MarkerKind::CarSpecialties: return "SPECIALTIES";
        case MarkerKind::CarLot:         return "CARLOT";
        case MarkerKind::Garage:         return "GARAGE";
        case MarkerKind::RaceStart:      return "RACE";
        default:                         return "BODYSHOP";
    }
}

bool StyleFromToken(const char* token, MarkerKind& kind) {
    for (MarkerKind placeable : kPlaceableKinds)
        if (std::strcmp(token, StyleToken(placeable)) == 0) {
            kind = placeable;
            return true;
        }
    return false;
}

float HeadingFromFacing(const Vec2& facing) {
    if (facing.x == 0.0f && facing.y == 0.0f) return 0.0f;
    return Degrees(std::atan2(facing.y, facing.x));
}

Mat4 TurnAboutZ(float degrees) {
    const float a = Radians(degrees), c = std::cos(a), s = std::sin(a);
    Mat4 r = Mat4::Identity();
    r.m[0][0] = c;  r.m[0][1] = s;
    r.m[1][0] = -s; r.m[1][1] = c;
    return r;
}

} // namespace

const char* MarkerEffectPrefix(MarkerKind kind) {
    switch (kind) {
        case MarkerKind::Graphics:       return "GRAPHICS_";
        case MarkerKind::BodyShop:       return "BODYSHOP_";
        case MarkerKind::Performance:    return "PERFORMANCE_";
        case MarkerKind::CarSpecialties: return "SPECIALTIES_";
        case MarkerKind::CarLot:         return "CARLOT_";
        case MarkerKind::Garage:         return "GARAGE_";
        default:                         return nullptr;
    }
}

const wchar_t* MarkerKindName(MarkerKind kind) {
    switch (kind) {
        case MarkerKind::Graphics:       return L"Graphics shop";
        case MarkerKind::BodyShop:       return L"Body shop";
        case MarkerKind::Performance:    return L"Performance shop";
        case MarkerKind::CarSpecialties: return L"Car specialties";
        case MarkerKind::CarLot:         return L"Car lot";
        case MarkerKind::Garage:         return L"Garage";
        case MarkerKind::OtherShop:      return L"Shop";
        case MarkerKind::RaceStart:      return L"Race start";
        case MarkerKind::Placed:         return L"Placed marker";
    }
    return L"?";
}

const char* MarkerKindIcon(MarkerKind kind) {
    switch (kind) {
        case MarkerKind::Graphics:       return "SHOP_ICON_GRAPHIC";
        case MarkerKind::BodyShop:       return "SHOP_ICON_BODY";
        case MarkerKind::Performance:    return "SHOP_ICON_PERFORMANCE";
        case MarkerKind::CarSpecialties: return "SHOP_ICON_INTERIORS";
        case MarkerKind::CarLot:         return "SHOP_ICON_CAR_LOT";
        case MarkerKind::Garage:         return "SHOP_ICON_CRIB";
        case MarkerKind::RaceStart:      return "MINIMAP_ICON_CIRCUIT";
        default:                         return nullptr;
    }
}

u32 MarkerKindColour(MarkerKind kind) {
    switch (kind) {
        // The hues of the game's own rings and smoke.
        case MarkerKind::Graphics:       return 0xFF4848;
        case MarkerKind::BodyShop:       return 0x48E060;
        case MarkerKind::Performance:    return 0x4888FF;
        case MarkerKind::CarSpecialties: return 0xFFE048;
        case MarkerKind::CarLot:         return 0x48E0F0;
        case MarkerKind::Garage:         return 0xB458FF;
        case MarkerKind::OtherShop:      return 0xA0A0A0;
        case MarkerKind::RaceStart:      return 0xFF9A20;
        case MarkerKind::Placed:         return 0x00FFFF;
    }
    return 0xFFFFFF;
}

Mat4 PlacedDoorMatrix(const Mat4& templateDoor, const Marker& shop, const Marker& placed) {
    // As it stood to its shop: about the shop's centre, by the difference in
    // heading, onto the marker.
    Mat4 door = templateDoor * Mat4::Translation({ -shop.position.x, -shop.position.y, -shop.position.z }) *
                TurnAboutZ(placed.heading - shop.heading) * Mat4::Translation(placed.position);
    // Then the marker's own adjustment: turned about the door's own place, and
    // moved in the marker's frame.
    const Vec3 at = { door.m[3][0], door.m[3][1], door.m[3][2] };
    const float h = Radians(placed.heading);
    const Vec3 forward = { std::cos(h), std::sin(h), 0.0f }, right = { std::sin(h), -std::cos(h), 0.0f };
    const Vec3 moved = at + forward * placed.doorForward + right * placed.doorRight +
                       Vec3{ 0.0f, 0.0f, placed.doorUp };
    return door * Mat4::Translation({ -at.x, -at.y, -at.z }) * TurnAboutZ(placed.doorTurn) * Mat4::Translation(moved);
}

void MarkerSet::Clear() {
    zones_.clear();
    markers_.clear();
    careerRaces_.clear();
    keptLines_.clear();
    placedChanged_ = false;
}

bool MarkerSet::Load(const std::wstring& gameFolder, const std::wstring& region) {
    Clear();
    gameFolder_ = gameFolder;
    region_ = region;

    if (!ReadFreeRoamZones(gameFolder, region, zones_)) {
        LOG_INFO("markers: no free-roam zones for %S", region.c_str());
        LoadPlaced();
        return false;
    }
    if (!LoadCareer(gameFolder + L"\\GLOBAL\\GLOBALB.BUN"))
        LOG_WARN("markers: could not read the career in GLOBALB.BUN; zones are shown unnamed");
    LoadPlaced();

    size_t shops = 0, races = 0;
    for (const Marker& marker : markers_) {
        if (marker.kind == MarkerKind::RaceStart) ++races;
        else if (!marker.IsPlaced()) ++shops;
    }
    LOG_INFO("markers: %zu zones, %zu shops, %zu race starts, %zu placed", zones_.size(), shops, races,
             PlacedCount());
    return true;
}

bool MarkerSet::LoadCareer(const std::wstring& path) {
    const std::vector<u8> bytes = ReadWholeFile(path);
    if (bytes.empty()) return false;

    std::map<u32, size_t> zoneByHash;
    for (size_t i = 0; i < zones_.size(); ++i)
        if (zones_[i].hash) zoneByHash.emplace(zones_[i].hash, i);

    const auto fromZone = [this](Marker& marker, size_t index) {
        const FreeRoamZone& zone = zones_[index];
        marker.zone     = (int)index;
        marker.hash     = zone.hash;
        marker.position = { zone.centre.x, zone.centre.y, 0.0f };
        marker.heading  = HeadingFromFacing(zone.facing);
        marker.radius   = zone.radius;
        marker.polygon  = zone.polygon;
    };

    for (const Span& root : Chunks(bytes, 0, bytes.size())) {
        if (root.id != kCareer) continue;

        // The first career chunk is the full career; later ones are subsets.
        Span strings{}, races{}, shops{};
        for (const Span& child : Chunks(bytes, root.data, root.data + root.size)) {
            if (child.id == kCareerStrings) strings = child;
            if (child.id == kCareerRaces)   races = child;
            if (child.id == kCareerShops)   shops = child;
        }

        // Shops: 160 bytes each. Name at +0, the zone's hash at +0x3C, the
        // front-end scene at +0x40, a type at +0x50 and a stage word at +0x9C.
        for (size_t at = shops.data; shops.size && at + kShopRecord <= shops.data + shops.size;
             at += kShopRecord) {
            Marker marker;
            marker.name      = std::string((const char*)bytes.data() + at, ::strnlen((const char*)bytes.data() + at, 0x3C));
            marker.hash      = Read<u32>(bytes, at + 0x3C);
            marker.sceneFile = std::string((const char*)bytes.data() + at + 0x40, ::strnlen((const char*)bytes.data() + at + 0x40, 16));
            marker.shopType  = Read<u32>(bytes, at + 0x50);
            marker.stage     = Read<u32>(bytes, at + 0x9C);
            marker.kind      = KindForScene(marker.sceneFile);
            const auto found = zoneByHash.find(marker.hash);
            // A shop without a zone -- the Uniques shop -- is reached from a
            // menu, not driven to.
            if (found == zoneByHash.end() || zones_[found->second].type != kZoneShop) continue;
            fromZone(marker, found->second);
            markers_.push_back(std::move(marker));
        }

        // Races: 136 bytes each. The name is an offset into the string table
        // at +0, the start trigger's hash at +0x28. Several races share a start.
        std::map<u32, std::string> names;
        if (strings.size) {
            const char* text = (const char*)bytes.data() + strings.data;
            for (size_t i = 0; i < strings.size;) {
                const size_t length = ::strnlen(text + i, strings.size - i);
                if (length) names.emplace(BinHash(text + i), std::string(text + i, length));
                i += length + 1;
            }
        }
        std::map<u32, size_t> startByHash;
        for (size_t at = races.data; races.size && at + kRaceRecord <= races.data + races.size;
             at += kRaceRecord) {
            const u32 nameOffset = Read<u32>(bytes, at);
            const u32 trigger = Read<u32>(bytes, at + 0x28);
            std::string raceName;
            if (nameOffset < strings.size) {
                const char* text = (const char*)bytes.data() + strings.data + nameOffset;
                raceName.assign(text, ::strnlen(text, strings.size - nameOffset));
            }
            // The few records with a hash where the name's offset goes are
            // known by it (io/MarkerWrite finds them the same way).
            {
                char hex[16];
                std::snprintf(hex, sizeof(hex), "%08X", nameOffset);
                const std::string known = raceName.empty() ? std::string(hex) : raceName;
                if (std::find(careerRaces_.begin(), careerRaces_.end(), known) == careerRaces_.end())
                    careerRaces_.push_back(known);
            }
            const auto zone = zoneByHash.find(trigger);
            if (zone == zoneByHash.end() || zones_[zone->second].type != kZoneRaceTrigger) continue;

            auto start = startByHash.find(trigger);
            if (start == startByHash.end()) {
                Marker marker;
                marker.kind = MarkerKind::RaceStart;
                const auto name = names.find(trigger);
                char fallback[16];
                std::snprintf(fallback, sizeof(fallback), "%08X", trigger);
                marker.name = name != names.end() ? name->second : fallback;
                fromZone(marker, zone->second);
                start = startByHash.emplace(trigger, markers_.size()).first;
                markers_.push_back(std::move(marker));
            }
            markers_[start->second].races.push_back(raceName.empty() ? "(unnamed)" : raceName);
        }
        return true;
    }
    return false;
}

std::wstring MarkerSet::PlacedPath() const {
    return gameFolder_ + L"\\models\\markers-" + region_ + L".frsmarkers";
}

size_t MarkerSet::PlacedCount() const {
    size_t count = 0;
    for (const Marker& marker : markers_) count += marker.IsPlaced();
    return count;
}

// One marker a line:
//   marker <name> <x> <y> <z> <heading> <radius> <RRGGBB> <kind> <shop copied, or -> <SHOP>
//          | | | | | <races, for RACE> | <door: forward right up turn>
// The empty fields between the bars are kept for older and other builds of the
// editor, which put there what this one does not place; a line whose action is
// not SHOP is one of those markers, and is kept as it is.
void MarkerSet::LoadPlaced() {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, PlacedPath().c_str(), L"rt") != 0 || !file) return;

    char line[512];
    int number = 0;
    while (std::fgets(line, sizeof(line), file)) {
        ++number;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char name[128] = {}, style[32] = {}, opens[128] = {}, action[16] = {};
        float x, y, z, heading, radius;
        unsigned colour = 0;
        if (std::sscanf(line, "marker %127s %f %f %f %f %f %x %31s %127s %15s", name, &x, &y, &z,
                        &heading, &radius, &colour, style, opens, action) < 8) {
            LOG_WARN("markers: line %d of the marker file is not a marker, skipped", number);
            continue;
        }
        Marker marker;
        marker.kind = MarkerKind::Placed;
        const bool known = StyleFromToken(style, marker.style);
        // Not a shop copy or a race start: a marker of another kind, kept in
        // the file and nothing more.
        if (!known || (marker.style != MarkerKind::RaceStart && std::strcmp(action, "SHOP") != 0)) {
            std::string kept = line;
            while (!kept.empty() && (kept.back() == '\n' || kept.back() == '\r')) kept.pop_back();
            keptLines_.push_back(kept);
            continue;
        }
        marker.name = name;
        marker.position = { x, y, z };
        marker.heading = heading;
        marker.radius = radius;
        if (marker.style != MarkerKind::RaceStart && FindShop(opens)) marker.opens = opens;

        // The fields after the bars: the fifth is the races, the sixth the door.
        std::string races, door;
        if (const char* bar = std::strstr(line, " | ")) {
            std::string rest = bar + 3;
            while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.pop_back();
            std::string unused;
            std::string* parts[] = { &unused, &unused, &unused, &unused, &races, &door };
            for (std::string* part : parts) {
                const size_t next = rest.find(" | ");
                *part = rest.substr(0, next);
                if (next == std::string::npos) break;
                rest = rest.substr(next + 3);
            }
        }
        std::sscanf(door.c_str(), "%f %f %f %f", &marker.doorForward, &marker.doorRight, &marker.doorUp,
                    &marker.doorTurn);
        for (size_t at = 0; at < races.size();) {
            size_t comma = races.find(',', at);
            if (comma == std::string::npos) comma = races.size();
            std::string race = races.substr(at, comma - at);
            while (!race.empty() && race.back() == ' ') race.pop_back();
            while (!race.empty() && race.front() == ' ') race.erase(race.begin());
            if (!race.empty()) marker.races.push_back(race);
            at = comma + 1;
        }
        markers_.push_back(std::move(marker));
    }
    std::fclose(file);
    if (!keptLines_.empty())
        LOG_INFO("markers: %zu markers of the file are of a kind this editor does not place; kept, not shown",
                 keptLines_.size());
}

bool MarkerSet::SavePlaced() {
    const std::wstring path = PlacedPath();
    if (PlacedCount() == 0 && keptLines_.empty()) {
        ::DeleteFileW(path.c_str());
        placedChanged_ = false;
        return true;
    }

    ::CreateDirectoryW((gameFolder_ + L"\\models").c_str(), nullptr);
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wt") != 0 || !file) return false;
    std::fprintf(file, "# FRS World Editor markers for %S\n", region_.c_str());
    std::fprintf(file, "# marker <name> <x> <y> <z> <heading> <radius> <RRGGBB> <kind> <shop copied, or -> SHOP | | | | | <races, for RACE> | <door: forward right up turn>\n");
    std::fprintf(file, "# kind: GRAPHICS BODYSHOP PERFORMANCE SPECIALTIES CARLOT GARAGE RACE\n");
    for (const Marker& marker : markers_) {
        if (!marker.IsPlaced()) continue;
        std::string races;
        for (const std::string& race : marker.races) races += (races.empty() ? "" : ",") + race;
        std::fprintf(file, "marker %s %.3f %.3f %.3f %.2f %.2f %06X %s %s SHOP |  |  |  |  | %s | %.3f %.3f %.3f %.2f\n",
                     marker.name.c_str(), marker.position.x, marker.position.y, marker.position.z,
                     marker.heading, marker.radius, MarkerKindColour(marker.style), StyleToken(marker.style),
                     marker.opens.empty() ? "-" : marker.opens.c_str(), races.c_str(), marker.doorForward,
                     marker.doorRight, marker.doorUp, marker.doorTurn);
    }
    for (const std::string& kept : keptLines_) std::fprintf(file, "%s\n", kept.c_str());
    const bool ok = std::fclose(file) == 0;
    if (ok) placedChanged_ = false;
    return ok;
}

size_t MarkerSet::AddPlaced(MarkerKind kind, const Vec3& position, float heading) {
    // FRS_BODYSHOP_1, FRS_RACE_2 ...: the first free number of the kind.
    const std::string prefix = std::string("FRS_") + StyleToken(kind) + "_";
    std::string name;
    for (int n = 1;; ++n) {
        name = prefix + std::to_string(n);
        bool used = false;
        for (const Marker& marker : markers_) used |= marker.name == name;
        // Names in the kept lines count as taken too.
        for (const std::string& kept : keptLines_) used |= kept.find(" " + name + " ") != std::string::npos;
        if (!used) break;
    }
    Marker marker;
    marker.kind = MarkerKind::Placed;
    marker.style = kind;
    marker.name = name;
    marker.position = position;
    marker.heading = heading;
    if (kind == MarkerKind::RaceStart) {
        marker.radius = 10.0f;           // about a shipped race trigger's
    } else {
        marker.radius = 12.0f;           // about a shop's
        if (const Marker* shop = FirstShopOfKind(kind)) marker.opens = shop->name;
    }
    markers_.push_back(std::move(marker));
    placedChanged_ = true;
    return markers_.size() - 1;
}

bool MarkerSet::RemovePlaced(size_t index) {
    if (index >= markers_.size() || !markers_[index].IsPlaced()) return false;
    markers_.erase(markers_.begin() + (std::ptrdiff_t)index);
    placedChanged_ = true;
    return true;
}

const Marker* MarkerSet::FindShop(const std::string& name) const {
    for (const Marker& marker : markers_)
        if (!marker.IsPlaced() && marker.kind != MarkerKind::RaceStart && marker.name == name) return &marker;
    return nullptr;
}

const Marker* MarkerSet::FirstShopOfKind(MarkerKind kind) const {
    // The one that opens earliest in the career.
    const Marker* best = nullptr;
    for (const Marker& marker : markers_)
        if (!marker.IsPlaced() && marker.kind == kind && (!best || marker.stage < best->stage)) best = &marker;
    return best;
}

void MarkerSet::PlaceOnGround(const World& world) {
    size_t missing = 0;
    for (Marker& marker : markers_) {
        if (marker.IsPlaced()) continue;   // they keep the height they were given
        float z = 0.0f;
        if (GroundHeight(world, marker.position.x, marker.position.y, z)) marker.position.z = z;
        else ++missing;
    }
    if (missing) LOG_INFO("markers: %zu have no collision under them, left at z = 0", missing);
}

} // namespace nfs
