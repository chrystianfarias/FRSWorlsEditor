#include "nfs/ModelCategory.h"

#include <initializer_list>

namespace nfs {

namespace {

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool HasAny(const std::string& text, std::initializer_list<const char*> words) {
    for (const char* word : words)
        if (text.find(word) != std::string::npos) return true;
    return false;
}

std::wstring ObjectsSub(const std::string& n) {
    if (HasAny(n, { "LIGHT", "LAMP", "NEON", "LANTERN" }))                      return L"Lights";
    if (HasAny(n, { "AIRPORT", "PLANE", "JETWAY" }))                            return L"Airport";
    if (HasAny(n, { "FWY", "HWY", "BRIDGE", "TUNNEL", "RAIL", "OVERPASS",
                    "SUBWAY", "BEAM", "SUPPORT", "PILLAR" }))                   return L"Highways and bridges";
    if (HasAny(n, { "CONTAINER", "TANK", "CRANE", "PIPE", "DUMPSTER", "CRATE",
                    "BARREL", "PALLET", "CARPILE", "WATERTOWER", "TOWER" }))    return L"Industry";
    if (HasAny(n, { "BARRIER", "CONE", "WORKZONE", "BARRICADE", "BOLLARD" }))  return L"Roadwork and barriers";
    if (HasAny(n, { "BENCH", "HYDRANT", "MAILBOX", "PHONE", "TRASH", "BIN",
                    "METER", "BUS", "KIOSK", "PLANTER", "FOUNTAIN", "TABLE",
                    "CHAIR", "UMBRELLA", "NEWSPAPER", "STAND" }))              return L"Street furniture";
    return L"Other objects";
}

std::wstring SignsSub(const std::string& n) {
    // The district codes -- City Core, Stadium, Industrial Park, Upper Class,
    // Shore -- mark the route signs made for that part of the city.
    if (StartsWith(n, "XS_CC_") || StartsWith(n, "XS_SH_") || StartsWith(n, "XS_IP_") ||
        StartsWith(n, "XS_UC_") || StartsWith(n, "XS_IND_") || StartsWith(n, "XS_FREEWAY"))
        return L"Road signs";
    if (HasAny(n, { "WARN", "LANE", "STOP", "ONEWAY", "SPEED", "ARROW", "EXIT",
                    "YIELD", "MERGE", "CROSSING", "35FT" }))                   return L"Road signs";
    return L"Billboards and shop signs";
}

std::wstring WallsSub(const std::string& n) {
    if (HasAny(n, { "FENCE", "CHAIN" }))                                        return L"Fences";
    if (HasAny(n, { "GUARDRAIL", "BARRIER", "WALLGUARD", "CURB", "RAIL" }))    return L"Rails and barriers";
    return L"Walls";
}

std::wstring PlantsSub(const std::string& n) {
    if (HasAny(n, { "TREE", "PALM", "PINE", "BIRCH", "ASPEN", "SYCAMORE",
                    "EUCALYPTUS", "BAMBOO" }))                                  return L"Trees";
    return L"Bushes and plants";
}

} // namespace

ModelCategory CategorizeModel(const std::string& name) {
    if (StartsWith(name, "FRS_"))                               return { L"My objects", L"" };
    if (StartsWith(name, "XB_") || StartsWith(name, "XBU_"))    return { L"Buildings", L"" };
    if (StartsWith(name, "XO_") || StartsWith(name, "XOU_"))    return { L"Objects", ObjectsSub(name) };
    if (StartsWith(name, "XS_"))                                return { L"Signs", SignsSub(name) };
    if (StartsWith(name, "XW_") || StartsWith(name, "XWU_"))    return { L"Walls and fences", WallsSub(name) };
    if (StartsWith(name, "XT_"))                                return { L"Trees and plants", PlantsSub(name) };
    if (StartsWith(name, "XV_"))                                return { L"Vehicles", L"" };
    if (StartsWith(name, "TRN_"))                               return { L"Terrain and roads", L"" };
    if (StartsWith(name, "ZPM_"))                               return { L"Race graphics", L"" };
    if (StartsWith(name, "PAN_") || StartsWith(name, "SKYDOME")) return { L"Sky and backdrop", L"" };
    // UC_, OSH_ and the odd one-off: pieces made for one place in the city.
    return { L"District pieces", L"" };
}

const std::vector<std::wstring>& ModelCategoryOrder() {
    static const std::vector<std::wstring> order = {
        L"My objects", L"Buildings", L"Objects", L"Signs", L"Walls and fences",
        L"Trees and plants", L"Vehicles", L"District pieces", L"Race graphics",
        L"Terrain and roads", L"Sky and backdrop" };
    return order;
}

} // namespace nfs
