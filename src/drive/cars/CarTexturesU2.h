#pragma once

// Port of NFSU2Unity Core/Cars/CarTexturesU2.cs: runtime texture replacement of a car
// (eReplacementTextureTable). GetUsedCarTextureInfo (0x631E20), CarRenderInfo ctor (0x638190),
// SwitchSkin (0x636840), UpdateCarReplacementTextures (0x615280), UpdateLightStateTextures
// (0x60C940) and UpdateDecalTextures (0x61FF40). See docs/re/car-textures.md.

#include "drive/cars/RideInfoU2.h"

#include <cstdint>
#include <functional>
#include <string>

namespace bb {

class CarTexturesU2 {
public:
    struct Entry {
        uint32_t Old = 0, New = 0; // New == 0: inactive
    };

    static constexpr uint32_t DefaultAlpha = 0x66994F4A;    // DEFAULTALPHA
    static constexpr uint32_t BrakeGlobal = 0x17F9F794;     // BRAKE_GLOBAL
    static constexpr uint32_t BrakeGlobalLeft = 0x85E9C79E; // BRAKE_GLOBAL_LEFT

    const RideInfoU2* Ride = nullptr;
    Entry Main[73];            // CRI+0x638
    Entry Decals[7][8];        // CRI+0x13E8 (slots 52..58; 59..62 use 55..58)
    Entry BrakeLeft[2], BrakeRight[2];

    uint32_t CompositeSkinHash = 0, CompositeWheelHash = 0, CompositeSpinnerHash = 0;

    // compositeSlot: DUMMY_SKIN slot (1..6) or 0 for no composite (traffic).
    // frontEnd: the driver becomes DEFAULTALPHA.
    CarTexturesU2(const RideInfoU2* ride, int compositeSlot, bool frontEnd,
                  const std::function<bool(uint32_t)>& textureExists, const std::string& badgingSuffix = "");

    // Texture actually drawn for a solid's texture hash, given the part slot.
    uint32_t Resolve(uint32_t hash, int slot, bool brake = false, bool leftBrake = false) const;
};

} // namespace bb
