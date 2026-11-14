#pragma once

// Port of NFSU2Unity Core/Cars/RideInfoU2.cs: the installed part per CAR_SLOT_ID (+0x590)
// and the per-slot enabled flags (+0x838). RideInfo::Init (0x610270), SetStockParts (0x637040),
// UpdatePartsEnabled (0x61BCD0) and SetPart (0x631D20).

#include "drive/cars/CarPartDatabaseU2.h"
#include "drive/cars/CarSlotsU2.h"
#include "drive/cars/CarTypeInfoU2.h"

#include <cstdint>

namespace bb {

class RideInfoU2 {
public:
    const CarPartDatabaseU2* Db = nullptr;
    const CarTypeInfoU2* CarType = nullptr;
    uint32_t CarNameHash = 0;
    int Parts[CarSlotsU2::NumSlots];        // part index per slot, -1 = none (+0x590)
    bool Enabled[CarSlotsU2::NumSlots];     // slot visible (+0x838)
    int HydraulicsLevel = 0;                // +0x581

    bool InGame = false;                    // makes the engine bay visible
    bool BrowsingBrakePaint = false;

    // db and carType must outlive the RideInfo.
    RideInfoU2(const CarPartDatabaseU2* db, const CarTypeInfoU2* carType, bool inGame = false);

    void SetStockParts(bool alternatePaint = false);
    int SetPart(int slot, int part);
    void UpdatePartsEnabled();

private:
    int Get(int slot, uint32_t partHash) const { return Db->NewGetCarPart(CarNameHash, slot, partHash, -1, -1); }
    void UpdateWideBody(int p);
};

} // namespace bb
