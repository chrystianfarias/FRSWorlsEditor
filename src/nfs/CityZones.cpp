#include "nfs/CityZones.h"

#include "nfs/FreeRoam.h"

#include <cwctype>

namespace nfs {

namespace {

u32 BinHash(const char* text) {
    u32 hash = 0xFFFFFFFFu;
    for (const char* c = text; *c; ++c) hash = hash * 33u + (u8)*c;
    return hash;
}

struct KnownZone {
    const char*    name;       // as the game spells it, for the hash
    const wchar_t* district;
};

// Every name the zones of L4RA carry, matched by hash against the strings in
// SPEED2.EXE and the game's data files.
const KnownZone kKnownZones[] = {
    { "HOTELS",            L"City Core" },
    { "FINANCE",           L"City Core" },
    { "PIGEON_PARK",       L"City Core" },
    { "FORT_UNION",        L"City Core" },
    { "SOUTH_MARKET",      L"City Core" },
    { "STADIUM",           L"City Core" },
    { "BEACON_HILL_WEST",  L"Beacon Hill" },
    { "BEACON_HILL_EAST",  L"Beacon Hill" },
    { "JACKSON_HEIGHTS",   L"Jackson Heights" },
    { "COAL_HARBOUR_EAST", L"Coal Harbour" },
    { "COAL_HARBOUR_WEST", L"Coal Harbour" },
    { "AIRPORT",           L"Airport" },
};

// PIGEON_PARK -> Pigeon Park.
std::wstring Readable(const char* name) {
    std::wstring out;
    bool start = true;
    for (const char* c = name; *c; ++c) {
        if (*c == '_') { out.push_back(L' '); start = true; continue; }
        out.push_back(start ? (wchar_t)std::towupper(*c) : (wchar_t)std::towlower(*c));
        start = false;
    }
    return out;
}

} // namespace

Vec2 CityZone::Centre() const {
    // The polygon's own centre of area, not the box's: an L-shaped zone's box
    // centre can lie outside it.
    double area = 0.0, cx = 0.0, cy = 0.0;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Vec2& a = polygon[i];
        const Vec2& b = polygon[(i + 1) % polygon.size()];
        const double cross = (double)a.x * b.y - (double)b.x * a.y;
        area += cross;
        cx += (a.x + b.x) * cross;
        cy += (a.y + b.y) * cross;
    }
    if (std::abs(area) < 1e-6)
        return { (boundsMin.x + boundsMax.x) * 0.5f, (boundsMin.y + boundsMax.y) * 0.5f };
    return { (float)(cx / (3.0 * area)), (float)(cy / (3.0 * area)) };
}

bool CityZone::Contains(float x, float y) const {
    if (x < boundsMin.x || x > boundsMax.x || y < boundsMin.y || y > boundsMax.y) return false;
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2& a = polygon[i];
        const Vec2& b = polygon[j];
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

std::vector<CityZone> ReadCityZones(const std::vector<FreeRoamZone>& zones) {
    std::vector<CityZone> out;
    for (const FreeRoamZone& zone : zones) {
        if (zone.type != kCityZoneType || zone.polygon.size() < 3) continue;
        CityZone city;
        city.hash = zone.hash;
        city.polygon = zone.polygon;
        city.boundsMin = zone.boundsMin;
        city.boundsMax = zone.boundsMax;
        city.name = L"Unnamed zone";
        city.district = L"Other";
        for (const KnownZone& known : kKnownZones)
            if (BinHash(known.name) == zone.hash) {
                city.name = Readable(known.name);
                city.district = known.district;
            }
        out.push_back(std::move(city));
    }
    return out;
}

const std::vector<std::wstring>& DistrictOrder() {
    static const std::vector<std::wstring> order = { L"City Core", L"Beacon Hill", L"Jackson Heights",
                                                     L"Coal Harbour", L"Airport", L"Other" };
    return order;
}

int ZoneAt(const std::vector<CityZone>& zones, float x, float y) {
    int best = -1;
    for (size_t i = 0; i < zones.size(); ++i)
        if (zones[i].Contains(x, y) && (best < 0 || zones[i].Area() < zones[(size_t)best].Area()))
            best = (int)i;
    return best;
}

} // namespace nfs
