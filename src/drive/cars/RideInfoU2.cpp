#include "drive/cars/RideInfoU2.h"

#include "drive/cars/CarBin.h"

namespace bb {

RideInfoU2::RideInfoU2(const CarPartDatabaseU2* db, const CarTypeInfoU2* carType, bool inGame)
    : Db(db), CarType(carType), InGame(inGame) {
    CarNameHash = carbin::Hash(carType->CollectionName); // CollectionName, not +0xD0
    for (int i = 0; i < CarSlotsU2::NumSlots; i++) {
        Parts[i] = -1;
        Enabled[i] = true;
    }
}

// RideInfo::SetStockParts (0x637040).
void RideInfoU2::SetStockParts(bool alternatePaint) {
    for (int s = 0; s < CarSlotsU2::NumSlots; s++) {
        if (s >= 64 && s <= 67) continue;     // VINYL_LAYER0..3
        if (s >= 97 && s <= 152) continue;    // DECAL_*_TEX*
        if (s == CarSlotsU2::BasePaint) {
            const uint32_t paint = alternatePaint ? CarType->DefaultBasePaint2 : CarType->DefaultBasePaint;
            const int p = Get(s, paint);
            if (p < 0) continue;
            Parts[s] = p;
        } else {
            const int p = Db->FindPartWithLevel(CarNameHash, s, 0);
            if (p < 0) continue;
            if ((s >= 97 && s <= 104) || s == CarSlotsU2::HoodUnder) continue;
            Parts[s] = p;
        }
        UpdatePartsEnabled();
    }
    const uint32_t colours[4] = {
        carbin::Hash("VINYL_L1_COLOR09"), carbin::Hash("VINYL_L1_COLOR02"),
        carbin::Hash("VINYL_L2_COLOR11"), carbin::Hash("VINYL_L1_COLOR01"),
    };
    for (int s = 77; s < 93;)
        for (int k = 0; k < 4; k++, s++) {
            Parts[s] = Get(s, colours[k]); // stored even when not found
            UpdatePartsEnabled();
        }
}

// RideInfo::SetPart (0x631D20). Returns the previous part.
int RideInfoU2::SetPart(int slot, int part) {
    if (slot >= 97 && slot <= 104) return -1;
    if (slot == CarSlotsU2::HoodUnder) return Parts[CarSlotsU2::HoodUnder];
    const int old = Parts[slot];
    Parts[slot] = part;
    UpdatePartsEnabled();
    return old;
}

// RideInfo::UpdatePartsEnabled (0x61BCD0), including the original quirks.
void RideInfoU2::UpdatePartsEnabled() {
    for (bool& e : Enabled) e = true;
    for (int s = 0; s < CarSlotsU2::NumSlots; s++) {
        const int p = Parts[s];
        if (p >= 0) {
            for (int a = Db->GetNextAppliedAttribute(p, CarPartDatabaseU2::AttrPartHide); a >= 0;
                 a = Db->GetNextAppliedAttribute(p, CarPartDatabaseU2::AttrPartHide, a)) {
                // Quirk: k indexes CarPartIDNames but is used as a slot id (DECAL_HOOD -> slot 37).
                const uint32_t value = Db->Attributes[a].Value;
                for (int k = 0; k < 69; k++)
                    if (carbin::Hash(CarSlotsU2::PartIdTableNames[k]) == value) {
                        Enabled[k] = false;
                        break;
                    }
            }
        }
        switch (s) {
            case CarSlotsU2::Hydraulics:
                if (p >= 0) HydraulicsLevel = Db->Parts[p].LevelGroup >> 5;
                break;
            case CarSlotsU2::FrontWheel:
                if (BrowsingBrakePaint) Enabled[29] = false;
                break;
            case CarSlotsU2::Hood:
                if (Parts[9] >= 0) {
                    const uint32_t h = Db->GetAttribute(Parts[9], CarPartDatabaseU2::AttrHoodUnder);
                    Parts[25] = Get(25, h);
                }
                break;
            case CarSlotsU2::Engine:
                if (p >= 0 && InGame) Enabled[9] = Enabled[25] = Enabled[52] = Enabled[13] = true;
                break;
            case CarSlotsU2::WideBody:
                UpdateWideBody(p);
                break;
            default:
                break;
        }
    }
}

void RideInfoU2::UpdateWideBody(int p) {
    if (p >= 0 && Db->Parts[p].LevelGroup >> 5 > 0) {
        const std::string& car = CarType->CollectionName;
        const int kit = Db->Parts[p].LevelGroup & 0x1F;
        const uint32_t w = carbin::Hash(car + "_WIDE" + std::to_string(kit) + "_");
        Parts[59] = Get(59, carbin::Hash("DECAL_LEFT_DOOR_RECT_MEDIUM", w));
        Parts[60] = Get(60, carbin::Hash("DECAL_RIGHT_DOOR_RECT_MEDIUM", w));
        Parts[61] = Get(61, carbin::Hash("DECAL_LEFT_QUARTER_RECT_MEDIUM", w));
        Parts[62] = Get(62, carbin::Hash("DECAL_RIGHT_QUARTER_RECT_MEDIUM", w));
        const uint32_t d = carbin::Hash(car + "_KITW0" + std::to_string(kit) + "_DOOR_");
        Parts[17] = Get(17, carbin::Hash("LEFT", d));
        Parts[18] = Get(18, carbin::Hash("RIGHT", d));
        Parts[19] = Get(19, carbin::Hash("PANEL_LEFT", d));
        Parts[20] = Get(20, carbin::Hash("PANEL_RIGHT", d));
        Parts[21] = Get(21, carbin::Hash("SILL_LEFT", d));
        Parts[22] = Get(22, carbin::Hash("SILL_RIGHT", d));
        Enabled[1] = Enabled[2] = Enabled[11] = Enabled[5] = Enabled[24] = Enabled[23] = false;
        Enabled[6] = true;
        for (int k = 0; k < 4; k++) {
            Enabled[55 + k] = false;
            Enabled[59 + k] = true;
        }
    } else {
        for (int s = 17; s <= 22; s++) Parts[s] = Get(s, 0);
        Enabled[1] = Enabled[2] = Enabled[11] = Enabled[5] = Enabled[24] = Enabled[23] = true;
        Enabled[6] = false;
        for (int k = 0; k < 4; k++) {
            Enabled[55 + k] = true;
            Enabled[59 + k] = false;
        }
    }
}

} // namespace bb
