#include "drive/cars/CarTexturesU2.h"

#include "drive/cars/CarBin.h"

namespace bb {

namespace {

uint32_t H(const std::string& s) { return carbin::Hash(s); }
uint32_t Hc(const char* s, uint32_t seed) { return carbin::Hash(s, seed); }

// dword_8A4358[27] (sub_77EF20): decal placeholders of main entries 46..72.
const char* const decalNames[27] = {
    "BOTTOM_DECAL", "EXHAUST_TIP_DECAL", "FRONT_BUMPER_DECAL", "FRONT_DECAL", "GTWING_DECAL", "HOOD_DECAL",
    "LEFT_BRAKELIGHT_DECAL", "LEFT_DOOR_DECAL", "LEFT_FENDER_DECAL", "LEFT_QUARTER_DECAL",
    "LEFT_SIDE_MIRROR_DECAL", "LEFT_SKIRT_DECAL", "REAR_BUMPER_DECAL", "REAR_DECAL",
    "RIGHT_BRAKELIGHT_DECAL", "RIGHT_DOOR_DECAL", "RIGHT_FENDER_DECAL", "RIGHT_QUARTER_DECAL",
    "RIGHT_SIDE_MIRROR_DECAL", "RIGHT_SKIRT_DECAL", "TOP_DECAL", "FRONT_WINDOW_DECAL", "REAR_WINDOW_DECAL",
    "LEFT_FRONT_WINDOW_DECAL", "LEFT_REAR_WINDOW_DECAL", "RIGHT_FRONT_WINDOW_DECAL", "RIGHT_REAR_WINDOW_DECAL",
};

const uint32_t windowOld[6] = { 0x7B220DDF, 0x1B0763A0, 0xE7E4EF49, 0x4CDEBFCA, 0x60F8B13C, 0x0AB88F5D };
const uint32_t window2Old[6] = { 0x90E09671, 0x13EE21F2, 0xD689A31B, 0xB8F3415C, 0x2433DF4E, 0xF95D432F };

uint32_t Lookup(const CarTexturesU2::Entry* table, int n, uint32_t hash) {
    for (int i = 0; i < n; i++)
        if (table[i].New != 0 && table[i].Old == hash) return table[i].New; // first active match wins (0x4905A0)
    return hash;
}

} // namespace

CarTexturesU2::CarTexturesU2(const RideInfoU2* ride, int compositeSlot, bool frontEnd,
                             const std::function<bool(uint32_t)>& textureExists, const std::string& badgingSuffix)
    : Ride(ride) {
    const CarPartDatabaseU2& db = *ride->Db;
    const CarTypeInfoU2& ct = *ride->CarType;
    const std::string& b = ct.BaseModelName;
    const uint32_t AttrSpinnerTexture = H("SPINNER_TEXTURE");
    const uint32_t AttrSize = H("SIZE");
    const uint32_t AttrShape = H("SHAPE");
    const uint32_t AttrName = H("NAME");

    // SetCompositeNameHash (0x61C280)
    if (compositeSlot > 0) {
        const std::string n = std::to_string(compositeSlot);
        CompositeSkinHash = ct.IsSkinnable ? H("DUMMY_SKIN" + n) : 0;
        CompositeWheelHash = H("DUMMY_WHEEL" + n);
        CompositeSpinnerHash = H("DUMMY_SPINNER" + n);
    }

    // GetUsedCarTextureInfo (0x631E20): the fields that feed the tables.
    const uint32_t skin1 = H(b + "_SKIN1"), skin1b = H(b + "_SKIN1B"), globalSkin1 = H("GLOBAL_SKIN1");
    const int wheel = ride->Parts[29];
    const uint32_t wheelTex = wheel >= 0 ? Hc("_WHEEL", db.GetTextureName(wheel)) : 0;
    const uint32_t spinnerTex = wheel >= 0 ? db.GetAttribute(wheel, AttrSpinnerTexture) : 0;
    uint32_t newSkin1, newWheel, newSpinner, newSpoiler, newRoof;
    if (CompositeSkinHash != 0) {
        newSkin1 = newSpoiler = newRoof = CompositeSkinHash;
        newWheel = CompositeWheelHash;
        newSpinner = CompositeSpinnerHash;
    } else {
        newSkin1 = newSpoiler = newRoof = skin1;
        newWheel = newSpinner = 0;
    }
    const uint32_t newSkinAll = newSkin1;

    uint32_t hl = H("HEADLIGHT"), bl = H("BRAKELIGHT");
    const uint32_t lightOld[10] = {
        Hc("_LEFT", hl), Hc("_RIGHT", hl), Hc("_LEFT", bl), Hc("_RIGHT", bl), Hc("_CENTRE", bl),
        Hc("_GLASS_LEFT", hl), Hc("_GLASS_RIGHT", hl), Hc("_GLASS_LEFT", bl), Hc("_GLASS_RIGHT", bl), Hc("_GLASS_CENTRE", bl),
    };
    if (ride->Parts[14] >= 0 && db.GetTextureName(ride->Parts[14]) != 0) hl = db.GetTextureName(ride->Parts[14]);
    if (ride->Parts[15] >= 0 && db.GetTextureName(ride->Parts[15]) != 0) bl = db.GetTextureName(ride->Parts[15]);
    const uint32_t hlOff = Hc("_OFF", hl), hlGlassOff = Hc("_GLASS_OFF", hl);
    const uint32_t blOff = Hc("_OFF", bl), blGlassOff = Hc("_GLASS_OFF", bl);
    const uint32_t centreOff = Hc("_OFF", H(b + "_CENTRE_BRAKELIGHT"));

    // Ctor: old hashes (0x638477..0x63861A).
    const uint32_t old[13] = {
        skin1, skin1b, globalSkin1, 0xA7366AE6 /*CARSKIN_CARBONFIBRE*/, 0x3C84D757 /*name unknown*/,
        H(b + "_BADGING"), wheelTex, spinnerTex, H("SPOILER_SKIN"), H("ROOF_SKIN"), 0,
        H("DRIVER"), H(b + "_TIRE"),
    };
    for (int i = 0; i < 13; i++) Main[i].Old = old[i];
    for (int i = 0; i < 6; i++) Main[13 + i].Old = windowOld[i];
    Main[19].Old = H("REAR_DEFROSTER");
    for (int i = 0; i < 6; i++) Main[20 + i].Old = window2Old[i];
    const uint32_t lightConst[10] = { 0xA7E6EA53, 0xA532FC46, 0xD947F346, 0x02B52399, 0x3617DCFC,
                                      0x267D188C, 0xF68EF19F, 0x000BCBBF, 0x01F40B32, 0x1D33B7B5 };
    for (int i = 0; i < 10; i++) {
        Main[26 + i].Old = lightConst[i];
        Main[36 + i].Old = lightOld[i];
    }
    for (int i = 0; i < 27; i++) Main[46 + i].Old = H(decalNames[i]);
    BrakeLeft[0] = { BrakeGlobal, BrakeGlobalLeft };
    BrakeRight[0] = { BrakeGlobal, BrakeGlobal };
    BrakeLeft[1].Old = BrakeRight[1].Old = globalSkin1;

    // SwitchSkin (0x636840)
    Main[0].New = newSkin1;
    Main[1].New = 0;
    Main[6].New = newWheel;
    Main[7].New = newSpinner;
    Main[8].New = newSpoiler;
    Main[9].New = newRoof;
    Main[2].New = Main[3].New = Main[4].New = newSkinAll;
    BrakeLeft[1].New = BrakeRight[1].New = newSkinAll;

    // Badging region suffix (0x6386D9): only if the suffixed texture exists.
    const uint32_t badging = H(b + "_BADGING" + badgingSuffix);
    Main[5].New = !badgingSuffix.empty() && textureExists && textureExists(badging) ? badging : Main[5].Old;

    // UpdateCarReplacementTextures (0x615280), no damage.
    const uint32_t damagedWindow0 = H("WINDOW"), damagedWindow20 = H("WINDOW2");
    for (int i = 0; i < 6; i++) {
        Main[13 + i].New = damagedWindow0;
        Main[20 + i].New = damagedWindow20;
    }
    Main[19].New = Main[19].Old;

    // UpdateDecalTextures (0x61FF40)
    for (int i = 0; i < 27; i++) Main[46 + i].New = DefaultAlpha;
    for (int t = 0; t < 7; t++)
        for (int k = 0; k < 8; k++) Decals[t][k] = { H("DUMMY_DECAL" + std::to_string(k + 1)), DefaultAlpha };
    for (int slot = 52; slot <= 58; slot++) {
        const int layout = ride->Parts[slot];
        if (layout < 0 || (slot == 52 && ride->Parts[9] < 0)) continue;
        if (!db.HasAttribute(layout, AttrSize) || !db.HasAttribute(layout, AttrShape)) continue;
        const uint32_t shape = db.GetAttribute(layout, AttrShape);
        const char* sfx = shape == H("SQUARE") ? "_SQUARE" : shape == H("RECT") ? "_RECT" : shape == H("WIDE") ? "_WIDE" : nullptr;
        for (int k = 0; k < 8; k++) {
            const int d = ride->Parts[97 + 8 * (slot - 52) + k];
            if (d < 0 || !sfx) continue; // unknown shape reads an uninitialised buffer in the game
            Decals[slot - 52][k].New = Hc(sfx, db.GetAttribute(d, AttrName));
        }
    }

    // Ctor: driver (0x63874D).
    Main[11].New = frontEnd ? DefaultAlpha : H("DRIVER_MALE");

    // UpdateLightStateTextures (0x60C940): this exe writes _OFF for both brake states.
    for (int o : { 0, 10 }) {
        Main[26 + o].New = Main[27 + o].New = hlOff;
        Main[31 + o].New = Main[32 + o].New = hlGlassOff;
        Main[28 + o].New = Main[29 + o].New = blOff;
        Main[30 + o].New = Main[35 + o].New = centreOff;
        Main[33 + o].New = Main[34 + o].New = blGlassOff;
    }
}

uint32_t CarTexturesU2::Resolve(uint32_t hash, int slot, bool brake, bool leftBrake) const {
    if (brake) return Lookup(leftBrake ? BrakeLeft : BrakeRight, 2, hash);
    if (slot >= 52 && slot <= 62) {
        const int t = slot <= 58 ? slot - 52 : slot - 59 + 3; // 59..62 use the tables of 55..58
        for (int k = 0; k < 8; k++)
            if (Decals[t][k].New != 0 && Decals[t][k].Old == hash) return Decals[t][k].New;
        return hash;
    }
    return Lookup(Main, 73, hash);
}

} // namespace bb
