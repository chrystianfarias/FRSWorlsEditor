#pragma once

// Port of NFSU2Unity Core/Cars/CarFxSetupU2.cs: per-car data of the light and shadow effects, taken from the
// CarRenderInfo built for a ride (docs/re/car-lights-shadow.md): the flare list, the parts bounding box, the
// headlight colour and the collision hull used by the projected shadow.

#include "drive/cars/CarLightFlaresU2.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace bb {

class CarRenderLayoutU2;
class RideInfoU2;
class SolidList;

class CarFxSetupU2 {
public:
    // CarRenderInfo::CreateCarLightFlares 0x6204D0: every marker of Models[slot][0][0] (LOD A, row 0) of the
    // 63 slots named LEFT/RIGHT/CENTRE_HEADLIGHT (type 0) or LEFT/RIGHT/CENTRE_BRAKELIGHT (type 1), flags 4,
    // position = marker translation - body pivot (CRI+0x30 = eModel::GetPivotPosition 0x488EB0 of BODY LOD A =
    // solid+0x70, the pivot matrix translation), direction = marker matrix row 2. Reverse-light markers are not
    // accepted (no reverse flares in U2). The bStringHash("COPLIGHT_RED") at 0x620522 is dead code.
    static std::vector<LightFlareU2> CreateFlares(const CarRenderLayoutU2& layout);

    // CRI+0x10/+0x20 (CarRenderInfo::UpdateCarParts 0x634800, 0x634E85 and 0x634F09): AABBs of the solids
    // Models[slot][row][LOD A] for slots 0..7, 9..12 and 47..51, rows 0 and 1, then -/+ 0.1.
    static void PartsBoundingBox(const CarRenderLayoutU2& layout, float* min, float* max);

    // CRI+0x380 (CarRenderInfo::UpdateCarParts 0x635BE8..0x635D8B): (BLUE << 16) | (GREEN << 8) | RED
    // of the HEADLIGHT_BULB part (slot 159, ride+0x80C), else 0x0090AA90. Packed 0x00BBGGRR (byte 0 = red).
    static uint32_t HeadlightColour(const RideInfoU2& ride);

    // Spoiler level > 0 (CarPart+5 >> 5 of ride+0x5C0, RenderFlaresOnCar 0x6157FD) for
    // RemoveCentreBrakeWithCustomSpoiler.
    static bool HasCustomSpoiler(const RideInfoU2& ride);

    // Points of the car's collision hull for the projected shadow: the hull named by the BODY part's CV
    // attribute, else <CAR>_CV (inferred: Car+0x3C vfunc +0x7C(0) returns the GJK polytope built from it).
    static std::vector<std::vector<float>> HullPoints(const RideInfoU2& ride,
                                                      const std::vector<std::unique_ptr<SolidList>>& packs);
};

} // namespace bb
