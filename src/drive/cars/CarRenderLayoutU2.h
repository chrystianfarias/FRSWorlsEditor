#pragma once

// Port of NFSU2Unity Core/Cars/CarRenderLayoutU2.cs: what CarRenderInfo draws for a car and
// where. CarRenderInfo::UpdateCarParts (0x634800), UpdateWheelYRenderOffset (0x61FE10) and the
// part passes of CarRenderInfo::Render (0x620AE0). See docs/re/car-render-models.md.
//
// Addition for the editor's test drive: every wheel item also carries its matrix relative to the
// wheel-centre frame (Wheel/Local), so a live wheel matrix (spin + steer + suspension) can replace
// the static FE pose.

#include "core/Math.h"
#include "drive/cars/RideInfoU2.h"
#include "drive/geometry/SolidList.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bb {

// One solid to draw with its full game-space transform (row vectors, body included).
struct CarDrawItem {
    const Solid* Solid_ = nullptr;   // C# Solid
    Mat4 Matrix;
    int Slot = 0;
    int Row = 0;
    bool LeftBrake = false;          // left brakes swap BRAKE_GLOBAL for BRAKE_GLOBAL_LEFT
    std::string Label;

    // Editor addition: -1 for body parts; else the wheel (0 FL, 1 FR, 2 RR, 3 RL) the item follows,
    // with Local its matrix in that wheel's centre frame. Brakes ignore the wheel's spin.
    int Wheel = -1;
    Mat4 Local;
    bool Brake = false;
};

class CarRenderLayoutU2 {
public:
    static constexpr float WheelStandardWidth = 0.184f;          // 0x8026C4
    static constexpr float WheelPivotTranslationAmount = 0.5f;   // 0x8026C0
    static constexpr float BodyZBias = 0.01f;                    // flt_7A66C0
    static constexpr int NumModelSlots = 63;

    const RideInfoU2* Ride = nullptr;

    // Render options (defaults = front-end garage view, everything closed).
    bool FrontEnd = true;
    bool HoodOpen = false, TrunkOpen = false, LeftDoorOpen = false, RightDoorOpen = false;
    float SteerDegrees = 0;

    CarRenderLayoutU2(const RideInfoU2* ride, std::function<const Solid*(uint32_t)> findSolid);

    const Solid* Model(int slot, int row, int lod) const { return models_[slot][row][lod]; }

    // Static rest pose: body origin Z = mean(radius - RideHeight).
    float RestBodyZ() const;

    // Everything drawn at car LOD carLod / tyre LOD tireLod. atRest: the car resting on z = 0
    // (body origin at RestBodyZ + BodyZBias, as the C#); otherwise the body matrix is the identity.
    std::vector<CarDrawItem> Build(int carLod = 0, int tireLod = 0, bool atRest = true) const;

    // Body-space centre of wheel i's tyre (the frame Local is relative to): (X, Y -/+ width/2, RideHeight).
    Vec3 WheelCentre(int i) const;
    // Render width of wheel i (0.184 for stock rims).
    float WheelWidth(int i) const;

private:
    using AddFn = std::function<void(const Solid*, const Mat4&, int, int, const char*, bool)>;
    void BodyParts(int carLod, const Mat4& body, const AddFn& add) const;
    void MarkerParts(int carLod, const Mat4& body, const AddFn& add) const;
    void Wheels(int carLod, int tireLod, const Mat4& body, std::vector<CarDrawItem>& items) const;
    std::vector<Mat4> CollectMarkers(const uint32_t* hashes, int n) const;
    int Level(int slot) const;

    std::function<const Solid*(uint32_t)> findSolid_;
    const Solid* models_[NumModelSlots][2][4] = {};
};

} // namespace bb
