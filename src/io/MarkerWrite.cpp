#include "io/MarkerWrite.h"

#include "core/Log.h"
#include "nfs/Jdlz.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace io {

namespace {

constexpr u32 kFreeRoamRoot   = 0x80034147;
constexpr u32 kFreeRoamZones  = 0x0003414A;
constexpr u32 kTrackPositions = 0x00034146;
constexpr u32 kCareer         = 0x80034A10;
constexpr u32 kCareerShops    = 0x00034A12;
constexpr u32 kZoneShop       = 0x0E;
constexpr u32 kZoneRaceTrigger = 0x0D;
constexpr u32 kCareerRaces    = 0x00034A11;
constexpr u32 kCareerStrings  = 0x00034A1D;
constexpr size_t kRaceRecord  = 136;
constexpr size_t kRaceTrigger = 0x28;   // the start trigger's zone hash

constexpr size_t kShopRecord     = 160;
constexpr size_t kShopNameBytes  = 0x38;   // the name, zero padded
constexpr size_t kShopNameHash   = 0x38;   // binhash of the name
constexpr size_t kShopZoneHash   = 0x3C;
// The record of a shop placed in the editor, for the loader (loader/src/Markers.cpp).
constexpr size_t kShopPlacedMagic = 0x54;
// "FRSP": the loader counts the shop as discovered, so it is in the career's
// event table -- the prompt, the minimap and the world map all come from there --
// and opens as the shop it copies.
constexpr u32    kPlacedMagic     = 0x50535246;
constexpr size_t kPositionRecord = 48;

// What follows a grown career chunk moves by a whole number of these, so any
// alignment the file's later chunks were laid out to survives the insertion.
constexpr size_t kShiftUnit = 0x8000;

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

bool WriteWholeFile(const std::wstring& path, const std::vector<u8>& bytes) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && ok;
}

