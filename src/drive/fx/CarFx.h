#pragma once

// The light and shadow effects of the test drive's car, as NFSU2 draws them in a race
// (NFSU2Unity docs/re/car-lights-shadow.md; port of Runtime/Fx/CarLightsFx.cs and CarShadowFx.cs):
//  - headlight and tail/brake light flares: CreateCarLightFlares 0x6204D0 (the markers), RenderFlares 0x61AE90
//    (the per-view table), RenderFlaresOnCar 0x615630 (intensities), eRenderLightFlare 0x490920 (the quads),
//    drawn in the flare pass with WORLDNODEPTH, additive, z write off;
//  - the shadow of CarRenderInfo::Render 0x621391..0x622EDE: the 4x4 grid textured with <BASE>_SHADOWIG laid on
//    the ground and the collision hull projected from the fixed light (0, 0, 2300) with SHADOWRAMP, both as
//    triangle strips, alpha blended, z write off, pulled 0.104 m toward the camera (sub_48AE30).
// The headlight projector on the world is bb::HeadlightProjector (HeadlightProjector.h).

#include "core/Math.h"
#include "drive/cars/CarLightFlaresU2.h"
#include "drive/cars/CarShadowU2.h"
#include "drive/fx/FxCommon.h"

#include <d3d9.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bb {

class BWareMath;
class CarModel;

class CarFx {
public:
    CarFx() = default;
    ~CarFx() { Release(); }
    CarFx(const CarFx&) = delete;
    CarFx& operator=(const CarFx&) = delete;

    // The flare list (CRI+0x3F0), headlight colour (CRI+0x380), parts box (CRI+0x10/+0x20), wheel positions,
    // collision hull and the car's shadow texture (CRI+0x33C) from a loaded car. False when the car has no layout.
    bool Init(IDirect3DDevice9* device, const CarModel& model);
    void Release();
    bool Ready() const { return ready_; }

    // Per-frame light state: Car+0x944 (brake lights: RealDriver brake > 0.05, sub_5F62B0) and Car+0x93D
    // (headlights: always on in game, sub_5F6190).
    void SetState(bool braking, bool headlightsOn) { braking_ = braking; headlightsOn_ = headlightsOn; }
    // RenderFlaresOnCar's force flags: 1 ForceHeadlightsOn, 2 ForceBrake (the game's debug switches
    // 0x8A1BF0 / 0x8A1BF4), 8 headlights off.
    void SetForceFlags(int flags) { forceFlags_ = flags; }

    uint32_t HeadlightColour() const { return headlightColour_; }
    size_t FlareCount() const { return flares_.size(); }
    const std::vector<LightFlareU2>& Flares() const { return flares_; }
    const CarShadowU2& Shadow() const { return shadow_; }
    std::string Describe() const;

    // g_CarShadowEnable: 0 off, 1 grid, 2 grid + hull (default).
    void SetShadowLevel(int level) { shadow_.ShadowLevel = level < 0 ? 0 : (level > 2 ? 2 : level); }

    // The shadow strips for the body matrix `body` (rows forward, left, up, position = Car+0x50) on `ground`.
    void DrawShadow(fx::StateGuard& guard, fx::PolyRenderer& polys, const fx::DeviceCamera& cam, const Mat4& body,
                    const ShadowElevation& ground, const BWareMath* math, fx::GlobalTextures& globals);

    // The headlight / brake light flares seen from `view`.
    void DrawFlares(fx::StateGuard& guard, fx::PolyRenderer& polys, const FlareViewU2& view, const Mat4& body,
                    fx::GlobalTextures& globals, int* quadCount = nullptr);

private:
    bool ready_ = false;
    IDirect3DDevice9* device_ = nullptr;
    std::vector<LightFlareU2> flares_;
    uint32_t headlightColour_ = 0x0090AA90;
    bool customSpoiler_ = false;
    int carType_ = 0;
    bool traffic_ = false;
    bool braking_ = false, headlightsOn_ = true;
    int forceFlags_ = 0;
    CarShadowU2 shadow_;
    fx::FxTexture shadowTexture_;     // <BASE>_SHADOWIG, owned
    std::string shadowTextureName_;
};

} // namespace bb