template <typename T> void Put(std::vector<u8>& out, T value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

u32 Get32(const std::vector<u8>& bytes, size_t at) {
    u32 value = 0;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

void Set32(std::vector<u8>& bytes, size_t at, u32 value) { std::memcpy(bytes.data() + at, &value, 4); }
void SetF(std::vector<u8>& bytes, size_t at, float value) { std::memcpy(bytes.data() + at, &value, 4); }

u32 BinHash(const std::string& text) {
    u32 h = 0xFFFFFFFF;
    for (char c : text) h = h * 33 + (u8)c;
    return h;
}

// The hash a placed marker's zone goes by. The shipped shops use hashes of some
// name that is not their own; any value unique among the zones does the job,
// since all that matters is that the shop record and its zone agree.
u32 ZoneHash(const std::string& name) { return BinHash("FRS_ZONE_" + name); }

// One shop zone laid out as the shipped ones are: a square footprint turned to
// the zone's facing, a little wider than the radius, as theirs are.
std::vector<u8> ShopZone(const nfs::Marker& marker, u32 hash, u32 type = kZoneShop) {
    const float h = Radians(marker.heading);
    const Vec2 facing = { std::cos(h), std::sin(h) };
    const Vec2 side = { -facing.y, facing.x };
    const float half = marker.radius * 0.54f;
    const Vec2 c = { marker.position.x, marker.position.y };

    Vec2 corners[4];
    const float signs[4][2] = { { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 } };
    Vec2 lo = c, hi = c;
    for (int k = 0; k < 4; ++k) {
        corners[k] = { c.x + (facing.x * signs[k][0] + side.x * signs[k][1]) * half,
                       c.y + (facing.y * signs[k][0] + side.y * signs[k][1]) * half };
        lo = { std::fmin(lo.x, corners[k].x), std::fmin(lo.y, corners[k].y) };
        hi = { std::fmax(hi.x, corners[k].x), std::fmax(hi.y, corners[k].y) };
    }

    std::vector<u8> out;
    Put<u32>(out, type);
    Put<float>(out, c.x);
    Put<float>(out, c.y);
    Put<float>(out, facing.x);
    Put<float>(out, facing.y);
    // The zone's height: on the shipped zones it follows the ground under
    // them (10 to 61) while their footprints stay the same size.
    Put<float>(out, marker.position.z);
    Put<u32>(out, type == kZoneShop ? 1u : 0u);   // set on every shipped shop, on few race triggers
    Put<u32>(out, 0);
    Put<float>(out, lo.x);
    Put<float>(out, lo.y);
    Put<float>(out, hi.x);
    Put<float>(out, hi.y);
    Put<u32>(out, hash);
    // Where the GPS leads, x, y, z in whole units (SPEED2.EXE 0x529410 and
    // 0x529480 route to it): a point on the street, 13-115 units from the
    // shipped shops, whose doors are off the road. A marker stands on the
    // street, so its own position.
    Put<i32>(out, (i32)std::lround(marker.position.x));
    Put<i32>(out, (i32)std::lround(marker.position.y));
    Put<i32>(out, (i32)std::lround(marker.position.z));
    Put<u16>(out, 4);
    Put<u16>(out, (u16)(0x44 + 4 * 8));
    for (const Vec2& corner : corners) {
        Put<float>(out, corner.x);
        Put<float>(out, corner.y);
    }
    return out;
}

// A placed shop and the shipped shop it copies.
struct PlacedShop {
    const nfs::Marker* marker;
    const nfs::Marker* shop;
};

// PathsFreeRoam.bin: zone records of one `type` put in 0x3414A where that
// type's run ends, for the reason BuildZones gives.
bool InsertZones(std::vector<u8>& bytes, u32 type, const std::vector<u8>& added, std::wstring& message) {
    if (added.empty()) return true;
    if (bytes.size() < 16 || Get32(bytes, 0) != kFreeRoamRoot) {
        message = L"PathsFreeRoam.bin does not start with its root chunk.";
        return false;
    }
    const size_t rootEnd = 8 + (size_t)Get32(bytes, 4);
    size_t at = 8, header = 0;
    while (at + 8 <= rootEnd) {
        if (Get32(bytes, at) == kFreeRoamZones) { header = at; break; }
        at += 8 + Get32(bytes, at + 4);
    }
    if (!header) {
        message = L"PathsFreeRoam.bin has no zone chunk (0x3414A).";
        return false;
    }
    const size_t zonesEnd = header + 8 + Get32(bytes, header + 4);
    size_t insertAt = zonesEnd;
    for (size_t record = header + 8; record + 0x44 <= zonesEnd;) {
        if (Get32(bytes, record) > type) { insertAt = record; break; }
        u16 size = 0;
        std::memcpy(&size, bytes.data() + record + 0x42, 2);
        if (size < 0x44) break;
        record += size;
    }
    bytes.insert(bytes.begin() + (std::ptrdiff_t)insertAt, added.begin(), added.end());
    Set32(bytes, header + 4, Get32(bytes, header + 4) + (u32)added.size());
    Set32(bytes, 4, Get32(bytes, 4) + (u32)added.size());
    return true;
}

// PathsFreeRoam.bin: a race trigger (type 0x0D) per race start placed in the
// editor. The shipped ones are the same record as a shop's, without the GPS
// point's flag; the career's races find them by the zone hash.
bool BuildRaceZones(std::vector<u8>& bytes, const std::vector<const nfs::Marker*>& starts, std::wstring& message) {
    std::vector<u8> added;
    for (const nfs::Marker* start : starts) {
        const std::vector<u8> zone = ShopZone(*start, ZoneHash(start->name), kZoneRaceTrigger);
        added.insert(added.end(), zone.begin(), zone.end());
    }
    return InsertZones(bytes, kZoneRaceTrigger, added, message);
}

// GLOBALB: every career race chosen for a placed race start is moved to it --
// the start trigger at +0x28 of its record becomes the start's zone hash, in
// every career chunk. Nothing changes size. A race is named as MarkerSet
// lists it: the string its record points at, or the record's first word in
// hex when that is a hash rather than an offset.
bool BuildCareerRaces(std::vector<u8>& bytes, const std::vector<const nfs::Marker*>& starts, size_t& moved,
                      std::wstring& message) {
    moved = 0;
    std::vector<std::string> wanted;
    for (const nfs::Marker* start : starts) wanted.insert(wanted.end(), start->races.begin(), start->races.end());
    std::vector<bool> found(wanted.size(), false);

    for (size_t career = 0; career + 8 <= bytes.size(); career += 8 + (size_t)Get32(bytes, career + 4)) {
        if (Get32(bytes, career) != kCareer) continue;
        const size_t careerEnd = career + 8 + Get32(bytes, career + 4);
        size_t strings = 0, stringsSize = 0, races = 0, racesSize = 0;
        for (size_t at = career + 8; at + 8 <= careerEnd; at += 8 + (size_t)Get32(bytes, at + 4)) {
            if (Get32(bytes, at) == kCareerStrings) { strings = at + 8; stringsSize = Get32(bytes, at + 4); }
            if (Get32(bytes, at) == kCareerRaces)   { races = at + 8; racesSize = Get32(bytes, at + 4); }
        }
        if (!races) continue;
        for (size_t record = races; record + kRaceRecord <= races + racesSize; record += kRaceRecord) {
            const u32 word = Get32(bytes, record);
            std::string name;
            if (strings && word < stringsSize) {
                const char* text = (const char*)bytes.data() + strings + word;
                name.assign(text, ::strnlen(text, stringsSize - word));
            }
            if (name.empty()) {
                char hex[16];
                std::snprintf(hex, sizeof(hex), "%08X", word);
                name = hex;
            }
            size_t k = 0;
            for (const nfs::Marker* start : starts)
                for (const std::string& race : start->races) {
                    if (race == name) {
                        Set32(bytes, record + kRaceTrigger, ZoneHash(start->name));
                        if (!found[k]) ++moved;
                        found[k] = true;
                    }
                    ++k;
                }
        }
    }
    for (size_t k = 0; k < wanted.size(); ++k)
        if (!found[k]) {
            message = L"The career has no race " + std::wstring(wanted[k].begin(), wanted[k].end()) + L".";
            return false;
        }
    return true;
}

// PathsFreeRoam.bin: the zones, one appended to 0x3414A per shop.
bool BuildZones(std::vector<u8>& bytes, const std::vector<PlacedShop>& shops, std::wstring& message) {
    // Right after the last shop zone. The game keeps the records sorted by
    // type and finds a type's zones as the run between where that type starts
    // and where the next one does (SPEED2.EXE 0x5D5630), so a shop zone past
    // the type-19 records is never seen as a shop -- which is how the first
    // tests failed.
    std::vector<u8> added;
    for (const PlacedShop& placed : shops) {
        const std::vector<u8> zone = ShopZone(*placed.marker, ZoneHash(placed.marker->name));
        added.insert(added.end(), zone.begin(), zone.end());
    }
    return InsertZones(bytes, kZoneShop, added, message);
}

// TrackPosMarkersFreeRoam.bin: where the beacon stands, in 3D. 48-byte records
// in 0x34146 named by the bin hash of the shop's own name; each new one is a
// copy of its template's with the name and the position changed.
bool BuildPositions(std::vector<u8>& bytes, const std::vector<PlacedShop>& shops, std::wstring& message) {
    if (bytes.size() < 8 || Get32(bytes, 0) != kTrackPositions) {
        message = L"TrackPosMarkersFreeRoam.bin does not start with 0x34146.";
        return false;
    }
    const size_t end = 8 + (size_t)Get32(bytes, 4);
    size_t first = 8;
    while (first < end && bytes[first] == 0x11) ++first;   // alignment filler

    std::vector<u8> added;
    for (const PlacedShop& placed : shops) {
        const u32 templateHash = BinHash(placed.shop->name);
        size_t found = 0;
        for (size_t at = first; at + kPositionRecord <= end; at += kPositionRecord)
            if (Get32(bytes, at + 8) == templateHash) { found = at; break; }
        if (!found) {
            message = L"No track position for the template shop " +
                      std::wstring(placed.shop->name.begin(), placed.shop->name.end()) + L".";
            return false;
        }
        std::vector<u8> record(bytes.begin() + (std::ptrdiff_t)found,
                               bytes.begin() + (std::ptrdiff_t)(found + kPositionRecord));
        Set32(record, 8, BinHash(placed.marker->name));
        SetF(record, 0x10, placed.marker->position.x);
        SetF(record, 0x14, placed.marker->position.y);
        SetF(record, 0x18, placed.marker->position.z);
        added.insert(added.end(), record.begin(), record.end());
    }
    bytes.insert(bytes.begin() + (std::ptrdiff_t)end, added.begin(), added.end());
    Set32(bytes, 4, Get32(bytes, 4) + (u32)added.size());
    return true;
}

// GLOBALB: a shop record per placed shop appended to the shop list of every
// career chunk, each a copy of its template with the name and zone hash
// changed. The padding chunk after the career is resized so everything behind
// it moves by a multiple of kShiftUnit.
bool BuildCareer(std::vector<u8>& bytes, const std::vector<PlacedShop>& shops, std::wstring& message) {
    // Top-level chunks, back to front, so growing one leaves the offsets of the
    // ones still to do alone.
    std::vector<size_t> tops;
    for (size_t at = 0; at + 8 <= bytes.size(); at += 8 + (size_t)Get32(bytes, at + 4))
        tops.push_back(at);

    size_t careers = 0;
    for (size_t t = tops.size(); t-- > 0;) {
        const size_t career = tops[t];
        if (Get32(bytes, career) != kCareer) continue;

        const size_t careerEnd = career + 8 + Get32(bytes, career + 4);
        size_t list = 0;
        for (size_t at = career + 8; at + 8 <= careerEnd; at += 8 + (size_t)Get32(bytes, at + 4))
            if (Get32(bytes, at) == kCareerShops) { list = at; break; }
        if (!list) continue;

        const size_t listEnd = list + 8 + Get32(bytes, list + 4);
        std::vector<u8> added;
        for (const PlacedShop& placed : shops) {
            size_t found = 0;
            for (size_t at = list + 8; at + kShopRecord <= listEnd; at += kShopRecord)
                if (std::strncmp((const char*)bytes.data() + at, placed.shop->name.c_str(),
                                 kShopNameBytes) == 0) { found = at; break; }
            if (!found) {
                message = L"The template shop is missing from a career chunk.";
                return false;
            }
            std::vector<u8> record(bytes.begin() + (std::ptrdiff_t)found,
                                   bytes.begin() + (std::ptrdiff_t)(found + kShopRecord));
            std::memset(record.data(), 0, kShopNameBytes);
            std::memcpy(record.data(), placed.marker->name.c_str(),
                        std::min(placed.marker->name.size(), kShopNameBytes - 1));
            // The game goes by the stored hash of the name, not the text: it
            // is what the track position and the parts tables name a shop by.
            // Left at zero the shop stood in the list and nothing found it.
            Set32(record, kShopNameHash, BinHash(placed.marker->name));
            Set32(record, kShopZoneHash, ZoneHash(placed.marker->name));
            // Without it the save has never heard of the shop: it stood on the
            // street, lit, with no prompt and off both maps.
            Set32(record, kShopPlacedMagic, kPlacedMagic);
            // Open on any save. The career gates a shop twice before its event
            // goes in the table (SPEED2.EXE 0x533310): its stage, +0x9D, against
            // the save's, and by +0x9C a test of the career's progress
            // (0x512870) -- the first stage's shops, the CC_ ones a copy is
            // usually made from, ask for a save entry that only holds early in
            // the career, so on a later save the copy stood lit and was never
            // offered.
            record[0x9C] = 0;   // no progress test
            record[0x9D] = 0;   // from the first stage
            added.insert(added.end(), record.begin(), record.end());
        }

        // Where the chunk after the career used to start, and the padding
        // chunk that is to put it back on the same alignment.
        const size_t oldNext = careerEnd;
        const bool padded = oldNext + 8 <= bytes.size() && Get32(bytes, oldNext) == 0;
        const size_t padEnd = padded ? oldNext + 8 + Get32(bytes, oldNext + 4) : oldNext;

        bytes.insert(bytes.begin() + (std::ptrdiff_t)listEnd, added.begin(), added.end());
        Set32(bytes, list + 4, Get32(bytes, list + 4) + (u32)added.size());
        Set32(bytes, career + 4, Get32(bytes, career + 4) + (u32)added.size());

        const size_t padAt = oldNext + added.size();
        size_t shift = kShiftUnit;
        while (shift < added.size() + 8) shift += kShiftUnit;
        const size_t newPadEnd = padEnd + shift;
        if (padded) {
            const size_t grow = newPadEnd - (padAt + 8 + Get32(bytes, padAt + 4));
            bytes.insert(bytes.begin() + (std::ptrdiff_t)(padAt + 8), grow, 0);
            Set32(bytes, padAt + 4, (u32)(newPadEnd - padAt - 8));
        } else {
            std::vector<u8> pad(newPadEnd - padAt, 0);
            Set32(pad, 4, (u32)(pad.size() - 8));
            bytes.insert(bytes.begin() + (std::ptrdiff_t)padAt, pad.begin(), pad.end());
        }
        ++careers;
    }
    if (!careers) {
        message = L"GLOBALB.BUN holds no career shop list.";
        return false;
    }
    return true;
}

// Inserts `data` -- whole chunks -- after the last top-level chunk `id`, then
// resizes the padding chunk that follows so every later chunk moves by a
// multiple of kShiftUnit and keeps its alignment. Adds a padding chunk when
// there is none.
// Replaces [start, end) -- whole top-level chunks, or nothing -- with
// `replacement`, and remakes the padding chunk (id 0) that follows so the
// next real chunk moves by a whole number of kShiftUnit, or not at all when
// the old padding has the room. Whatever alignment the later chunks were laid
// out to survives.
void SpliceRealigned(std::vector<u8>& bytes, size_t start, size_t end, const std::vector<u8>& replacement) {
    size_t next = end;
    if (next + 8 <= bytes.size() && Get32(bytes, next) == 0) next += 8 + Get32(bytes, next + 4);

    const size_t padAt = start + replacement.size();
    size_t newNext = next;
    while (newNext < padAt + 8) newNext += kShiftUnit;

    std::vector<u8> pad(newNext - padAt, 0);
    Set32(pad, 4, (u32)(pad.size() - 8));

    std::vector<u8> middle(replacement);
    middle.insert(middle.end(), pad.begin(), pad.end());
    bytes.erase(bytes.begin() + (std::ptrdiff_t)start, bytes.begin() + (std::ptrdiff_t)next);
    bytes.insert(bytes.begin() + (std::ptrdiff_t)start, middle.begin(), middle.end());
}

// The offset of the last top-level chunk `id`, or SIZE_MAX.
size_t LastTopChunk(const std::vector<u8>& bytes, u32 id) {
    size_t last = SIZE_MAX;
    for (size_t at = 0; at + 8 <= bytes.size(); at += 8 + (size_t)Get32(bytes, at + 4))
        if (Get32(bytes, at) == id) last = at;
    return last;
}

// `data` -- whole chunks -- after the last top-level chunk `id`.
bool AppendToBlock(std::vector<u8>& bytes, u32 id, const std::vector<u8>& data) {
    const size_t last = LastTopChunk(bytes, id);
    if (last == SIZE_MAX) return false;
    const size_t end = last + 8 + Get32(bytes, last + 4);
    SpliceRealigned(bytes, end, end, data);
    return true;
}

// <region>.BUN: the beacon itself. The glow is a placed effect, not something
// the shop list conjures: a 0x37090 instance (168 bytes: an id, the effect
// type B8A18038, the shop's zone hash twice, a 4x4 matrix and a box) and a
// 0x37150 index record (id, type, flags) per shop. Each placed shop gets a copy
// of its template's pair with a new id, its own zone hash, and its position.
bool BuildBeacons(std::vector<u8>& bytes, const std::vector<PlacedShop>& shops, std::wstring& message) {
    constexpr u32 kInstance = 0x00037090, kIndex = 0x00037150;
    std::vector<size_t> instances, indices;
    u32 nextId = 0;
    for (size_t at = 0; at + 8 <= bytes.size(); at += 8 + (size_t)Get32(bytes, at + 4)) {
        const u32 id = Get32(bytes, at);
        if (id != kInstance && id != kIndex) continue;
        (id == kInstance ? instances : indices).push_back(at);
        const u32 number = Get32(bytes, at + 8 + 8);   // past the 8 bytes of filler
        if (number < 0x10000 && number >= nextId) nextId = number + 1;
    }

    std::vector<u8> newInstances, newIndices;
    for (const PlacedShop& placed : shops) {
        const u32 templateZone = placed.shop->hash;
        size_t instance = 0, index = 0;
        u32 number = 0;
        for (size_t at : instances)
            if (Get32(bytes, at + 8 + 0x28) == templateZone) { instance = at; break; }
        if (instance) {
            number = Get32(bytes, instance + 16);
            for (size_t at : indices)
                if (Get32(bytes, at + 16) == number) { index = at; break; }
        }
        if (!instance || !index) {
            message = L"The template shop has no beacon in the region file.";
            return false;
        }

        std::vector<u8> chunk(bytes.begin() + (std::ptrdiff_t)instance,
                              bytes.begin() + (std::ptrdiff_t)(instance + 8 + Get32(bytes, instance + 4)));
        const size_t p = 8;   // payload
        const u32 id = nextId++;
        // The zone hash the door answers to: the placed shop's own.
        const u32 zone = ZoneHash(placed.marker->name);
        Set32(chunk, p + 0x08, id);
        Set32(chunk, p + 0x28, zone);
        Set32(chunk, p + 0x2C, zone);

        // Where it stood to its shop, carried over to the marker, plus the
        // marker's door adjustment (nfs::PlacedDoorMatrix -- the editor
        // previews the door through the same function).
        Mat4 shipped;
        std::memcpy(shipped.m, chunk.data() + p + 0x48, sizeof(shipped.m));
        const Mat4 door = nfs::PlacedDoorMatrix(shipped, *placed.shop, *placed.marker);
        std::memcpy(chunk.data() + p + 0x48, door.m, sizeof(door.m));
        // The box goes along with the door's place (it only culls).
        for (int k = 0; k < 3; ++k)
            for (size_t box : { p + 0x88, p + 0x98 }) {
                float v;
                std::memcpy(&v, chunk.data() + box + 4 * k, 4);
                SetF(chunk, box + 4 * k, v - shipped.m[3][k] + door.m[3][k]);
            }
        newInstances.insert(newInstances.end(), chunk.begin(), chunk.end());

        std::vector<u8> entry(bytes.begin() + (std::ptrdiff_t)index,
                              bytes.begin() + (std::ptrdiff_t)(index + 8 + Get32(bytes, index + 4)));
        Set32(entry, 8 + 0x08, id);
        newIndices.insert(newIndices.end(), entry.begin(), entry.end());
    }

    // The index block comes after the instance block, so it goes in first and
    // the instance offsets found above stay true.
    if (newInstances.empty()) return true;
    if (!AppendToBlock(bytes, kIndex, newIndices) || !AppendToBlock(bytes, kInstance, newInstances)) {
        message = L"The region file has no beacon chunks to add to.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The world map (UI_InGame_WorldMap.fng, an FE package in GLOBALB)
//
// The map shows an event with an object of the package named after its
// category and a count, PARTSSHOPINDICATOR_1..6 and so on (SPEED2.EXE
// 0x4B0670, prefixes at 0x7F4348); the package lays out six of a shop kind,
// one garage, and the shipped shops use them, so a placed shop gets the next
// one cloned from the kind's first. The minimap does the same from its HUD
// package, numbered from 0 (AddMinimapIndicators).
//
// The package: tag + u32 size throughout. The root holds PkHd (+0x0C: the
// number of top-level objects, those without a parent), then the objects
// container, one FOb per object. An FOb holds ObjD -- fields of a two-byte tag
// and a u16 size: Ot type, Oh name hash, OP {id, name hash, flags, resource},
// SA {colour B, G, R, A as u32, ...} -- and its scripts. Object ids are unique
// in the package.
// ---------------------------------------------------------------------------

constexpr u32 kWorldMapPackage = 0xB105A73B;     // hash of UI_InGame_WorldMap.fng
constexpr u32 kChunkPackedPackage = 0x00030210;

struct FeField { size_t at, size; };   // the field's data

// ObjD's fields of the FOb at `object`, by tag.
bool ObjectFields(const std::vector<u8>& fe, size_t object, FeField& oh, FeField& op, FeField& sa) {
    const size_t end = object + 8 + Get32(fe, object + 4);
    oh = op = sa = { 0, 0 };
    for (size_t at = object + 8; at + 8 <= end; at += 8 + Get32(fe, at + 4)) {
        if (std::memcmp(fe.data() + at, "ObjD", 4) != 0) continue;
        const size_t dataEnd = at + 8 + Get32(fe, at + 4);
        for (size_t f = at + 8; f + 4 <= dataEnd;) {
            u16 size = 0;
            std::memcpy(&size, fe.data() + f + 2, 2);
            const FeField field{ f + 4, size };
            if (fe[f] == 'O' && fe[f + 1] == 'h') oh = field;
            else if (fe[f] == 'O' && fe[f + 1] == 'P') op = field;
            else if (fe[f] == 'S' && fe[f + 1] == 'A') sa = field;
            f += 4 + size;
        }
        return oh.size == 4 && op.size == 16;
    }
    return false;
}

// One object to add to an FE package: a copy of `from` named `name`, in
// `colour` (0xRRGGBB) when `recolour`.
struct ObjectClone {
    std::string from, name;
    bool recolour = false;
    u32 colour = 0;
};

// Whether the FOb at `object` has a parent (a PA field in its ObjD).
bool HasParent(const std::vector<u8>& fe, size_t object) {
    const size_t end = object + 8 + Get32(fe, object + 4);
    for (size_t at = object + 8; at + 8 <= end; at += 8 + Get32(fe, at + 4)) {
        if (std::memcmp(fe.data() + at, "ObjD", 4) != 0) continue;
        const size_t dataEnd = at + 8 + Get32(fe, at + 4);
        for (size_t f = at + 8; f + 4 <= dataEnd;) {
            u16 size = 0;
            std::memcpy(&size, fe.data() + f + 2, 2);
            if (fe[f] == 'P' && fe[f + 1] == 'A') return true;
            f += 4 + size;
        }
    }
    return false;
}

// Adds `clones` to an unpacked FE package (root at 0, PkHd at 8, then the
// objects container), each right after its template with the next free id.
// A name the package has already is left alone, so writing twice adds
// nothing. Top-level clones are counted in PkHd; a template inside a group
// keeps its parent, and so do its copies.
bool CloneObjects(std::vector<u8>& fe, const std::vector<ObjectClone>& clones, const wchar_t* package,
                  size_t& added, std::wstring& message) {
    added = 0;
    if (fe.size() < 16 || std::memcmp(fe.data() + 8, "PkHd", 4) != 0) {
        message = std::wstring(package) + L" is not an FE package this editor reads.";
        return false;
    }
    for (const ObjectClone& wanted : clones) {
        const size_t rootEnd = std::min(fe.size(), (size_t)8 + Get32(fe, 4));
        size_t objects = 0;
        for (size_t at = 8; at + 8 <= rootEnd; at += 8 + Get32(fe, at + 4))
            if (std::memcmp(fe.data() + at, "Obj", 3) == 0) objects = at;
        if (!objects) {
            message = std::wstring(package) + L" has no objects.";
            return false;
        }
        const size_t objectsEnd = objects + 8 + Get32(fe, objects + 4);
        const u32 fromHash = BinHash(wanted.from), nameHash = BinHash(wanted.name);
        size_t source = 0;
        bool present = false;
        u32 lastId = 0;
        for (size_t at = objects + 8; at + 8 <= objectsEnd; at += 8 + Get32(fe, at + 4)) {
            if (std::memcmp(fe.data() + at, "FOb", 3) != 0) continue;
            FeField oh, op, sa;
            if (!ObjectFields(fe, at, oh, op, sa)) continue;
            if (Get32(fe, oh.at) == nameHash) present = true;
            if (Get32(fe, oh.at) == fromHash && !source) source = at;
            lastId = std::max(lastId, Get32(fe, op.at));
        }
        if (present) continue;
        if (!source) {
            message = std::wstring(package) + L" has no " + std::wstring(wanted.from.begin(), wanted.from.end()) +
                      L" to copy.";
            return false;
        }
        std::vector<u8> clone(fe.begin() + (std::ptrdiff_t)source,
                              fe.begin() + (std::ptrdiff_t)(source + 8 + Get32(fe, source + 4)));
        FeField oh, op, sa;
        ObjectFields(clone, 0, oh, op, sa);
        Set32(clone, oh.at, nameHash);
        Set32(clone, op.at, lastId + 1);
        Set32(clone, op.at + 4, nameHash);
        if (wanted.recolour && sa.size >= 16) {
            Set32(clone, sa.at + 0, wanted.colour & 0xFF);
            Set32(clone, sa.at + 4, (wanted.colour >> 8) & 0xFF);
            Set32(clone, sa.at + 8, (wanted.colour >> 16) & 0xFF);
        }
        const size_t insertAt = source + 8 + Get32(fe, source + 4);
        fe.insert(fe.begin() + (std::ptrdiff_t)insertAt, clone.begin(), clone.end());
        Set32(fe, objects + 4, Get32(fe, objects + 4) + (u32)clone.size());
        Set32(fe, 4, Get32(fe, 4) + (u32)clone.size());
        if (!HasParent(clone, 0)) Set32(fe, 8 + 8 + 0x0C, Get32(fe, 8 + 8 + 0x0C) + 1);
        ++added;
    }
    return true;
}

// The world map's objects (UI_InGame_WorldMap.fng, packed in GLOBALB).
bool AddMapIndicators(std::vector<u8>& bytes, const std::vector<ObjectClone>& clones, std::wstring& message) {
    if (clones.empty()) return true;
    size_t chunk = SIZE_MAX;
    for (size_t at = 0; at + 12 <= bytes.size(); at += 8 + (size_t)Get32(bytes, at + 4))
        if (Get32(bytes, at) == kChunkPackedPackage && Get32(bytes, at + 8) == kWorldMapPackage) chunk = at;
    if (chunk == SIZE_MAX) {
        message = L"GLOBALB.BUN has no UI_InGame_WorldMap.fng.";
        return false;
    }
    const size_t chunkEnd = chunk + 8 + Get32(bytes, chunk + 4);
    std::vector<u8> fe = nfs::JdlzDecompress(ByteSpan{ bytes.data() + chunk + 12, chunkEnd - chunk - 12 });
    size_t added = 0;
    if (!CloneObjects(fe, clones, L"UI_InGame_WorldMap.fng", added, message)) return false;
    if (!added) return true;

    const std::vector<u8> packed = nfs::JdlzStore(ByteSpan{ fe.data(), fe.size() });
    std::vector<u8> rebuilt(12);
    Set32(rebuilt, 0, kChunkPackedPackage);
    Set32(rebuilt, 8, kWorldMapPackage);
    rebuilt.insert(rebuilt.end(), packed.begin(), packed.end());
    while (rebuilt.size() % 4) rebuilt.push_back(0);
    Set32(rebuilt, 4, (u32)(rebuilt.size() - 8));
    SpliceRealigned(bytes, chunk, chunkEnd, rebuilt);
    return true;
}

// The minimap's pool (HUD_SingleRace.fng, an unpacked package -- chunk
// 0x30203 -- in GLOBAL\InGameFreeRoam.bun). The game looks up
// <KIND>INDICATOR_0..7 per kind (the loader makes it 0..15); the package has
// 0..5 of each shop kind and the shipped shops use them all.
bool AddMinimapIndicators(std::vector<u8>& bytes, const std::vector<ObjectClone>& clones, std::wstring& message) {
    if (clones.empty()) return true;
    constexpr u32 kChunkPackage = 0x00030203;
    const u32 probe = BinHash("PARTSSHOPINDICATOR_0");
    size_t chunk = SIZE_MAX;
    for (size_t at = 0; at + 8 <= bytes.size(); at += 8 + (size_t)Get32(bytes, at + 4)) {
        if (Get32(bytes, at) != kChunkPackage) continue;
        const size_t end = std::min(bytes.size(), at + 8 + (size_t)Get32(bytes, at + 4));
        for (size_t k = at + 8; k + 4 <= end && chunk == SIZE_MAX; k += 4)
            if (Get32(bytes, k) == probe) chunk = at;
    }
    if (chunk == SIZE_MAX) {
        message = L"InGameFreeRoam.bun has no minimap package (HUD_SingleRace.fng).";
        return false;
    }
    const size_t chunkEnd = chunk + 8 + Get32(bytes, chunk + 4);
    std::vector<u8> fe(bytes.begin() + (std::ptrdiff_t)(chunk + 8), bytes.begin() + (std::ptrdiff_t)chunkEnd);
    size_t added = 0;
    if (!CloneObjects(fe, clones, L"HUD_SingleRace.fng", added, message)) return false;
    if (!added) return true;

    std::vector<u8> rebuilt(8);
    Set32(rebuilt, 0, kChunkPackage);
    rebuilt.insert(rebuilt.end(), fe.begin(), fe.end());
    while (rebuilt.size() % 4) rebuilt.push_back(0);
    Set32(rebuilt, 4, (u32)(rebuilt.size() - 8));
    SpliceRealigned(bytes, chunk, chunkEnd, rebuilt);
    return true;
}

// The prefix the world map and the minimap name a shop kind's objects with.
const char* IndicatorPrefix(nfs::MarkerKind kind) {
    switch (kind) {
        case nfs::MarkerKind::Graphics:       return "PAINTSHOPINDICATOR_";
        case nfs::MarkerKind::BodyShop:       return "PARTSSHOPINDICATOR_";
        case nfs::MarkerKind::Performance:    return "PERFORMANCESHOPINDICATOR_";
        case nfs::MarkerKind::CarSpecialties: return "ICESHOPINDICATOR_";
        case nfs::MarkerKind::CarLot:         return "CARLOTINDICATOR_";
        case nfs::MarkerKind::Garage:         return "CRIBINDICATOR_";
        default:                              return nullptr;
    }
}

// The objects the placed shops need: per kind, as many as the shipped shops
// of it plus the placed ones -- on the world map numbered from 1, on the
// minimap from 0 -- each a copy of the kind's first.
void PlacedShopIndicators(const nfs::MarkerSet& markers, std::vector<ObjectClone>& worldMap,
                          std::vector<ObjectClone>& minimap) {
    for (nfs::MarkerKind kind : nfs::kPlaceableKinds) {
        const char* prefix = IndicatorPrefix(kind);
        if (!prefix) continue;
        size_t shipped = 0, placed = 0;
        for (const nfs::Marker& marker : markers.Markers()) {
            if (!marker.IsPlaced() && marker.kind == kind) ++shipped;
            if (marker.IsPlaced() && marker.style == kind && !marker.opens.empty()) ++placed;
        }
        if (!placed) continue;
        const size_t total = std::min<size_t>(shipped + placed, 16);
        for (size_t n = 1; n <= total; ++n)
            worldMap.push_back({ prefix + std::string("1"), prefix + std::to_string(n) });
        for (size_t n = 0; n < total; ++n)
            minimap.push_back({ prefix + std::string("0"), prefix + std::to_string(n) });
    }
}

void MakeFolders(const std::wstring& gameFolder, const std::wstring& region) {
    for (const std::wstring& part : { std::wstring(L"\\models"), std::wstring(L"\\models\\files"),
                                      std::wstring(L"\\models\\files\\TRACKS"),
                                      L"\\models\\files\\TRACKS\\ROUTES" + region,
                                      std::wstring(L"\\models\\files\\GLOBAL") })
        ::CreateDirectoryW((gameFolder + part).c_str(), nullptr);
}

} // namespace

std::vector<std::wstring> MarkerFiles(const std::wstring& gameFolder, const std::wstring& region) {
    const std::wstring files = gameFolder + L"\\models\\files";
    return { files + L"\\TRACKS\\ROUTES" + region + L"\\PathsFreeRoam.bin",
             files + L"\\TRACKS\\ROUTES" + region + L"\\TrackPosMarkersFreeRoam.bin",
             files + L"\\GLOBAL\\GLOBALB.BUN",
             files + L"\\GLOBAL\\GLOBALB.LZC",
             files + L"\\TRACKS\\" + region + L".BUN",
             files + L"\\GLOBAL\\InGameFreeRoam.bun" };
}

bool WriteMarkersToGame(const std::wstring& gameFolder, const std::wstring& region,
                        const nfs::MarkerSet& markers, std::wstring& message) {
    const std::vector<std::wstring> targets = MarkerFiles(gameFolder, region);

    // Race starts with the races moved to them. One with no race chosen would
    // be a trigger nothing starts from.
    std::vector<const nfs::Marker*> starts;
    std::vector<PlacedShop> shops;
    size_t skipped = 0, idleStarts = 0;
    for (const nfs::Marker& marker : markers.Markers()) {
        if (!marker.IsPlaced()) continue;
        if (marker.IsRaceStart()) {
            if (marker.races.empty()) ++idleStarts;
            else starts.push_back(&marker);
            continue;
        }
        const nfs::Marker* shop = markers.FindShop(marker.opens);
        if (!shop || marker.name.size() >= kShopNameBytes) { ++skipped; continue; }
        shops.push_back({ &marker, shop });
    }

    // What each file needs: the route files and the career for shops and race
    // starts, the region file for the doors, InGameFreeRoam for the minimap's
    // room. A file with nothing to carry is removed, so the game reads its own.
    const bool needRoutes = !shops.empty() || !starts.empty();
    const bool needGlobal = needRoutes;

    std::vector<u8> zones, positions, career, layout;
    const std::wstring routes = gameFolder + L"\\TRACKS\\ROUTES" + region;
    if (needRoutes) {
        zones = ReadWholeFile(routes + L"\\PathsFreeRoam.bin");
        positions = ReadWholeFile(routes + L"\\TrackPosMarkersFreeRoam.bin");
    }
    if (needGlobal) career = ReadWholeFile(gameFolder + L"\\GLOBAL\\GLOBALB.BUN");
    layout = ReadWholeFile(gameFolder + L"\\TRACKS\\" + region + L".BUN");
    if ((needRoutes && (zones.empty() || positions.empty())) || (needGlobal && career.empty()) ||
        layout.empty()) {
        message = L"Could not read the shipped PathsFreeRoam.bin, TrackPosMarkersFreeRoam.bin, "
                  L"GLOBALB.BUN or the region file.";
        return false;
    }
    const size_t shippedLayout = layout.size();

    // The race triggers first: their run comes before the shops'.
    if (needRoutes && (!BuildRaceZones(zones, starts, message) || !BuildZones(zones, shops, message) ||
                       !BuildPositions(positions, shops, message)))
        return false;
    if (!shops.empty() && !BuildCareer(career, shops, message)) return false;
    size_t movedRaces = 0;
    if (!starts.empty() && !BuildCareerRaces(career, starts, movedRaces, message)) return false;

    // The world map and the minimap name an object for each event of a kind;
    // the packages lay out a fixed number, which the shipped shops use up.
    std::vector<ObjectClone> worldMapClones, minimapClones;
    PlacedShopIndicators(markers, worldMapClones, minimapClones);
    if (needGlobal && !AddMapIndicators(career, worldMapClones, message)) return false;
    std::vector<u8> hud;
    const bool needHud = !minimapClones.empty();
    if (needHud) {
        hud = ReadWholeFile(gameFolder + L"\\GLOBAL\\InGameFreeRoam.bun");
        if (hud.empty()) {
            message = L"Could not read GLOBAL\\InGameFreeRoam.bun.";
            return false;
        }
        if (!AddMinimapIndicators(hud, minimapClones, message)) return false;
    }
    if (!BuildBeacons(layout, shops, message)) return false;

    MakeFolders(gameFolder, region);
    // The game asks for GLOBALB.LZC and GLOBALB.BUN by name; an uncompressed
    // copy answers for both.
    const std::vector<u8>* contents[6] = { &zones, &positions, &career, &career, &layout, &hud };
    const bool wanted[6] = { needRoutes, needRoutes, needGlobal, needGlobal, layout.size() != shippedLayout,
                             needHud };
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!wanted[i]) {
            ::DeleteFileW(targets[i].c_str());
            continue;
        }
        if (!WriteWholeFile(targets[i], *contents[i])) {
            message = L"Could not write " + targets[i];
            return false;
        }
    }

    wchar_t text[400];
    ::swprintf_s(text, L"%zu shop(s) and %zu race start(s), with %zu race(s) moved to them, handed to the game%s%s",
                 shops.size(), starts.size(), movedRaces,
                 skipped ? L"; shops without a shipped shop to copy were left out" : L"",
                 idleStarts ? L"; race starts with no race chosen were left out" : L"");
    message = text;
    LOG_INFO("markers: %zu shops, %zu race starts (%zu races) written to models\\files (%zu left out)",
             shops.size(), starts.size(), movedRaces, skipped + idleStarts);
    return true;
}

} // namespace io
